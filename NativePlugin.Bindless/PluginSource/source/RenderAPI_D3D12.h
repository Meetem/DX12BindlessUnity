#pragma once

#if SUPPORT_D3D12
#include <assert.h>
#include <dxgi1_6.h>
#include <initguid.h>
#include <d3d12.h>
#include "d3dx12.h"
#include "Unity/IUnityGraphicsD3D12.h"

#include <atomic>
#include <unordered_map>
#include <utility>
#include <map>
#include "RenderAPI.h"

class RenderAPI_D3D12 : public RenderAPI
{
public:
    RenderAPI_D3D12();
    virtual ~RenderAPI_D3D12() override { }

    virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces) override;
    virtual bool GetUsesReverseZ() override { return true; }

    virtual int SetBindlessTextures(int offset, unsigned numTextures, BindlessTexture* textures
    );

    virtual void SetCurrentBindlessOffset(void* eventData) override;
    static DXGI_FORMAT typeless_fmt_to_typed(DXGI_FORMAT format);
    
    static uint32_t numAdditionalSrv() {
        return 4096u;
    }

    static uint32_t numAdditionalSrvTotal() {
        return numAdditionalSrv() * 4u;
    }

    uint32_t srvBaseOffset;
    uint32_t srvIncrement;
    ID3D12Device* device;
    BindlessTextureList lastList;

    inline int getCurrentOffset() const {
        return currentFrameBindlessOffset;
    }

    std::vector<ID3D12DescriptorHeap*> srvDescriptorHeaps{};
    std::vector<ID3D12DescriptorHeap*> hookedDescriptorHeaps{};

private:
    int currentFrameBindlessOffset;

    // Creates and initializes all resources that are used across multiple frames
    void initialize_and_create_resources();
    void release_resources();

    virtual void HookSetFunctions() override;

    // When unity frame fence changes we can be sure that previously submitted command lists have finished executing
    void wait_for_unity_frame_fence(UINT64 fence_value);

    // Wait on any user provided fence
    void wait_on_fence(UINT64 fence_value, ID3D12Fence* fence, HANDLE fence_event);
   
    IUnityGraphicsD3D12v7* s_d3d12;
};

#endif