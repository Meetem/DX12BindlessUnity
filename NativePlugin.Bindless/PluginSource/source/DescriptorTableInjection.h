#pragma once

// A runtime/wrapper trampoline can forward into another hooked setter.
// Such calls must reach the driver without injecting another bindless table.
class DescriptorTableInjection {
public:
	DescriptorTableInjection() { ++depth; }
	~DescriptorTableInjection() { --depth; }
	DescriptorTableInjection(const DescriptorTableInjection&) = delete;
	DescriptorTableInjection& operator=(const DescriptorTableInjection&) = delete;
	static bool Active() { return depth != 0; }
private:
	inline static thread_local unsigned depth = 0;
};

template<typename Forward, typename Inject>
void ForwardDescriptorTable(Forward forward, Inject inject) {
	const bool nested = DescriptorTableInjection::Active();
	DescriptorTableInjection scope;
	forward();
	if (!nested)
		inject();
}
