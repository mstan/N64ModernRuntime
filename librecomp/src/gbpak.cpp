// N64ModernRuntime — Transfer Pak (GB Pak) libultra accessory-API HLE.
//
// Copyright (c) 2026 Matthew Stanley
//
// Level A of the universal Transfer Pak (see
// PocketMonstersStadiumRecomp/docs/transfer_pak_universal_design.md): game-
// agnostic HLE bodies for the standard libultra accessory block-I/O functions,
// routed to the shared cart model (librecomp::gbcart). Any game whose accessory
// functions carry these standard names binds automatically (they are in
// N64Recomp's reimplemented_funcs set) — no per-game shim. Promoted from
// PokemonStadiumRecomp's app-level transfer_pak.cpp shims.
//
// ---------------------------------------------------------------------
#include <array>
#include <cstdint>
#include <cstdlib>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"
#include "librecomp/ultra_trace.hpp"
#include "librecomp/gbcart.hpp"

namespace {
    // Model the SI DMA latency so the game's osRecvMesg ordering around an
    // accessory transfer holds (a real read/write is two SI DMAs). Latency is
    // overridable via N64RECOMP_SI_DMA_US (default 50us).
    uint32_t si_dma_latency_us() {
        static uint32_t value = []() {
            constexpr uint32_t default_latency_us = 50;
            const char* env = std::getenv("N64RECOMP_SI_DMA_US");
            if (env == nullptr || env[0] == '\0') {
                return default_latency_us;
            }
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(env, &end, 10);
            if (end == env) {
                return default_latency_us;
            }
            return static_cast<uint32_t>(parsed);
        }();
        return value;
    }

    void wait_for_si_dma(uint8_t* rdram, PTR(OSMesgQueue) mq) {
        const uint32_t latency_us = si_dma_latency_us();
        if (latency_us == 0 || mq == NULLPTR || !ultramodern::is_game_thread()) {
            return;
        }
        if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
            ultramodern::sleep_milliseconds((latency_us + 999) / 1000);
            return;
        }
        ultramodern::send_external_message_after(PASS_RDRAM mq, (OSMesg)0, latency_us);
        osRecvMesg(PASS_RDRAM mq, NULLPTR, OS_MESG_BLOCK);
    }

    void wait_for_cont_ram_transfer(uint8_t* rdram, PTR(OSMesgQueue) mq) {
        wait_for_si_dma(rdram, mq);
        wait_for_si_dma(rdram, mq);
    }
}

// s32 __osContRamRead(OSMesgQueue* mq, int channel, u16 address, u8* buffer)
extern "C" void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    const PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    const int channel = _arg<1, s32>(rdram, ctx);
    const uint16_t address = _arg<2, u16>(rdram, ctx);
    const gpr buffer = ctx->r7;
    std::array<uint8_t, librecomp::gbcart::block_size> block{};

    wait_for_cont_ram_transfer(rdram, mq);

    const int ret = librecomp::gbcart::read_block(channel, address, block.data());
    if (ret == 0) {
        for (int i = 0; i < librecomp::gbcart::block_size; i++) {
            MEM_B(i, buffer) = static_cast<int8_t>(block[i]);
        }
    }
    _return<s32>(ctx, ret);
}

// s32 __osContRamWrite(OSMesgQueue* mq, int channel, u16 address, u8* buffer)
extern "C" void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    const PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    const int channel = _arg<1, s32>(rdram, ctx);
    const uint16_t address = _arg<2, u16>(rdram, ctx);
    const gpr buffer = ctx->r7;
    std::array<uint8_t, librecomp::gbcart::block_size> block{};

    for (int i = 0; i < librecomp::gbcart::block_size; i++) {
        block[i] = MEM_BU(i, buffer);
    }
    wait_for_cont_ram_transfer(rdram, mq);

    const int ret = librecomp::gbcart::write_block(channel, address, block.data());
    _return<s32>(ctx, ret);
}

// s32 __osPfsGetStatus(OSMesgQueue* mq, int channel)
extern "C" void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    const PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    const int channel = _arg<1, s32>(rdram, ctx);
    wait_for_cont_ram_transfer(rdram, mq);
    _return<s32>(ctx, librecomp::gbcart::has_pak(channel) ? 0 : 1);
}
