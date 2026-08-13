#ifndef __UNITY_LOG__H___
#define __UNITY_LOG__H___

#ifndef UNITY_LOG_MAX_STR
#define UNITY_LOG_MAX_STR 4096
#endif

struct IUnityInterfaces;
struct IUnityLog;

class UnityLog {
public:
	static void Initialize(IUnityInterfaces* unityInterfaces);

	static void Debug(const char* format, ...);
	static void Log(const char* format, ...);

	static void LogError(const char* format, ...);

	static void LogWarning(const char* format, ...);

	static void LogException(const char* fileName, int fileLine, const char* format, ...);
protected:
	//static char logData[UNITY_LOG_MAX_STR];
	static IUnityLog* logInstance;
};

#endif
