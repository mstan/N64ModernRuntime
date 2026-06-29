#include "recomp.h"
#include "ultramodern/ultra_trace.hpp"
#include "librecomp/rdp.hpp"

enum class RDPStatusBit {
    XbusDmem = 0,
    Freeze = 1,
    Flush = 2,
    CommandBusy = 6,
    BufferReady = 7,
    DmaBusy = 8,
    EndValid = 9,
    StartValid = 10,
};

// A DPC_STATUS write encodes each controllable bit as an adjacent
// clear/set request pair: even position requests a clear, odd position a
// set. Honor the request only when exactly one of the pair is asserted;
// asking to both set and clear (or neither) leaves the bit alone.
constexpr void apply_status_request(uint32_t& status, uint32_t write_word, RDPStatusBit bit) {
    const int bit_index = static_cast<int>(bit);
    const uint32_t clear_request = 1U << (bit_index * 2);
    const uint32_t set_request   = 1U << (bit_index * 2 + 1);
    const bool want_clear = (write_word & clear_request) != 0;
    const bool want_set   = (write_word & set_request) != 0;

    if (want_set == want_clear) {
        return;
    }
    if (want_set) {
        status |= (1U << bit_index);
    } else {
        status &= ~(1U << bit_index);
    }
}

recomp::rdp::DpRegisters& recomp::rdp::dp_registers() {
    static DpRegisters regs{
        .status = 1 << (int)RDPStatusBit::BufferReady,
    };
    return regs;
}

extern "C" void osDpSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    assert(false);
}

extern "C" void osDpGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    ctx->r2 = recomp::rdp::dp_registers().status;
}

extern "C" void osDpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    uint32_t& status = recomp::rdp::dp_registers().status;
    apply_status_request(status, ctx->r4, RDPStatusBit::XbusDmem);
    apply_status_request(status, ctx->r4, RDPStatusBit::Freeze);
    apply_status_request(status, ctx->r4, RDPStatusBit::Flush);
}

extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    const auto& regs = recomp::rdp::dp_registers();
    const gpr array = ctx->r4;
    MEM_W(0x00, array) = regs.clock;
    MEM_W(0x04, array) = regs.bufbusy;
    MEM_W(0x08, array) = regs.pipebusy;
    MEM_W(0x0C, array) = regs.tmem;
}
