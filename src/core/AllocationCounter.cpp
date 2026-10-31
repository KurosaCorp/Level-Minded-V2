#include "core/AllocationCounter.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace {

std::atomic<uint64_t> g_heapAllocations{0};
std::atomic<uint64_t> g_imguiAllocations{0};

void* CountedMalloc(std::size_t size)
{
    g_heapAllocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}

void* CountedAlignedMalloc(std::size_t size, std::align_val_t alignment)
{
    g_heapAllocations.fetch_add(1, std::memory_order_relaxed);
    return _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
}

} // namespace

namespace lm {

uint64_t HeapAllocationCount()
{
    return g_heapAllocations.load(std::memory_order_relaxed);
}

uint64_t ImGuiAllocationCount()
{
    return g_imguiAllocations.load(std::memory_order_relaxed);
}

void* CountingImGuiAlloc(size_t size, void*)
{
    g_imguiAllocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}

void CountingImGuiFree(void* pointer, void*)
{
    std::free(pointer);
}

} // namespace lm

// ---- Global allocation function replacements (counting only; same semantics as the defaults).

void* operator new(std::size_t size)
{
    if (void* p = CountedMalloc(size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return CountedMalloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return CountedMalloc(size); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* p = CountedAlignedMalloc(size, alignment))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return operator new(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return CountedAlignedMalloc(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return CountedAlignedMalloc(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { _aligned_free(p); }
