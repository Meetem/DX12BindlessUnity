#include "RenderAPI.h"
#include "PlatformBase.h"

#include <cmath>

// Direct3D 12 implementation of RenderAPI.

#if SUPPORT_D3D12
#include "RenderAPI_D3D12.h"
#include "UAL/UnityLog.h"

#include <unordered_map>
#include <unordered_set>
#include <concurrent_unordered_set.h>
#include <mutex>
#include <type_traits>

std::unordered_map<void*, void*> hookedFunctions = {};

concurrency::concurrent_unordered_set<void*> hookedCmdVTables;
static std::unordered_set<void*> pendingHookCmdVTables;
static std::mutex commandListInstallMutex;

// My data:
// {CAD4DE65-63E8-4CF6-B82C-6F7EE6776335}
static const GUID MeetemBindlessData =
{ 0xcad4de65, 0x63e8, 0x4cf6, { 0xb8, 0x2c, 0x6f, 0x7e, 0xe6, 0x77, 0x63, 0x35 } };

const uint32_t absoluteMaxDescriptors = 1000000u;
const uint32_t mainDescHeapMagic = 262144u;
const uint32_t srvBindlessDescriptorStart = 31u;
static bool isInitialized = false;
static RenderAPI_D3D12* myD3D12 = nullptr;

struct HookedRootSignature {
	unsigned descriptorId;
	unsigned numMaxBindings;
};

const unsigned NoBindless = 0;
class CommandListStateData {
protected:
	unsigned isInHookedCmpRootSig : 1;
	unsigned isInHookedGfxRootSig : 1;
public:
	unsigned assignedHookedHeap : 16;
protected:
	unsigned bindlessCmpSrvDescId;
	unsigned bindlessGfxSrvDescId;

public:
	unsigned getIsInHookedRootSig(bool gfx) const {
		return gfx ? isInHookedGfxRootSig : isInHookedCmpRootSig;
	}

	unsigned getSrvDescId(bool gfx) const {
		return gfx ? bindlessGfxSrvDescId : bindlessCmpSrvDescId;
	}

	void setIsInHookedRootSig(unsigned value, bool gfx) {
		if(gfx) { isInHookedGfxRootSig = value; } else { isInHookedCmpRootSig = value; };
	}

	void setSrvDescId(unsigned value, bool gfx) {
		if(gfx) { bindlessGfxSrvDescId = value; } else { bindlessCmpSrvDescId = value; };
	}
};

#define ReturnOnFail(x, hr, OnFailureMsg, onFailureReturnValue) hr = x; if(FAILED(hr)){OutputDebugStringA(OnFailureMsg); return onFailureReturnValue;}

static void handle_hr_fatal(HRESULT hr, const char* error = "")
{
	if (FAILED(hr))
	{
		UnityLog::LogError("D3D12 Call failed with error 0x%p\n", (void*)(size_t)hr);
		abort();
	}
}

_forceinline static void** GetVTableEntryPtrFromVT(void* vtableO, int vtableOffset) {
	return (void**)((BYTE*)vtableO + (vtableOffset));
}

template<typename T>
_forceinline static void** GetVTableEntryPtr(T* obj, int vtableOffset) {
	size_t* vtable = *(size_t**)obj;
	return GetVTableEntryPtrFromVT(vtable, vtableOffset);
}

static bool Unprotect(void* addr) {
	const uint64_t pageSize = 4096;

	DWORD oldProtect[2] = {};
	auto startPage1 = (void*)((((size_t)addr) / pageSize) * pageSize);
	auto startPage2 = (void*)((size_t)startPage1 + pageSize);

	auto protectResult = VirtualProtect(startPage1, pageSize, PAGE_READWRITE, &oldProtect[0]);
	protectResult &= VirtualProtect(startPage2, pageSize, PAGE_READWRITE, &oldProtect[1]);

	if (protectResult == 0) {
		UnityLog::LogError("VirtualProtect failed, result: %d, old protect: %p\n", protectResult, (void*)(size_t)oldProtect);
		return false;
	}

	return true;
}

#ifdef USE_MINHOOK
#include "MinHook/include/MinHook.h"
#include "TargetFunctionHook.h"

_forceinline static void* HookVT(void* vtablePtr, int vtableOffset, void* newFunction) {
	if (vtablePtr == nullptr) {
		UnityLog::LogError("HookVT called with nullptr vtable. Exiting.");
		__debugbreak();
		abort();
		return nullptr;
	}

	auto pptr = GetVTableEntryPtrFromVT(vtablePtr, vtableOffset);
	auto func = *pptr;

	auto foundTrampoline = hookedFunctions.find(func);
	if (foundTrampoline == hookedFunctions.end()) {
		void* orig = nullptr;
		auto createHookRes = MH_CreateHook(func, newFunction, &orig);
		if (createHookRes == MH_ERROR_ALREADY_CREATED) {
			return orig;
		}

		UnityLog::Debug("Hooking %p of %p\n", (void*)func, (void*)vtablePtr);

		if (createHookRes != MH_OK) {
			UnityLog::LogError("Can't install hook for %p = %s\n", func, MH_StatusToString(createHookRes));
			return func;
		}

		auto enableHookRes = MH_EnableHook(func);
		if (enableHookRes != MH_OK) {
			UnityLog::LogError("Can't enable hook for %p = %s\n", func, MH_StatusToString(enableHookRes));
			return func;
		}

		hookedFunctions[func] = orig;
		return orig;
	}

	return foundTrampoline->second;
}

