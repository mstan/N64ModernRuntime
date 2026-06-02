#include "recomp.h"
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

constexpr void update_bit(uint32_t& state, uint32_t flags, RDPStatusBit bit) {
    int reset_bit_pos = (int)bit * 2 + 0;
    int set_bit_pos = (int)bit * 2 + 1;
    bool set = (flags & (1U << set_bit_pos)) != 0;
    bool reset = (flags & (1U << reset_bit_pos)) != 0;

    if (set ^ reset) {
        if (set) {
            state |= (1U << (int)bit);
        }
        else {
            state &= ~(1U << (int)bit);
        }
    }
}

recomp::rdp::DpRegisters& recomp::rdp::dp_registers() {
    static DpRegisters regs{
        .status = 1 << (int)RDPStatusBit::BufferReady,
    };
    return regs;
}

extern "C" void osDpSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    assert(false);
}

extern "C" void osDpGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = recomp::rdp::dp_registers().status;
}

extern "C" void osDpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t& status = recomp::rdp::dp_registers().status;
    update_bit(status, ctx->r4, RDPStatusBit::XbusDmem);
    update_bit(status, ctx->r4, RDPStatusBit::Freeze);
    update_bit(status, ctx->r4, RDPStatusBit::Flush);
}

extern "C" void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx) {
    const auto& regs = recomp::rdp::dp_registers();
    const gpr array = ctx->r4;
    MEM_W(0x00, array) = regs.clock;
    MEM_W(0x04, array) = regs.bufbusy;
    MEM_W(0x08, array) = regs.pipebusy;
    MEM_W(0x0C, array) = regs.tmem;
}
