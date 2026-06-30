#include "ultramodern/ultra64.h"

#define K0BASE        0x80000000
#define K1BASE        0xA0000000
#define K2BASE        0xC0000000
#define IS_KSEG0(x)   ((u32)(x) >= K0BASE && (u32)(x) < K1BASE)
#define IS_KSEG1(x)   ((u32)(x) >= K1BASE && (u32)(x) < K2BASE)
#define K0_TO_PHYS(x) ((u32)(x)&0x1FFFFFFF)
#define K1_TO_PHYS(x) ((u32)(x)&0x1FFFFFFF)

u32 osVirtualToPhysical(PTR(void) addr) {
    const uintptr_t vaddr = (uintptr_t)addr;

    // The cached and uncached direct-mapped windows both resolve to a
    // physical address simply by masking off the segment bits; only the
    // mask macro that applies differs between them.
    if (IS_KSEG0(vaddr)) {
        return K0_TO_PHYS(vaddr);
    }
    if (IS_KSEG1(vaddr)) {
        return K1_TO_PHYS(vaddr);
    }

    // Outside those windows a real translation would consult the TLB,
    // which isn't modeled here yet, so return the value untouched.
    return (u32)vaddr;
}