template<typename T>
static void* Hook(T* obj, int vtableOffset, void* newFunction) {
	if(obj == nullptr)
		return nullptr;

	void* vtable = *(void**)obj;
	return HookVT(vtable, vtableOffset, newFunction);
}

#else
template<typename T>
static void* Hook(T* obj, int vtableOffset, void* newFunction)
{
	auto pptr = GetVTableEntryPtr<T>(obj, vtableOffset);
	auto old = *pptr;
	if (Unprotect(pptr)) {
		*pptr = newFunction;
	}

	return old;
}
#endif

#include "D3D12Hooks.h"
#include "DescriptorTableInjection.h"
#ifdef USE_MINHOOK
#define COMMAND_LIST_ORIGINAL(Name) D3D12_##Name Orig##Name,
#define COMMAND_LIST_TARGET(list, Name) TargetFunctionHook<D3D12_##Name>::Original(*GetVTableEntryPtr(list, __D3D12_VTOFFS_##Name))
#else
#define COMMAND_LIST_ORIGINAL(Name)
#define COMMAND_LIST_TARGET(list, Name) Orig##Name
#endif

static bool _InstallRealCmdlistHooks(void* vtablePtr);
static void _InstallCmdlistHooks(void* cmdList, const char* name);
RenderAPI* CreateRenderAPI_D3D12()
{
	auto obj = new RenderAPI_D3D12();
	std::lock_guard<std::mutex> lock(commandListInstallMutex);
	myD3D12 = obj;
	
	// Publish a vtable as installed only after all six hooks are ready.
	for (void* vtable : pendingHookCmdVTables) {
		if (_InstallRealCmdlistHooks(vtable))
			hookedCmdVTables.insert(vtable);
	}
	pendingHookCmdVTables.clear();
	return obj;
}

RenderAPI_D3D12::RenderAPI_D3D12() :
	s_d3d12(NULL),
	device(NULL),
	currentFrameBindlessOffset(0),
	lastList{}
{
}

static void FreeDeepCopy(D3D12_ROOT_SIGNATURE_DESC* p) {
	if (p->pParameters != nullptr) {
		free((void*)p->pParameters);
		p->pParameters = nullptr;
	}
}

static D3D12_ROOT_SIGNATURE_DESC DeepCopy(const D3D12_ROOT_SIGNATURE_DESC* original) {
	const unsigned MaxParams = 128;
	const unsigned MaxRangesPerTable = 64;

	uint8_t* mem = (uint8_t*)malloc(1024 * 1024);
	memset(mem, 0, 1024 * 1024);

	D3D12_ROOT_SIGNATURE_DESC o{};
	o.Flags = original->Flags;

	// Setup params
	o.NumParameters = original->NumParameters;
	o.pParameters = (D3D12_ROOT_PARAMETER*)mem;
	mem += sizeof(D3D12_ROOT_PARAMETER) * MaxParams;

	// Copy params
	auto outParams = (D3D12_ROOT_PARAMETER*)o.pParameters;
	for (unsigned pid = 0; pid < original->NumParameters; pid++) {
		D3D12_ROOT_PARAMETER pCopy = original->pParameters[pid];

		// Descriptor table, copy ranges.
		if (pCopy.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
			auto inputRanges = pCopy.DescriptorTable.pDescriptorRanges;
			auto outputRanges = (D3D12_DESCRIPTOR_RANGE*)mem;
			mem += sizeof(D3D12_DESCRIPTOR_RANGE) * MaxRangesPerTable;

			for (unsigned rid = 0; rid < pCopy.DescriptorTable.NumDescriptorRanges; rid++) {
				outputRanges[rid] = inputRanges[rid];
			}

			pCopy.DescriptorTable.pDescriptorRanges = outputRanges;
			outParams[pid] = pCopy;
		}
		// Straight copy.
		else {
			outParams[pid] = pCopy;
		}
	}

	// Copy samplers
	o.NumStaticSamplers = original->NumStaticSamplers;
	o.pStaticSamplers = (D3D12_STATIC_SAMPLER_DESC*)mem;
	for (unsigned sid = 0; sid < original->NumStaticSamplers; sid++) {
		((D3D12_STATIC_SAMPLER_DESC*)o.pStaticSamplers)[sid] = original->pStaticSamplers[sid];
	}

	return o;
}

static inline CommandListStateData GetCommandListState(ID3D12CommandList* cmdList) {
	if (cmdList == nullptr) {
		return {};
	}

	UINT dsize = sizeof(CommandListStateData);
	CommandListStateData ret;
	auto res = cmdList->GetPrivateData(MeetemBindlessData, &dsize, &ret);
	if ((FAILED(res)) || (dsize != sizeof(CommandListStateData)))
		return {};

	return ret;
}

static inline bool SetCommandListState(ID3D12CommandList* cmdList, CommandListStateData state) {
	if (cmdList == nullptr) {
		return true;
	}

	UINT dsize = sizeof(CommandListStateData);
	auto res = cmdList->SetPrivateData(MeetemBindlessData, dsize, &state);
	return !FAILED(res);
}

template<class T>
static inline bool TryGetBindlessData(ID3D12Object* iface, T& outputSig) {
	if (iface == nullptr) {
		outputSig = {};
		return false;
	}

	UINT dsize = sizeof(T);
	auto res = iface->GetPrivateData(MeetemBindlessData, &dsize, &outputSig);
	return !FAILED(res) && dsize == sizeof(T);
}


extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(
		ID3D12Device* This,
		_In_  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
		REFIID riid,
		_COM_Outptr_  void** ppPipelineState
)
{
	HookedRootSignature data;
	auto hasHookedRootsig = false;

	UnityLog::Debug("CreateGraphicsPipelineState creating with root sig %p\n", pDesc->pRootSignature);

	if (pDesc->pRootSignature != nullptr) {
		hasHookedRootsig = TryGetBindlessData(pDesc->pRootSignature, data);
		if (hasHookedRootsig) {
			UnityLog::Debug("CreateGraphicsPipelineState found using the hooked root signature\n");
		}
	}

	auto res = OrigCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
	if (!FAILED(res) && hasHookedRootsig) {
		ID3D12PipelineState* state = (ID3D12PipelineState*)*ppPipelineState;
		state->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &data);
	}

	return res;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
	ID3D12Device* This,
	_In_  const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
	REFIID riid,
	_COM_Outptr_  void** ppPipelineState)
{
	HookedRootSignature data;
	auto hasHookedRootsig = false;

	if (pDesc->pRootSignature != nullptr) {
		hasHookedRootsig = TryGetBindlessData(pDesc->pRootSignature, data);
		if (hasHookedRootsig) {
			UnityLog::Debug("CreateComputePipelineState found using the hooked root signature\n");
		}
	}

	auto res = OrigCreateComputePipelineState(This, pDesc, riid, ppPipelineState);
	if (!FAILED(res) && hasHookedRootsig) {
		ID3D12PipelineState* state = (ID3D12PipelineState*)*ppPipelineState;
		state->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &data);
	}

	return res;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(COMMAND_LIST_ORIGINAL(Reset) 
	ID3D12GraphicsCommandList1* This,
	_In_  ID3D12CommandAllocator* pAllocator,
	_In_opt_  ID3D12PipelineState* pInitialState
)
{
	auto result = OrigReset(This, pAllocator, pInitialState);
	if (SUCCEEDED(result)) {
		SetCommandListState(This, {});
		// Reset may replace the vtable and its recording entry points.
		// Install before Unity starts recording, rather than at submission.
		_InstallCmdlistHooks(This, "Reset");
	}
	return result;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
	ID3D12Device* This,
	_In_  UINT nodeMask,
	_In_reads_(blobLengthInBytes)  const void* pBlobWithRootSignature,
	_In_  SIZE_T blobLengthInBytes,
	REFIID riid,
	_COM_Outptr_  void** ppvRootSignature) {

	ID3D12RootSignatureDeserializer* deserializer;
	auto hr = D3D12CreateRootSignatureDeserializer(
		pBlobWithRootSignature, blobLengthInBytes, IID_PPV_ARGS(&deserializer));

	if (FAILED(hr)) {
		UnityLog::LogError("Can't create deserializer for root signature\n");
		return OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
	}

	auto rootSig = DeepCopy(deserializer->GetRootSignatureDesc());
	deserializer->Release();

	bool needPlaceSrv = false;
	bool ignore = false;

	std::vector<D3D12_DESCRIPTOR_RANGE> newRanges{};
	newRanges.reserve(8192);

	auto writableParams = ((D3D12_ROOT_PARAMETER*)(rootSig.pParameters));
	for (unsigned i = 0; i < rootSig.NumParameters; i++) {
		auto& p = writableParams[i];

		// We are only interested in descriptor tables
		// As only there is possible to use bindless.
		if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
			continue;
		}

		UnityLog::Debug("Param: %d, type: %d, shader vis: %d\n", i, p.ParameterType, p.ShaderVisibility);

		// For computes
		if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL 
			|| p.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL
			//|| p.ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX
		)
		{
			auto& v = p.DescriptorTable;
			UnityLog::Debug("Descriptor Table: %d, num ranges %d\n", i, v.NumDescriptorRanges);

			//auto startIndex = newRanges.size();
			newRanges.clear();

			auto writableDescriptors = (D3D12_DESCRIPTOR_RANGE*)v.pDescriptorRanges;
			for (unsigned x = 0; x < v.NumDescriptorRanges; x++) {
				auto& dr = writableDescriptors[x];

				UnityLog::Debug("DescriptorTable: %d, type: %d, base register: %d, numDescriptors: %d, offset: %d\n",
					x, dr.RangeType, dr.BaseShaderRegister, dr.NumDescriptors, dr.OffsetInDescriptorsFromTableStart);

				if (dr.RegisterSpace != 0 || dr.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
					newRanges.push_back(dr);
					continue;
				}

				if (dr.BaseShaderRegister + dr.NumDescriptors != (srvBindlessDescriptorStart + 1)) {
					if (dr.BaseShaderRegister + dr.NumDescriptors > (srvBindlessDescriptorStart + 1)) {
						ignore = true;

						UnityLog::LogWarning("Some shader uses more than %d texture registers, this shader is not eligible for bindless.", (srvBindlessDescriptorStart + 1));
						UnityLog::LogWarning("DescriptorTable: %d, type: %d, base register: %d, numDescriptors: %d, offset: %d\n",
							x, dr.RangeType, dr.BaseShaderRegister, dr.NumDescriptors, dr.OffsetInDescriptorsFromTableStart);
					}

					newRanges.push_back(dr);
					continue;
				}

				if (ignore) {
					needPlaceSrv = false;
					newRanges.push_back(dr);
					continue;
				}

				// Past this list we are only working with descriptors
				// which have (srvBindlessDescriptorStart + 1) entries.
				needPlaceSrv = true;

				if (dr.NumDescriptors == 1) {
					// Just remove it.
					UnityLog::Log("Removing descriptor range\n");
					continue;
				}

				UnityLog::Log("Modified descriptor range.\n");
				dr.NumDescriptors--;
				newRanges.push_back(dr);
			}

			// Copy the list.
			v.NumDescriptorRanges = newRanges.size();
			for (unsigned k = 0; k < newRanges.size(); k++) {
				writableDescriptors[k] = newRanges.at(k);
			}
		}
	}

	if (ignore || !needPlaceSrv) {
		FreeDeepCopy(&rootSig);
		auto ret = OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
		UnityLog::Debug("Created root desc [n] %p, %p\n", *ppvRootSignature, (void*)(size_t)(ret));
		
		return ret;
	}

	D3D12_ROOT_PARAMETER p = {};
	p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// This must survive stack pop, so declared here.
	D3D12_DESCRIPTOR_RANGE newRange{};
	newRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	newRange.NumDescriptors = myD3D12->numAdditionalSrv();
	newRange.BaseShaderRegister = srvBindlessDescriptorStart;
	newRange.RegisterSpace = 0;
	newRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	p.DescriptorTable.NumDescriptorRanges = 1;
	p.DescriptorTable.pDescriptorRanges = &newRange;

	// Append to the end.
	writableParams[rootSig.NumParameters] = p;
	rootSig.NumParameters++;

	HookedRootSignature hookedValue{};
	hookedValue.descriptorId = rootSig.NumParameters - 1;
	hookedValue.numMaxBindings = myD3D12->numAdditionalSrv();

	UnityLog::Debug("Adding new descriptor table, new num: %d\n", rootSig.NumParameters);

	ID3DBlob* serializedBlob = nullptr;
	ID3DBlob* errorBlob = nullptr;

	auto serializeResult = D3D12SerializeRootSignature(&rootSig, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedBlob, &errorBlob);
	if (FAILED(serializeResult)) {
		UnityLog::LogError("Failed to serialize new root signature %p.\n", (void*)(size_t)serializeResult);

		// Null terminate if needed.
		if (errorBlob != nullptr && errorBlob->GetBufferSize() > 0) {
			auto sz = errorBlob->GetBufferSize() - 1;
			auto ptr = (char*)errorBlob->GetBufferPointer();
			if (ptr[sz] != 0)
				ptr[sz] = 0;

			UnityLog::LogError("ErrorBlob: %s\n", errorBlob->GetBufferPointer());
			errorBlob->Release();
		}

		// Return unmodified.
		auto ret = OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
		UnityLog::Debug("Created root desc [f] %p\n", *ppvRootSignature);
		return ret;
	}

	auto ret = OrigCreateRootSignature(This, nodeMask, serializedBlob->GetBufferPointer(), serializedBlob->GetBufferSize(), riid, ppvRootSignature);
	serializedBlob->Release();

	if (needPlaceSrv && !FAILED(ret) && (*ppvRootSignature) != nullptr) {
		ID3D12RootSignature* sig = (ID3D12RootSignature*)*ppvRootSignature;
		UnityLog::Debug("Created root desc [s] %p\n", *ppvRootSignature);

		// Set via private data
		if (FAILED(sig->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &hookedValue))) {
			UnityLog::LogError("Can't set private data\n");
			abort();
		}
	}

	FreeDeepCopy(&rootSig);
	return ret;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device * device, _In_  D3D12_DESCRIPTOR_HEAP_DESC * pDescriptorHeapDesc, REFIID riid, _COM_Outptr_  void** ppvHeap) 
{
	D3D12_DESCRIPTOR_HEAP_DESC pDescCopy = *pDescriptorHeapDesc;
	UnityLog::Debug("Creating descriptor heap: %d, elements: %d\n", pDescCopy.Type, pDescCopy.NumDescriptors);

	if (pDescCopy.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
		uint32_t additional = RenderAPI_D3D12::numAdditionalSrvTotal();
		bool hooked = false;

		if (pDescCopy.NumDescriptors >= mainDescHeapMagic) {
			hooked = true;
			if (pDescCopy.NumDescriptors + additional > absoluteMaxDescriptors) {
				pDescCopy.NumDescriptors = absoluteMaxDescriptors;
				myD3D12->srvBaseOffset = pDescCopy.NumDescriptors - additional;
			}
			else {
				myD3D12->srvBaseOffset = pDescCopy.NumDescriptors;
				pDescCopy.NumDescriptors += additional;
			}
		}

		UnityLog::Log("Creating CBV/SRV/UAV descriptor set with count %d with type %d (%d additional entries)\n", (int)pDescCopy.NumDescriptors, (int)pDescCopy.Type, additional);

		auto res = OrigCreateDescriptorHeap(device, &pDescCopy, riid, ppvHeap);
		if (FAILED(res) || ppvHeap == nullptr) {
			UnityLog::LogError("Can't create new descriptor heap: %p\n", (void*)(size_t)res);
		}
		else if (ppvHeap != nullptr) {
			auto ptr = (ID3D12DescriptorHeap*)*ppvHeap;
			if (ptr != nullptr) {
				myD3D12->srvDescriptorHeaps.push_back(ptr);

				if (hooked) {
					myD3D12->hookedDescriptorHeaps.push_back(ptr);
				}
			}
		} else {
			UnityLog::LogError("Can't create new descriptor heap, reason unknown.\n");
		}

		return res;
	}

	return OrigCreateDescriptorHeap(device, &pDescCopy, riid, ppvHeap);
}

