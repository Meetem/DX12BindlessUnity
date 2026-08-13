#include <cinttypes>
/*
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
static void* Hook(T* obj, int vtableOffset, void* newFunction) {
	auto pptr = GetVTableEntryPtr<T>(obj, vtableOffset);
	auto old = *pptr;
	if (Unprotect(pptr)) {
		*pptr = newFunction;
	}

	return old;
}
// #endregion
*/