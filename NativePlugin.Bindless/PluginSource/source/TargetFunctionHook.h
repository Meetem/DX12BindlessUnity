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

	// Slots are published once and live as long as the installed hooks. Readers
	// never lock or allocate; release/acquire publishes the immutable payload.
	static Function Original(void* target) {
		for (auto& slot : slots) {
			if (slot.target.load(std::memory_order_acquire) == target)
				return slot.original.load(std::memory_order_relaxed);
		}
		return reinterpret_cast<Function>(target);
	}

	static MH_STATUS Install(void* target, Handler handler) {
		// Different objects may acquire private vtables with identical targets.
		// Already-installed targets require no installation lock or MinHook call.
		for (auto& slot : slots) {
			if (slot.target.load(std::memory_order_acquire) == target &&
				slot.enabled.load(std::memory_order_acquire))
				return slot.handler == handler ? MH_OK : MH_ERROR_ALREADY_CREATED;
		}

		// Patching executable code suspends threads inside MinHook. A spinlock
		// would waste CPU here; only the cold installation path uses a mutex.
		std::lock_guard<std::mutex> lock(installMutex);
		for (auto& slot : slots) {
			if (slot.target.load(std::memory_order_relaxed) == target) {
				if (slot.handler != handler)
					return MH_ERROR_ALREADY_CREATED;
				return Enable(slot);
			}
		}

		for (size_t i = 0; i < slots.size(); ++i) {
			auto& slot = slots[i];
			if (slot.target.load(std::memory_order_relaxed) != nullptr)
				continue;

			void* original = nullptr;
			auto status = MH_CreateHook(target, reinterpret_cast<void*>(detours[i]), &original);
			if (status != MH_OK)
				return status;

			slot.handler = handler;
			// Publish before enabling: another thread may enter immediately.
			slot.original.store(reinterpret_cast<Function>(original), std::memory_order_release);
			slot.target.store(target, std::memory_order_release);
			return Enable(slot);
		}
		return MH_ERROR_MEMORY_ALLOC;
	}

private:
	static constexpr size_t Capacity = 16;
	struct Slot {
		std::atomic<void*> target{nullptr};
		Handler handler = nullptr;
		std::atomic<bool> enabled{false};
		std::atomic<Function> original{nullptr};
	};
	static_assert(std::atomic<void*>::is_always_lock_free &&
		std::atomic<Function>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
		"Hook dispatch requires lock-free pointer and flag loads");

	static MH_STATUS Enable(Slot& slot) {
		if (slot.enabled.load(std::memory_order_relaxed))
			return MH_OK;
		auto status = MH_EnableHook(slot.target.load(std::memory_order_relaxed));
		slot.enabled.store(status == MH_OK, std::memory_order_release);
		// On failure keep the disabled hook/trampoline alive. Published readers
		// may hold it, and a later Install can retry enabling the same target.
		return status;
	}

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
