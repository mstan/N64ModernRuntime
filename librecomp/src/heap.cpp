#include "o1heap/o1heap.h"

#include "librecomp/addresses.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/helpers.hpp"

// Byte offset of the o1heap control block within the guest RDRAM image.
// The heap lives entirely inside RDRAM, so every accessor reconstitutes the
// instance pointer from this offset rather than caching a raw host pointer.
static uint32_t s_heap_rdram_off;

static inline O1HeapInstance* heap_instance(uint8_t* rdram) {
    return reinterpret_cast<O1HeapInstance*>(rdram + s_heap_rdram_off);
}

extern "C" void recomp_alloc(uint8_t* rdram, recomp_context* ctx) {
    // r4 carries the requested size; hand the guest back a KSEG0 virtual
    // address (host offset | 0x80000000, sign-extended into a 64-bit reg).
    uint8_t* block = reinterpret_cast<uint8_t*>(recomp::alloc(rdram, ctx->r4));
    uint32_t block_off = static_cast<uint32_t>(block - rdram);
    ctx->r2 = block_off + 0xFFFFFFFF80000000ULL;
}

extern "C" void recomp_free(uint8_t* rdram, recomp_context* ctx) {
    PTR(void) guest_ptr = _arg<0, PTR(void)>(rdram, ctx);
    // free(NULL) is a no-op, matching the C contract the game relies on.
    if (guest_ptr == NULLPTR) {
        return;
    }
    recomp::free(rdram, TO_PTR(void, guest_ptr));
}

void recomp::register_heap_exports() {
    recomp::overlays::register_base_export("recomp_alloc", recomp_alloc);
    recomp::overlays::register_base_export("recomp_free", recomp_free);
}

void recomp::init_heap(uint8_t* rdram, uint32_t address) {
    // Round the requested base up to a 16-byte boundary for o1heap.
    address = (address + 15U) & ~15U;

    // Translate the KSEG0 base into an RDRAM offset; the heap then occupies
    // everything from there to the end of emulated memory.
    s_heap_rdram_off = address - 0x80000000U;
    size_t span = recomp::mem_size - s_heap_rdram_off;

    printf("Initializing recomp heap at offset 0x%08X with size 0x%08X\n", s_heap_rdram_off, static_cast<uint32_t>(span));

    o1heapInit(rdram + s_heap_rdram_off, span);
}

void* recomp::alloc(uint8_t* rdram, size_t size) {
    return o1heapAllocate(heap_instance(rdram), size);
}

void recomp::free(uint8_t* rdram, void* mem) {
    o1heapFree(heap_instance(rdram), mem);
}
