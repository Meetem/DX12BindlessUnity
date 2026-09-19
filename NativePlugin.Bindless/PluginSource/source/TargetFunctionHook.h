#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

// MinHook must be included before this header. Each patched entry point gets
// its own detour, so nested wrapper/runtime calls retain their own original.
template<typename Function>
class TargetFunctionHook;

template<typename Result, typename... Args>
class TargetFunctionHook<Result (STDMETHODCALLTYPE*)(Args...)> {
public:
	using Function = Result (STDMETHODCALLTYPE*)(Args...);
	using Handler = Result (STDMETHODCALLTYPE*)(Function, Args...);

	// Used when a hook needs to call a different method on the same object.
	static Function Original(void* target) {
		std::lock_guard<std::mutex> lock(installMutex);
		for (auto& slot : slots) {
			if (slot.target == target)
				return slot.original.load(std::memory_order_acquire);
		}
		return reinterpret_cast<Function>(target);
	}

	static MH_STATUS Install(void* target, Handler handler) {
		std::lock_guard<std::mutex> lock(installMutex);
		for (size_t i = 0; i < slots.size(); ++i) {
			auto& slot = slots[i];
			if (slot.target == target)
				return slot.enabled && slot.handler == handler ? MH_OK : MH_ERROR_ALREADY_CREATED;
		}

		for (size_t i = 0; i < slots.size(); ++i) {
			auto& slot = slots[i];
			if (slot.target != nullptr)
				continue;

			void* original = nullptr;
			auto status = MH_CreateHook(target, reinterpret_cast<void*>(detours[i]), &original);
			if (status != MH_OK)
				return status;

			slot.target = target;
			slot.handler = handler;
			// Publish before enabling: another thread may enter immediately.
			slot.original.store(reinterpret_cast<Function>(original), std::memory_order_release);
			status = MH_EnableHook(target);
			slot.enabled = status == MH_OK;
			if (status != MH_OK) {
				// Keep the slot reserved if removal fails; never reuse a live detour.
				if (MH_RemoveHook(target) == MH_OK) {
					slot.original.store(nullptr, std::memory_order_release);
					slot.target = nullptr;
				}
			}
			return status;
		}
		return MH_ERROR_MEMORY_ALLOC;
	}

private:
	static constexpr size_t Capacity = 16;
	struct Slot {
		void* target = nullptr;
		Handler handler = nullptr;
		bool enabled = false;
		std::atomic<Function> original{nullptr};
	};

	template<size_t Index>
	static Result STDMETHODCALLTYPE Invoke(Args... args) {
		auto original = slots[Index].original.load(std::memory_order_acquire);
		return slots[Index].handler(original, args...);
	}

	template<size_t... Indices>
	static constexpr std::array<Function, sizeof...(Indices)> MakeDetours(std::index_sequence<Indices...>) {
		return { &Invoke<Indices>... };
	}

	inline static std::array<Slot, Capacity> slots{};
	inline static std::mutex installMutex;
	inline static constexpr auto detours = MakeDetours(std::make_index_sequence<Capacity>{});
};
