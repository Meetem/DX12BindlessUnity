#include "../Unity/IUnityInterface.h"
#include "../Unity/IUnityLog.h"
#include <cstdarg>
#include <cstdlib>
#include <cstdio>

#include "UnityLog.h"

#if _BINDLESS_DEBUG
#include <Windows.h>
#include <debugapi.h>
#endif

const char* prefix = "[Meetem.Bindless]: ";
const unsigned prefixLength = 19u;

char UnityLog::logData[UNITY_LOG_MAX_STR];
IUnityLog* UnityLog::logInstance;

void UnityLog::Initialize(IUnityInterfaces* unityInterfaces) {
	logInstance = unityInterfaces->Get<IUnityLog>();
	for (int i = 0; i < UNITY_LOG_MAX_STR; i++) {
		logData[i] = 0;
	}

	for (unsigned i = 0; i < prefixLength; i++) {
		logData[i] = prefix[i];
	}
}

void UnityLog::Debug(const char* format, ...) {
#if _BINDLESS_DEBUG
	va_list args;
	va_start(args, format);
	vsprintf_s(logData + prefixLength, (size_t)(UNITY_LOG_MAX_STR - 1 - prefixLength), format, args);
	va_end(args);

	if(logInstance != nullptr)
		logInstance->Log(kUnityLogTypeLog, logData, "", 0);

	//OutputDebugStringA(logData);
#endif
}

void UnityLog::Log(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsprintf_s(logData + prefixLength, (size_t)(UNITY_LOG_MAX_STR - 1 - prefixLength), format, args);
	va_end(args);

	if(logInstance != nullptr)
		logInstance->Log(kUnityLogTypeLog, logData, "", 0);

#if _BINDLESS_DEBUG
	OutputDebugStringA(logData);
#endif

}

void UnityLog::LogError(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsprintf_s(logData + prefixLength, (size_t)(UNITY_LOG_MAX_STR - 1 - prefixLength), format, args);
	va_end(args);

	if (logInstance != nullptr)
		logInstance->Log(kUnityLogTypeError, logData, "", 0);

#if _BINDLESS_DEBUG
	OutputDebugStringA(logData);
#endif

}

void UnityLog::LogWarning(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsprintf_s(logData + prefixLength, (size_t)(UNITY_LOG_MAX_STR - 1 - prefixLength), format, args);
	va_end(args);

	if (logInstance != nullptr)
		logInstance->Log(kUnityLogTypeWarning, logData, "", 0);

#if _BINDLESS_DEBUG
	OutputDebugStringA(logData);
#endif

}

void UnityLog::LogException(const char* fileName, int fileLine, const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsprintf_s(logData + prefixLength, (size_t)(UNITY_LOG_MAX_STR - 1 - prefixLength), format, args);
	va_end(args);

	if (fileName == nullptr)
		fileName = "";

	if (logInstance != nullptr)
		logInstance->Log(kUnityLogTypeException, logData, fileName, fileLine);

#if _BINDLESS_DEBUG
	OutputDebugStringA(logData);
#endif

}
