#include "../source/DescriptorTableInjection.h"
#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

static std::vector<std::pair<unsigned, unsigned>> bindings;
static unsigned injections = 0;
static void Hook(unsigned index, unsigned handle);
static void Inject() {
	++injections;
	assert(injections < 10); // The former implementation recursed indefinitely.
	DescriptorTableInjection scope;
	Hook(5, 900); // The selected original forwards through another detour.
}
static void Hook(unsigned index, unsigned handle) {
	ForwardDescriptorTable(
		[&] { bindings.emplace_back(index, handle); },
		Inject);
}
int main() {
	// Unity's table must reach the driver, then exactly one bindless table.
	Hook(3, 100);
	assert(injections == 1);
	assert((bindings == std::vector<std::pair<unsigned, unsigned>>{{3, 100}, {5, 900}}));
	assert(!DescriptorTableInjection::Active());
	bindings.clear();
	injections = 0;
	// An outer wrapper forwards Unity's request through another hooked setter.
	ForwardDescriptorTable([&] { Hook(4, 200); }, Inject);
	assert(injections == 1);
	assert((bindings == std::vector<std::pair<unsigned, unsigned>>{{4, 200}, {5, 900}}));
	// Root-signature-triggered injection also bypasses all nested injections.
	bindings.clear();
	injections = 0;
	Inject();
	assert(injections == 1 && bindings.size() == 1);
	try {
		ForwardDescriptorTable([] { throw std::runtime_error("test"); }, Inject);
	} catch (const std::runtime_error&) {}
	assert(!DescriptorTableInjection::Active());
}
