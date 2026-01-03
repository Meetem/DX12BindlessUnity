#include "RenderAPI.h"
#include "PlatformBase.h"

#include <cmath>

// Direct3D 12 implementation of RenderAPI.

#if SUPPORT_D3D12
#include "RenderAPI_D3D12.h"
#include "UAL/UnityLog.h"

#include <unordered_map>

// My data:
// {CAD4DE65-63E8-4CF6-B82C-6F7EE6776333}
static const GUID MeetemBindlessData =
{ 0xcad4de65, 0x63e8, 0x4cf6, { 0xb8, 0x2c, 0x6f, 0x7e, 0xe6, 0x77, 0x63, 0x33 } };

const uint32_t absoluteMaxDescriptors = 1000000u;
const uint32_t mainDescHeapMagic = 262144u;
const uint32_t srvBindlessDescriptorStart = 31u;
static bool isInitialized = false;
static RenderAPI_D3D12* myD3D12 = nullptr;

struct HookedRootSignature {
	unsigned descriptorId;
	unsigned numMaxBindings;
};

enum CmdListHookedPipeline {
	CmdListHookedPipeline_Unset = 0,
	CmdListHookedPipeline_Set = 1,
};

const unsigned NoBindless = 0;
struct CommandListStateData {
	unsigned isInHookedCmpRootSig : 1;
	unsigned isInHookedGfxRootSig : 1;
	unsigned isHookedCmpDescSetAssigned : 1;
	unsigned isHookedGfxDescSetAssigned : 1;
	unsigned assignedHookedSrvHeap : 16;

	unsigned bindlessCmpSrvDescId;
	unsigned bindlessGfxSrvDescId;
	//unsigned remains;
};

// Meetem TODO: Rewrite that to a sorted list,
// I guess it would be faster since list would be small;
static std::unordered_map<size_t, HookedRootSignature> hookedDescriptors;

#define ReturnOnFail(x, hr, OnFailureMsg, onFailureReturnValue) hr = x; if(FAILED(hr)){OutputDebugStringA(OnFailureMsg); return onFailureReturnValue;}

static void handle_hr_fatal(HRESULT hr, const char* error = "")
{
	if (FAILED(hr))
	{
		UnityLog::LogError("D3D12 Call failed with error 0x%p\n", (void*)(size_t)hr);
		abort();
	}
}

// #region Hooking
static bool Unprotect(void* addr) {
	const uint64_t pageSize = 4096;

	DWORD oldProtect = 0;
	auto protectResult = VirtualProtect((void*)((((size_t)addr) / pageSize) * pageSize), pageSize, PAGE_READWRITE, &oldProtect);
	if (protectResult == 0) {
		UnityLog::LogError("VirtualProtect failed, result: %d, old protect: %p\n", protectResult, (void*)(size_t)oldProtect);
		return false;
	}

	return true;
}

template<typename T>
static void** GetVTableEntryPtr(T* obj, int vtableOffset) {
	size_t* vtable = *(size_t**)obj;
	return (void**)((BYTE*)vtable + (vtableOffset));
}

template<typename T>
static void* Hook(T* obj, void* currentOrigValue, int vtableOffset, void* newFunction) {
	auto pptr = GetVTableEntryPtr<T>(obj, vtableOffset);
	auto old = *pptr;
	
	// already hooked, return the orig value.
	if (old == newFunction) {
		return currentOrigValue;
	}

	if (Unprotect(pptr)) {
		*pptr = newFunction;
	}

	return old;
}
// #endregion

#include "D3D12Hooks.h"