template<bool gfx>
static void BindDescriptorTable(ID3D12CommandList* list, const CommandListStateData& dt,
	std::conditional_t<gfx, D3D12_SetGraphicsRootDescriptorTable, D3D12_SetComputeRootDescriptorTable> original = nullptr)
{
	auto srvId = dt.getSrvDescId(gfx);
	if (!dt.getIsInHookedRootSig(gfx) || !dt.assignedHookedHeap || srvId == NoBindless)
		return;

	if (myD3D12->hookedDescriptorHeaps.empty()) {
		UnityLog::LogWarning("Set*RootDescriptorTable is called, but no srvHeap is set.\n");
		return;
	}

	const UINT targetIdx = srvId - 1;
	auto heap = myD3D12->hookedDescriptorHeaps[dt.assignedHookedHeap - 1];
	CD3DX12_GPU_DESCRIPTOR_HANDLE h(heap->GetGPUDescriptorHandleForHeapStart());
	h.Offset((myD3D12->srvBaseOffset + myD3D12->getCurrentOffset()) * myD3D12->srvIncrement);

	DescriptorTableInjection injection;
	if constexpr (gfx) {
		if (original == nullptr)
			original = COMMAND_LIST_TARGET(list, SetGraphicsRootDescriptorTable);
		original((ID3D12GraphicsCommandList*)list, targetIdx, h);
	} else {
		if (original == nullptr)
			original = COMMAND_LIST_TARGET(list, SetComputeRootDescriptorTable);
		original(list, targetIdx, h);
	}
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(COMMAND_LIST_ORIGINAL(SetGraphicsRootSignature) ID3D12GraphicsCommandList* This,
	_In_opt_  ID3D12RootSignature* pRootSignature) 
{
	OrigSetGraphicsRootSignature(This, pRootSignature);

	const bool isGfx = true;
	HookedRootSignature d{};
	auto hasDescHook = TryGetBindlessData(pRootSignature, d);
	auto dt = GetCommandListState(This);

	if (hasDescHook)
	{
		dt.setIsInHookedRootSig(true, isGfx);
		dt.setSrvDescId(d.descriptorId + 1, isGfx);
		BindDescriptorTable<isGfx>(This, dt);
	} 
	else
	{
		dt.setIsInHookedRootSig(false, isGfx);
		dt.setSrvDescId(NoBindless, isGfx);
	}

	SetCommandListState(This, dt);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(COMMAND_LIST_ORIGINAL(SetComputeRootSignature) ID3D12GraphicsCommandList* This,
	_In_opt_  ID3D12RootSignature* pRootSignature) 
{
	OrigSetComputeRootSignature(This, pRootSignature);

	const bool isGfx = false;
	HookedRootSignature d{};
	auto hasDescHook = TryGetBindlessData(pRootSignature, d);
	auto dt = GetCommandListState(This);

	if (hasDescHook)
	{
		dt.setIsInHookedRootSig(true, isGfx);
		dt.setSrvDescId(d.descriptorId + 1, isGfx);
		BindDescriptorTable<isGfx>(This, dt);
	} else
	{
		dt.setIsInHookedRootSig(false, isGfx);
		dt.setSrvDescId(NoBindless, isGfx);
	}

	SetCommandListState(This, dt);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(COMMAND_LIST_ORIGINAL(SetDescriptorHeaps) ID3D12GraphicsCommandList* This,
	_In_  UINT NumDescriptorHeaps,
	_In_reads_(NumDescriptorHeaps)  ID3D12DescriptorHeap* const* ppDescriptorHeaps) {
	
	auto dt = GetCommandListState(This);

	if (myD3D12->hookedDescriptorHeaps.empty())
	{
		OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}

	if (ppDescriptorHeaps == nullptr || NumDescriptorHeaps == 0)
	{
		OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);

		dt.assignedHookedHeap = 0;
		SetCommandListState(This, dt);
		return;
	}

	ID3D12DescriptorHeap* heaps[128];

	if (NumDescriptorHeaps >= _countof(heaps))
	{
		OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}

	unsigned assigned = 0;
	bool hasSrvHeap = false;

	const auto& srvHeaps = myD3D12->srvDescriptorHeaps;
	const auto& hookedHeaps = myD3D12->hookedDescriptorHeaps;

	for (UINT i = 0; i < NumDescriptorHeaps; ++i)
	{
		ID3D12DescriptorHeap* heap = ppDescriptorHeaps[i];
		heaps[i] = heap;

		if (heap == nullptr)
			continue;

		for (auto v : srvHeaps)
		{
			if (v == heap)
			{
				hasSrvHeap = true;
				break;
			}
		}

		for (unsigned k = 0; k < hookedHeaps.size(); ++k)
		{
			if (hookedHeaps[k] == heap)
			{
				assigned = k + 1;
				break;
			}
		}
	}

	// No CBV/SRV/UAV heap was supplied at all, so append ours.
	// If Unity supplied a different CBV/SRV/UAV heap, we CANNOT append
	// another one of the same type.
	if (!assigned && !hasSrvHeap)
	{
		heaps[NumDescriptorHeaps++] = hookedHeaps[0];
		assigned = 1;
	}

	// SetDescriptorHeaps invalidates descriptor-table bindings.
	OrigSetDescriptorHeaps(
		This,
		NumDescriptorHeaps,
		heaps);

	dt.assignedHookedHeap = assigned;

	SetCommandListState(This, dt);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(COMMAND_LIST_ORIGINAL(SetComputeRootDescriptorTable) ID3D12CommandList* list,
	_In_  UINT RootParameterIndex,
	_In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	ForwardDescriptorTable(
		[&] { OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor); },
		[&] {
			auto dt = GetCommandListState(list);
			BindDescriptorTable<false>(list, dt, OrigSetComputeRootDescriptorTable);
		});
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(COMMAND_LIST_ORIGINAL(SetGraphicsRootDescriptorTable) ID3D12GraphicsCommandList* list,
	_In_  UINT RootParameterIndex,
	_In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	ForwardDescriptorTable(
		[&] { OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor); },
		[&] {
			auto dt = GetCommandListState(list);
			BindDescriptorTable<true>(list, dt, OrigSetGraphicsRootDescriptorTable);
		});
}


#ifdef USE_MINHOOK
#define HookCommandListVtable(vtable, Name) do { \
    void* target = *GetVTableEntryPtrFromVT(vtable, __D3D12_VTOFFS_##Name); \
    auto status = TargetFunctionHook<D3D12_##Name>::Install(target, Hooked_##Name); \
    if (status != MH_OK) { \
        UnityLog::LogError("Can't hook " #Name " at %p: %s\n", target, MH_StatusToString(status)); \
        return false; \
    } \
} while (false)
#else
#define HookCommandListVtable(vtable, Name) HookVtableFunc(vtable, Name)
#endif
static bool _InstallRealCmdlistHooks(void* vtablePtr)
{
	HookCommandListVtable(vtablePtr, SetDescriptorHeaps);
	HookCommandListVtable(vtablePtr, Reset);

	HookCommandListVtable(vtablePtr, SetComputeRootDescriptorTable);
	HookCommandListVtable(vtablePtr, SetComputeRootSignature);

	HookCommandListVtable(vtablePtr, SetGraphicsRootDescriptorTable);
	HookCommandListVtable(vtablePtr, SetGraphicsRootSignature);
	return true;
}

static void _InstallCmdlistHooks(void* cmdList, const char* name) {
	if(cmdList == nullptr || ((ID3D12CommandList*)cmdList)->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
		return;

	void* vtablePtr = *(void**)cmdList;
	if(vtablePtr == nullptr)
		return;

	// The common Reset/submission path only reads the completed set. Unlike
	// insert-before-install, another thread cannot observe a half-hooked table.
	if (hookedCmdVTables.find(vtablePtr) != hookedCmdVTables.end())
		return;

	std::lock_guard<std::mutex> lock(commandListInstallMutex);
	if (hookedCmdVTables.find(vtablePtr) != hookedCmdVTables.end())
		return;
	if (myD3D12 == nullptr) {
		pendingHookCmdVTables.insert(vtablePtr);
		return;
	}
	if (_InstallRealCmdlistHooks(vtablePtr)) {
		hookedCmdVTables.insert(vtablePtr);
		UnityLog::Debug("Hooked CommandList vtable=%p via %s\n", vtablePtr, name);
	}
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList(
#ifdef USE_MINHOOK
	D3D12_CreateCommandList original,
#endif
	ID3D12Device* This,
	_In_  UINT nodeMask,
	_In_  D3D12_COMMAND_LIST_TYPE type,
	_In_  ID3D12CommandAllocator* pCommandAllocator,
	_In_opt_  ID3D12PipelineState* pInitialState,
	REFIID riid,
	_COM_Outptr_  void** ppCommandList) 
{

#ifdef USE_MINHOOK
	auto result = original(This, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
#else
	auto result = OrigCreateCommandList(This, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
#endif
	if (ppCommandList != nullptr && type == D3D12_COMMAND_LIST_TYPE_DIRECT && !FAILED(result)) {
		_InstallCmdlistHooks(ppCommandList[0], "CreateCommandList0");
		SetCommandListState((ID3D12CommandList*)ppCommandList[0], {});
	}

	return result;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList1(
#ifdef USE_MINHOOK
	D3D12_CreateCommandList1 original,
#endif
	ID3D12Device4* This,
	_In_  UINT nodeMask,
	_In_  D3D12_COMMAND_LIST_TYPE type,
	_In_  D3D12_COMMAND_LIST_FLAGS flags,
	REFIID riid,
	_COM_Outptr_  void** ppCommandList) 
{

#ifdef USE_MINHOOK
	auto result = original(This, nodeMask, type, flags, riid, ppCommandList);
#else
	auto result = OrigCreateCommandList1(This, nodeMask, type, flags, riid, ppCommandList);
#endif
	if (ppCommandList != nullptr && type == D3D12_COMMAND_LIST_TYPE_DIRECT && !FAILED(result)) {
		_InstallCmdlistHooks(ppCommandList[0], "CreateCommandList1");
		SetCommandListState((ID3D12CommandList*)ppCommandList[0], {});
	}

	return result;
}

extern "C" static void STDMETHODCALLTYPE Hooked_ExecuteCommandLists(
	ID3D12CommandQueue* This,
	_In_  UINT NumCommandLists,
	_In_reads_(NumCommandLists)  ID3D12CommandList* const* ppCommandLists)
{
	// Lists can reach the queue without passing through the hooked
	// CreateCommandList/CreateCommandList1, so make sure they are hooked here.
	if (ppCommandLists != nullptr) {
		for (UINT i = 0; i < NumCommandLists; i++) {
			_InstallCmdlistHooks(ppCommandLists[i], "ExecuteCommandLists");
		}
	}

	OrigExecuteCommandLists(This, NumCommandLists, ppCommandLists);
}

#ifdef USE_MINHOOK
template<typename Function, typename Device, typename Handler>
static void InstallCreationHook(Device* device, unsigned offset, Handler handler) {
	void* target = *GetVTableEntryPtr(device, offset);
	auto status = TargetFunctionHook<Function>::Install(target, handler);
	if (status != MH_OK) {
		UnityLog::LogError("Can't install command-list creation hook for %p: %s\n", target, MH_StatusToString(status));
	} else {
		UnityLog::Debug("Command-list creation hook ready for %p\n", target);
	}
}
#define HookCreationFunc(obj, Name) InstallCreationHook<D3D12_##Name>(obj, __D3D12_VTOFFS_##Name, Hooked_##Name)
#else
#define HookCreationFunc(obj, Name) HookGenericFunc(obj, Name)
#endif

void InstallEarlyD3D12Hooks()
{
	static bool s_earlyHooksInstalled = false;
	if (s_earlyHooksInstalled)
		return;

	UnityLog::Debug("Installing Early Hooks");

	__D3D12HOOKS_InitializeD3D12Offsets();

	HMODULE d3d12Module = GetModuleHandleA("d3d12.dll");
	if (d3d12Module == nullptr) {
		UnityLog::LogWarning("d3d12.dll is not loaded yet, early hooks were not installed.\n");
		return;
	}

	auto createDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12Module, "D3D12CreateDevice");
	if (createDevice == nullptr) {
		UnityLog::LogError("Can't find D3D12CreateDevice export in d3d12.dll\n");
		return;
	}

	ID3D12Device* tempDevice = nullptr;
	HRESULT hr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tempDevice));
	if (FAILED(hr) || tempDevice == nullptr) {
		UnityLog::LogError("Can't create a temporary D3D12 device for early hooking, hr = 0x%p\n", (void*)(size_t)hr);
		return;
	}

	// MinHook patches the function code itself, so hooking through a temporary
	// device covers every device sharing this d3d12.dll, including the one
	// D3D11On12 creates internally before Unity's device event fires.
	HookCreationFunc(tempDevice, CreateCommandList);

	ID3D12Device4* tempDevice4 = nullptr;
	if (!FAILED(tempDevice->QueryInterface(IID_PPV_ARGS(&tempDevice4))) && tempDevice4 != nullptr) {
		HookCreationFunc(tempDevice4, CreateCommandList1);
		tempDevice4->Release();
	}

	tempDevice->Release();
	s_earlyHooksInstalled = true;
	UnityLog::Debug("Installed Early Hooks");
}

extern unsigned __api_call_counter;

int RenderAPI_D3D12::SetBindlessTextures(int offset, unsigned numTextures, BindlessTexture* textures
) {
	if (!isInitialized) {
		UnityLog::LogError("Plugin is not initialized, try restart Unity Editor\n");
		return 0;
	}

	if (myD3D12->hookedDescriptorHeaps.empty()) {
		UnityLog::LogError("SetBindlessTextures is called, but no srvHeap is set.\n");
		return 0;
	}

	// TODO: Optimize;
	// TODO: Check for changes
	const auto& heaps = this->hookedDescriptorHeaps;
	for (auto heap : heaps) {
		CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(heap->GetCPUDescriptorHandleForHeapStart());
		cpuHandle.Offset(this->srvIncrement * this->srvBaseOffset);
		cpuHandle.Offset(this->srvIncrement * offset);

		for (unsigned i = 0; i < numTextures; i++) {
			auto t = textures[i];

			if (t.type == BindlessTextureType::None) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MostDetailedMip = 0;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Texture2D.PlaneSlice = 0;
				srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

				// Empty resource
				device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
				goto next;
			}

			if (t.type == BindlessTextureType::Resource) {
				auto texResource = (ID3D12Resource*)t.handle;
				auto desc = texResource->GetDesc();

				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Format = t.forceFormat != 0 ? (DXGI_FORMAT)t.forceFormat : typeless_fmt_to_typed(desc.Format);
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MostDetailedMip = t.minMip;
				srvDesc.Texture2D.MipLevels = t.maxMip == 255u ? desc.MipLevels : (t.maxMip - t.minMip);
				srvDesc.Texture2D.PlaneSlice = 0;
				srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

				device->CreateShaderResourceView(texResource, &srvDesc, cpuHandle);
				goto next;
			}

			if (t.type == BindlessTextureType::SRV) {
				UnityLog::LogWarning("Raw SRVs are not supported on D3D12\n");
				goto next;
			}

		next:
			cpuHandle.Offset(this->srvIncrement);
		}
	}

	return 1;
}

void RenderAPI_D3D12::HookSetFunctions() {
	
}

void RenderAPI_D3D12::SetCurrentBindlessOffset(void* eventData) {
	currentFrameBindlessOffset = (int)eventData;
}

void RenderAPI_D3D12::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
	ID3D12Device* dev = nullptr;
	void** vtableEntryPtr = nullptr;

	if (type == kUnityGfxDeviceEventInitialize) {
		UnityLog::Log("Initializing D3D12");
		__D3D12HOOKS_InitializeD3D12Offsets();

		this->s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v7>();
		this->device = s_d3d12->GetDevice();

		UnityD3D12PluginEventConfig config_second;
		config_second.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
		config_second.flags = kUnityD3D12EventConfigFlag_SyncWorkerThreads | kUnityD3D12EventConfigFlag_EnsurePreviousFrameSubmission | kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
		config_second.ensureActiveRenderTextureIsBound = false;
		s_d3d12->ConfigureEvent(2147473649, &config_second);

		ID3D12Device4* device4 = nullptr;
		
		if (!FAILED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
			UnityLog::Log("Supports ID3D12Device4\n");
			HookCreationFunc(device4, CreateCommandList1);
		} else {
			device4 = nullptr;
		}

		HookCreationFunc(device, CreateCommandList);

		HookDeviceFunc(CreateDescriptorHeap);
		HookDeviceFunc(CreateRootSignature);
		HookDeviceFunc(CreateComputePipelineState);
		HookDeviceFunc(CreateGraphicsPipelineState);

		ID3D12CommandQueue* cmdQueue = s_d3d12->GetCommandQueue();
		if (cmdQueue != nullptr) {
			HookGenericFunc(cmdQueue, ExecuteCommandLists);
		}

		srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		initialize_and_create_resources();
		isInitialized = true;

		if (device4 != nullptr) {
			device4->Release();
		}

		return;
	}

	if (!isInitialized)
		return;

	if (type == kUnityGfxDeviceEventShutdown) {
		release_resources();
		return;
	}
}

static bool initialized = false;
static bool disabled = false;

void RenderAPI_D3D12::initialize_and_create_resources()
{
	ID3D12Device* device = s_d3d12->GetDevice();
	assert(device != nullptr);
}

void RenderAPI_D3D12::release_resources()
{

}

void RenderAPI_D3D12::wait_for_unity_frame_fence(UINT64 fence_value)
{
	/*
	ID3D12Fence* unity_fence = s_d3d12->GetFrameFence();
	UINT64 current_fence_value = unity_fence->GetCompletedValue();

	if (current_fence_value < fence_value)
	{
		handle_hr_fatal(unity_fence->SetEventOnCompletion(fence_value, m_fence_event), "Failed to set fence event on completion\n");
		WaitForSingleObject(m_fence_event, INFINITE);
	}
	*/
}

void RenderAPI_D3D12::wait_on_fence(UINT64 fence_value, ID3D12Fence* fence, HANDLE fence_event)
{
	/*
	UINT64 current_fence_value = fence->GetCompletedValue();

	if (current_fence_value < fence_value)
	{
		handle_hr_fatal(fence->SetEventOnCompletion(fence_value, fence_event));
		WaitForSingleObject(fence_event, INFINITE);
	}
	*/
}

DXGI_FORMAT RenderAPI_D3D12::typeless_fmt_to_typed(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return DXGI_FORMAT_R32G32B32A32_UINT;

	case DXGI_FORMAT_R32G32B32_TYPELESS:
		return DXGI_FORMAT_R32G32B32_UINT;

	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_UNORM;

	case DXGI_FORMAT_R32G32_TYPELESS:
		return DXGI_FORMAT_R32G32_UINT;

	case DXGI_FORMAT_R32G8X24_TYPELESS:
		return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return DXGI_FORMAT_R8G8B8A8_UNORM;

	case DXGI_FORMAT_R16G16_TYPELESS:
		return DXGI_FORMAT_R16G16_UNORM;

	case DXGI_FORMAT_R32_TYPELESS:
		return DXGI_FORMAT_R32_UINT;

	case DXGI_FORMAT_R24G8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

	case DXGI_FORMAT_R8G8_TYPELESS:
		return DXGI_FORMAT_R8G8_UNORM;

	case DXGI_FORMAT_R16_TYPELESS:
		return DXGI_FORMAT_R16_UNORM;

	case DXGI_FORMAT_R8_TYPELESS:
		return DXGI_FORMAT_R8_UNORM;

	case DXGI_FORMAT_BC1_TYPELESS:
		return DXGI_FORMAT_BC1_UNORM;

	case DXGI_FORMAT_BC2_TYPELESS:
		return DXGI_FORMAT_BC2_UNORM;

	case DXGI_FORMAT_BC3_TYPELESS:
		return DXGI_FORMAT_BC3_UNORM;

	case DXGI_FORMAT_BC4_TYPELESS:
		return DXGI_FORMAT_BC4_UNORM;

	case DXGI_FORMAT_BC5_TYPELESS:
		return DXGI_FORMAT_BC5_UNORM;

	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

	case DXGI_FORMAT_BC6H_TYPELESS:
		return DXGI_FORMAT_BC6H_UF16;

	case DXGI_FORMAT_BC7_TYPELESS:
		return DXGI_FORMAT_BC7_UNORM;

	default:
		return format;
	}
}

#undef ReturnOnFail

#endif // #if SUPPORT_D3D12
