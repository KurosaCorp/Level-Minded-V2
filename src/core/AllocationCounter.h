#pragma once

#include <cstddef>
#include <cstdint>

namespace lm {

// Running totals of heap allocations, for spotting per-frame allocations (a steady-state frame
// should allocate nothing). Counted by the global operator new replacements in
// AllocationCounter.cpp and by the ImGui allocator hooks below.
uint64_t HeapAllocationCount();
uint64_t ImGuiAllocationCount();

// Pass to ImGui::SetAllocatorFunctions() before creating the ImGui context.
void* CountingImGuiAlloc(size_t size, void* userData);
void CountingImGuiFree(void* pointer, void* userData);

} // namespace lm