RenderAPI* CreateRenderAPI_D3D12()
{
	auto obj = new RenderAPI_D3D12();
	myD3D12 = obj;
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

	uint32_t allocSize = sizeof(D3D12_ROOT_PARAMETER) * MaxParams + 1024 * 1024;
	uint8_t* mem = (uint8_t*)malloc(allocSize);
	memset(mem, 0, allocSize);

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

static inline CommandListStateData GetCommandListState(ID3D12GraphicsCommandList* cmdList) {
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

static inline bool SetCommandListState(ID3D12GraphicsCommandList* cmdList, CommandListStateData state) {
	if (cmdList == nullptr) {
		return true;
	}

	UINT dsize = sizeof(CommandListStateData);
	CommandListStateData ret;
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

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device* device, _In_  D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
	REFIID riid,
	_COM_Outptr_  void** ppvHeap) {

	if (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
		uint32_t additional = RenderAPI_D3D12::numAdditionalSrvTotal();
		bool hooked = false;

		if (pDescriptorHeapDesc->NumDescriptors >= mainDescHeapMagic) {
			hooked = true;
			if (pDescriptorHeapDesc->NumDescriptors + additional > absoluteMaxDescriptors) {
				pDescriptorHeapDesc->NumDescriptors = absoluteMaxDescriptors;
				myD3D12->srvBaseOffset = pDescriptorHeapDesc->NumDescriptors - additional;
			} else {
				myD3D12->srvBaseOffset = pDescriptorHeapDesc->NumDescriptors;
				pDescriptorHeapDesc->NumDescriptors += additional;
			}
		}

		UnityLog::Log("Creating CBV/SRV/UAV descriptor set with count %d with type %d (%d additional entries)\n", (int)pDescriptorHeapDesc->NumDescriptors, (int)pDescriptorHeapDesc->Type, additional);

		auto res = OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
		if (FAILED(res)) {
			UnityLog::LogError("Can't create new descriptor heap: %p\n", (void*)(size_t)res);
		} else {
			auto ptr = (ID3D12DescriptorHeap*)*ppvHeap;
			if (ptr != nullptr) {
				myD3D12->srvDescriptorHeaps.push_back(ptr);

				if (hooked) {
					myD3D12->hookedDescriptorHeaps.push_back(ptr);
				}
			}
		}

		return res;
	}

	return OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
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
	hookedValue.descriptorId = rootSig.NumParameters;
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
		hookedDescriptors[(size_t)sig] = hookedValue;
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

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(
	ID3D12GraphicsCommandList1* This,
	_In_  ID3D12CommandAllocator* pAllocator,
	_In_opt_  ID3D12PipelineState* pInitialState
)
{
	SetCommandListState(This, {});
	return OrigReset(This, pAllocator, pInitialState);
}

int tries = 0;

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(ID3D12GraphicsCommandList* list,
	_In_opt_  ID3D12RootSignature* pRootSignature) 
{
	HookedRootSignature d{};
	auto hasDescHook = TryGetBindlessData(pRootSignature, d) && d.descriptorId > 0;
	auto dt = GetCommandListState(list);
	hasDescHook &= !myD3D12->hookedDescriptorHeaps.empty() && OrigSetGraphicsRootDescriptorTable != nullptr;

	if (!hasDescHook) {
		dt.bindlessGfxSrvDescId = 0;
	} else {
		dt.bindlessGfxSrvDescId = d.descriptorId;
	}

	SetCommandListState(list, dt);
	UnityLog::Log("%p: Set Graphics Root Signature %d %p", list, (int)hasDescHook, pRootSignature);
	OrigSetGraphicsRootSignature(list, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_ExecuteBundle(ID3D12GraphicsCommandList* This,
	_In_  ID3D12GraphicsCommandList* pCommandList)
{
	//UnityLog::Log("Executing bundle\n");
	OrigExecuteBundle(This, pCommandList);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(ID3D12GraphicsCommandList* This,
	_In_opt_  ID3D12RootSignature* pRootSignature) 
{
	// Meetem TODO: Copy Gfx.
	OrigSetComputeRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(ID3D12GraphicsCommandList10* This,
	_In_  UINT NumDescriptorHeaps,
	_In_reads_(NumDescriptorHeaps)  ID3D12DescriptorHeap* const* ppDescriptorHeaps) {

	auto dt = GetCommandListState(This);
	if(NumDescriptorHeaps == 2)
		UnityLog::Log("%p: Setting descriptor heaps: %d %d %d %p %p\n", This, NumDescriptorHeaps, dt.bindlessGfxSrvDescId, dt.isInHookedGfxRootSig, ppDescriptorHeaps[0], ppDescriptorHeaps[1]);
	else
		UnityLog::Log("%p: Setting descriptor heaps: %d %d %d\n", This, NumDescriptorHeaps, dt.bindlessGfxSrvDescId, dt.isInHookedGfxRootSig);

	if (myD3D12->hookedDescriptorHeaps.empty()) {
		UnityLog::LogWarning("SetDescriptorHeaps is called, but no srvHeap is set.\n");
		OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}

	if (ppDescriptorHeaps == nullptr || NumDescriptorHeaps == 0u) {
		UnityLog::Log("Reset desc heaps\n");
		OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}
	
	OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
	if(dt.bindlessGfxSrvDescId > 0){
		// Force set desc table if shader is bound
		This->SetGraphicsRootDescriptorTable(-1, {});
	}

	return;
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list,
	_In_  UINT RootParameterIndex,
	_In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {

	auto d3d = myD3D12;
	auto gfxList = (ID3D12GraphicsCommandList*)list;

	// This will never return that we are in bindless for non-graphics list.
	auto dt = GetCommandListState(gfxList);
	
	if (myD3D12->hookedDescriptorHeaps.empty()) {
		UnityLog::LogWarning("SetGraphicsRootDescriptorTable is called, but no srvHeap is set.\n");
		return;
	}

	int id = -1;
	if (dt.bindlessGfxSrvDescId != NoBindless)
		id = dt.bindlessGfxSrvDescId - 1;
	
	UnityLog::Log("%p: Set graphics root descriptor table: %d %d\n", list, RootParameterIndex, id);

	if (myD3D12->hookedDescriptorHeaps.size() > 1) {
		UnityLog::LogWarning("Can't know which hooked heap to bind. There are more than one SRV heap hooked.\n");
		id = -1;
	}

	if(id < 0 || id != RootParameterIndex){
		// Special case for just updating from heap setup.
		if((int)RootParameterIndex > 0)
			OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
	}

	if (id >= 0)
	{
		//SetCommandListState(gfxList, dt);
		//UnityLog::Debug("Set graphics root descriptor table for bindless: (hooked %d) -> %d with offset %d\n", RootParameterIndex, id, myD3D12->srvBaseOffset);

		auto assignedHeap = myD3D12->hookedDescriptorHeaps[0];
		//UnityLog::Log("Setting hooked descriptor table %d = %p\n", id, assignedHeap);

		CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(assignedHeap->GetGPUDescriptorHandleForHeapStart());
		gpuHandle.Offset(myD3D12->srvBaseOffset * myD3D12->srvIncrement);
		gpuHandle.Offset(myD3D12->getCurrentOffset() * myD3D12->srvIncrement);

		OrigSetGraphicsRootDescriptorTable(list, id, gpuHandle);
	}
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(ID3D12CommandList* list,
	_In_  UINT RootParameterIndex,
	_In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {

	// Meetem TODO: Mimic gfx
	OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

static bool d3d12hookedCmdList = false;

void RenderAPI_D3D12::HookCommandListObject(ID3D12GraphicsCommandList* cmdList) {
	if (cmdList == nullptr)
		return;
	
	HookCmdListFunc(cmdList, SetGraphicsRootDescriptorTable);
	HookCmdListFunc(cmdList, SetComputeRootDescriptorTable);
	HookCmdListFunc(cmdList, SetGraphicsRootSignature);
	HookCmdListFunc(cmdList, SetComputeRootSignature);
	
	HookCmdListFunc(cmdList, SetDescriptorHeaps);
	
	HookCmdListFunc(cmdList, ExecuteBundle);
	HookCmdListFunc(cmdList, Reset);
	//HookCmdListFunc(SetPipelineState);
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
		__D3D12HOOKS_InitializeD3D12Offsets();
		UnityLog::Log("Initializing D3D12");

		//UnityLog::Log("Hooked offsets: %d, %d, %d\n",
		//	__D3D12_CREATEDESCHEAP_VTOFFS, __D3D12_SetComputeRootDescriptorTable_VTOFFS, __D3D12_CreateRootSignature_VTOFFS);

		this->s_d3d12 = interfaces->Get<IUnityGraphicsD3D12v7>();
		this->device = s_d3d12->GetDevice();

		UnityD3D12PluginEventConfig config_second;
		config_second.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
		config_second.flags = kUnityD3D12EventConfigFlag_SyncWorkerThreads | kUnityD3D12EventConfigFlag_EnsurePreviousFrameSubmission | kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
		config_second.ensureActiveRenderTextureIsBound = false;
		s_d3d12->ConfigureEvent(2147473649, &config_second);

		HookDeviceFunc(CreateDescriptorHeap);
		HookDeviceFunc(CreateRootSignature);
		HookDeviceFunc(CreateComputePipelineState);
		HookDeviceFunc(CreateGraphicsPipelineState);

		srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		initialize_and_create_resources();
		isInitialized = true;
		return;
	}

	if (!isInitialized)
		return;

	if (type == kUnityGfxDeviceEventShutdown) {
		//vtableEntryPtr = GetVTableEntryPtr<ID3D12Device>(dev, 14);
		//*vtableEntryPtr = origCreateDescriptorHeap;
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
	
	ID3D12Device4* device4 = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&device4));
	
	ID3D12CommandAllocator* commandAllocator = nullptr;
	handle_hr_fatal(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&commandAllocator)
	), "Can't create command allocator.\n");
	
	ID3D12CommandAllocator* commandAllocatorBundle = nullptr;
	handle_hr_fatal(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_BUNDLE,
		IID_PPV_ARGS(&commandAllocatorBundle)
	), "Can't create command allocator bundle.\n");
	
	#define HookTypedCommandList(type, allocator, listtype) type* variable_##type##allocator;\
	handle_hr_fatal(device->CreateCommandList(\
		0,          /* Node mask; for single-GPU operation, set to 0*/\
		listtype,   /*Type of command list*/\
		allocator,  /*Command allocator associated with the command list*/\
		nullptr,    /*Pipeline state(initial, can be nullptr)*/\
		IID_PPV_ARGS(&variable_##type##allocator)\
		), "Can't create command list");\
		HookCommandListObject((ID3D12GraphicsCommandList*)variable_##type##allocator);\
		variable_##type##allocator->Release();
	
	#define HookTypedCommandList1(type, allocator, listtype) type* variable_##type##allocator;\
	handle_hr_fatal(device4->CreateCommandList1(\
		0,          /* Node mask; for single-GPU operation, set to 0*/\
		listtype,   /*Type of command list*/\
		D3D12_COMMAND_LIST_FLAG_NONE,   \
		IID_PPV_ARGS(&variable_##type##allocator)\
		), "Can't create command list");\
		HookCommandListObject((ID3D12GraphicsCommandList*)variable_##type##allocator);\
		variable_##type##allocator->Release();

	HookTypedCommandList(ID3D12GraphicsCommandList,   commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList1,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList2,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList3,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList4,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList5,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList6,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList7,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList8,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList9,  commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
	HookTypedCommandList(ID3D12GraphicsCommandList10, commandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)
		
	HookTypedCommandList(ID3D12GraphicsCommandList,   commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList1,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList2,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList3,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList4,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList5,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList6,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList7,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList8,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList9,  commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	HookTypedCommandList(ID3D12GraphicsCommandList10, commandAllocatorBundle, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		
	if (device4 != nullptr) {
		HookTypedCommandList1(ID3D12GraphicsCommandList, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList1, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList2, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList3, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList4, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList5, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList6, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList7, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList8, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList9, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)
		HookTypedCommandList1(ID3D12GraphicsCommandList10, commandAllocator1, D3D12_COMMAND_LIST_TYPE_DIRECT)

		HookTypedCommandList1(ID3D12GraphicsCommandList, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList1, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList2, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList3, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList4, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList5, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList6, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList7, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList8, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList9, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
		HookTypedCommandList1(ID3D12GraphicsCommandList10, commandAllocator2, D3D12_COMMAND_LIST_TYPE_BUNDLE)
	}
	
	commandAllocatorBundle->Release();
	commandAllocator->Release();
}

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
