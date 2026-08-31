// Example low level rendering Unity plugin

#include "PlatformBase.h"
#include "RenderAPI.h"
#include "Unity/IUnityLog.h"

#include <assert.h>
#include <math.h>
#include <vector>
//#include "Unity/IUnityGraphicsVulkan.h"
#include "UAL/UnityLog.h"

#ifdef USE_MINHOOK
#include "MinHook/include/MinHook.h"

static bool InitializeHooks(){
	if(MH_Initialize() != MH_STATUS::MH_OK){
		UnityLog::LogError("Can't initialize MinHook!\n");
		abort();
		return false;
	}

	return true;
}
#else
static bool InitializeHooks() {
	return true;
}
#endif

// --------------------------------------------------------------------------
// UnitySetInterfaces

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType);

IUnityInterfaces* s_UnityInterfaces = NULL;
IUnityGraphics* s_Graphics = NULL;

extern void InstallEarlyD3D12Hooks();
extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces * unityInterfaces)
{
	s_UnityInterfaces = unityInterfaces;
	UnityLog::Initialize(s_UnityInterfaces);
	
	if(!InitializeHooks()){
		return;
	}

	#if SUPPORT_D3D12
	InstallEarlyD3D12Hooks();
	#endif

	s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
	s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

	// Run OnGraphicsDeviceEvent(initialize) manually on plugin load
	//OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
{
	s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
}

// --------------------------------------------------------------------------
// GraphicsDeviceEvent
static RenderAPI* s_CurrentAPI = NULL;
static UnityGfxRenderer s_DeviceType = kUnityGfxRendererNull;

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
{
	// Create graphics API implementation upon initialization
	if (eventType == kUnityGfxDeviceEventInitialize)
	{
		assert(s_CurrentAPI == NULL);
		UnityLog::Log("OnGraphicsDeviceEvent == kUnityGfxDeviceEventInitialize\n");

		s_DeviceType = s_Graphics->GetRenderer();
		s_CurrentAPI = CreateRenderAPI(s_DeviceType);
	}

	// Let the implementation process the device related events
	if (s_CurrentAPI)
	{
		s_CurrentAPI->ProcessDeviceEvent(eventType, s_UnityInterfaces);
	}

	// Cleanup graphics API implementation upon shutdown
	if (eventType == kUnityGfxDeviceEventShutdown)
	{
		delete s_CurrentAPI;
		s_CurrentAPI = NULL;
		s_DeviceType = kUnityGfxRendererNull;
	}
}

static void UNITY_INTERFACE_API OnRenderEventAndData(int eventID, void* data)
{
	// Unknown / unsupported graphics device type? Do nothing
	if (s_CurrentAPI == NULL)
		return;

	if (eventID == 2147473649) {
		s_CurrentAPI->SetCurrentBindlessOffset(data);
	}
}

// --------------------------------------------------------------------------
// GetRenderEventFunc, an example function we export which is used to get a rendering event callback function.
extern "C" UNITY_INTERFACE_EXPORT UnityRenderingEventAndData UNITY_INTERFACE_API MeetemBindless_GetRenderEventFuncWithData()
{
	return OnRenderEventAndData;
}

unsigned long numTextureUpdates = 0;
extern "C" UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API MeetemBindless_SetBindlessTextures(int offset, int numTextures, BindlessTexture * textures)
{
	if (s_CurrentAPI == nullptr || textures == nullptr || numTextures <= 0)
		return 0;

	return s_CurrentAPI->SetBindlessTextures(offset,(unsigned)numTextures, textures);
}