#pragma once

// meetem hooks
typedef HRESULT(STDMETHODCALLTYPE* D3D12_CreateDescriptorHeap)(ID3D12Device* device, _In_  const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
    REFIID riid,
    _COM_Outptr_  void** ppvHeap);

typedef void (STDMETHODCALLTYPE* D3D12_SetComputeRootDescriptorTable)(
    ID3D12CommandList* list,
    _In_  UINT RootParameterIndex,
    _In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);

typedef HRESULT(STDMETHODCALLTYPE* D3D12_CreateRootSignature)(
    ID3D12Device* This,
    _In_  UINT nodeMask,
    _In_reads_(blobLengthInBytes)  const void* pBlobWithRootSignature,
    _In_  SIZE_T blobLengthInBytes,
    REFIID riid,
    _COM_Outptr_  void** ppvRootSignature);

typedef void (STDMETHODCALLTYPE* D3D12_SetPipelineState)(
    ID3D12GraphicsCommandList* This,
    _In_  ID3D12PipelineState* pPipelineState);

typedef void (STDMETHODCALLTYPE* D3D12_SetDescriptorHeaps)(
    ID3D12GraphicsCommandList* This,
    _In_  UINT NumDescriptorHeaps,
    _In_reads_(NumDescriptorHeaps)  ID3D12DescriptorHeap* const* ppDescriptorHeaps);

typedef HRESULT(STDMETHODCALLTYPE* D3D12_CreateComputePipelineState)(
    ID3D12Device* This,
    _In_  const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_  void** ppPipelineState);

typedef HRESULT(STDMETHODCALLTYPE* D3D12_CreateGraphicsPipelineState)(
    ID3D12Device* This,
    _In_  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_  void** ppPipelineState);

typedef void (STDMETHODCALLTYPE* D3D12_SetGraphicsRootDescriptorTable)(
    ID3D12GraphicsCommandList* This,
    _In_  UINT RootParameterIndex,
    _In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);

typedef void (STDMETHODCALLTYPE* D3D12_SetComputeRootSignature)(
    ID3D12GraphicsCommandList* This,
    _In_opt_  ID3D12RootSignature* pRootSignature);

typedef void (STDMETHODCALLTYPE* D3D12_SetGraphicsRootSignature)(
    ID3D12GraphicsCommandList* This,
    _In_opt_  ID3D12RootSignature* pRootSignature);

typedef HRESULT(STDMETHODCALLTYPE* D3D12_Reset)(
    ID3D12GraphicsCommandList1* This,
    _In_  ID3D12CommandAllocator* pAllocator,
    _In_opt_  ID3D12PipelineState* pInitialState);

#ifndef D3D12_HOOKS_DECLARE

#define RegisterHookFunc(Name) static D3D12_##Name Orig##Name = NULL;\
extern "C" unsigned __D3D12_VTOFFS_##Name;

#define HookDeviceFunc(Name) Orig##Name = (D3D12_##Name)\
    Hook(device, __D3D12_VTOFFS_##Name, Hooked_##Name)

#define HookCmdListFunc(Name) Orig##Name = (D3D12_##Name)\
    Hook(cmdList, __D3D12_VTOFFS_##Name, Hooked_##Name)

extern "C" void __D3D12HOOKS_InitializeD3D12Offsets();
#else
#define RegisterHookFunc(Name) unsigned __D3D12_VTOFFS_##Name = 0;

#define HookDeviceFunc(Name) __D3D12_VTOFFS_##Name = offsetof(ID3D12DeviceVtbl, ##Name)
#define HookCmdListFunc(Name) __D3D12_VTOFFS_##Name = offsetof(ID3D12GraphicsCommandListVtbl, ##Name)

#endif

// device
RegisterHookFunc(CreateDescriptorHeap)
RegisterHookFunc(CreateRootSignature)
RegisterHookFunc(CreateComputePipelineState)
RegisterHookFunc(CreateGraphicsPipelineState)

// graphics command list
RegisterHookFunc(SetPipelineState)
RegisterHookFunc(SetDescriptorHeaps)
RegisterHookFunc(SetComputeRootDescriptorTable)
RegisterHookFunc(SetGraphicsRootDescriptorTable)

RegisterHookFunc(SetComputeRootSignature)
RegisterHookFunc(SetGraphicsRootSignature)
RegisterHookFunc(Reset)

#undef FuncFunc
