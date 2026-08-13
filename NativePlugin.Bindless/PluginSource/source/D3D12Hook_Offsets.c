// This file must be C file, not CPP.

#include "d3d12.h"

#define D3D12_HOOKS_DECLARE
#include "D3D12Hooks.h"

void __D3D12HOOKS_InitializeD3D12Offsets() {

	// Device
	HookDeviceFunc(CreateDescriptorHeap);
	HookDeviceFunc(CreateRootSignature);
	HookDeviceFunc(CreateComputePipelineState);
	HookDeviceFunc(CreateGraphicsPipelineState);

	// Creation
	HookDeviceFunc(CreateCommandList);
	HookDevice4Func(CreateCommandList1);

	// Command List
	HookCmdListFunc(SetPipelineState);
	HookCmdListFunc(SetDescriptorHeaps);
	HookCmdListFunc(SetComputeRootDescriptorTable);
	HookCmdListFunc(SetGraphicsRootDescriptorTable);
	HookCmdListFunc(SetComputeRootSignature);
	HookCmdListFunc(SetGraphicsRootSignature);
	HookCmdListFunc(Reset);

}

