// N64ModernRuntime — Transfer Pak SI/joybus processor (Level B).
//
// Copyright (c) 2026 Matthew Stanley
//
// The universal Transfer Pak seam (see
// PocketMonstersStadiumRecomp/docs/transfer_pak_universal_design.md). Every
// game funnels all accessory I/O through __osSiRawStartDma, which DMAs a 64-byte
// PIF command block to the joybus. This reimplements that primitive: it parses
// the command block and answers the accessory read/write/info joybus commands
// from the shared cart model (librecomp::gbcart). A game with a custom-named
// accessory path (e.g. Pocket Monsters Stadium's block loop) binds to the shared
// Transfer Pak by naming just this one universal primitive.
//
// Controller button reads (joybus cmd 0x01) are HLE'd higher up (osContStartReadData
// in cont.cpp) and do not reach here; if one does, we answer idle.
//
// ---------------------------------------------------------------------
#include <array>
#include <cstdint>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"
#include "librecomp/ultra_trace.hpp"
#include "librecomp/gbcart.hpp"

namespace {
    // Joybus command bytes.
    constexpr uint8_t JOY_INFO  = 0x00; // device type + status (3 rx)
    constexpr uint8_t JOY_STATE = 0x01; // controller buttons (4 rx)
    constexpr uint8_t JOY_READ  = 0x02; // read 32-byte accessory block (+ crc)
    constexpr uint8_t JOY_WRITE = 0x03; // write 32-byte accessory block (-> crc)

    // Flag set in the rxsize byte of a response when no device answered.
    constexpr uint8_t RX_NO_DEVICE = 0x80;

    // Standard N64 8-bit accessory data CRC (libultra __osContDataCrc, poly 0x85).
    uint8_t data_crc(const uint8_t* data) {
        uint32_t crc = 0;
        for (int i = 0; i <= 32; i++) {
            for (int mask = 0x80; mask != 0; mask >>= 1) {
                const uint32_t xorv = (crc & 0x80) ? 0x85u : 0u;
                crc <<= 1;
                if (i < 32 && (data[i] & mask)) {
                    crc |= 1;
                }
                crc ^= xorv;
            }
        }
        return static_cast<uint8_t>(crc);
    }

    // Parse the 64-byte PIF command block at `pif` in RDRAM and fill the accessory
    // responses in place. Channel index = N64 controller port (0..3) = gbcart port.
    void run_joybus(uint8_t* rdram, gpr pif) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            blk[i] = MEM_BU(i, pif);
        }

        int chan = 0;
        int p = 0;
        while (p < 63) {
            const uint8_t t = blk[p];
            if (t == 0xFE) break;                       // end of commands
            if (t == 0xFF) { p++; continue; }           // pad / format byte
            if (t == 0x00) { p++; chan++; continue; }   // skip this channel
            if (t == 0xFD) { p++; continue; }           // channel reset

            const int tx = t & 0x3F;
            const int rx = blk[p + 1] & 0x3F;
            const int cmd_i = p + 2;
            const int rx_i = p + 2 + tx;
            if (rx_i + rx > 64) break;                  // malformed frame
            if (chan < 0 || chan > 3) { p = rx_i + rx; chan++; continue; }

            const uint8_t cmd = blk[cmd_i];
            bool no_device = false;

            if (cmd == JOY_INFO) {
                // Standard controller type (0x0500) + status; pak-present bit from
                // the shared cart model so the game's Transfer Pak detect succeeds.
                if (rx >= 3) {
                    blk[rx_i + 0] = 0x05;
                    blk[rx_i + 1] = 0x00;
                    blk[rx_i + 2] = librecomp::gbcart::has_pak(chan) ? 0x01 : 0x00;
                }
            } else if (cmd == JOY_READ) {
                // tx = [cmd][addr_hi][addr_lo]; rx = 32 data + 1 crc.
                const uint16_t addr = static_cast<uint16_t>((blk[cmd_i + 1] << 8) | blk[cmd_i + 2]);
                std::array<uint8_t, 32> data{};
                const int ret = librecomp::gbcart::read_block(chan, static_cast<uint16_t>(addr >> 5), data.data());
                if (ret == 0 && rx >= 33) {
                    for (int i = 0; i < 32; i++) {
                        blk[rx_i + i] = data[i];
                    }
                    blk[rx_i + 32] = data_crc(data.data());
                } else {
                    no_device = true;
                }
            } else if (cmd == JOY_WRITE) {
                // tx = [cmd][addr_hi][addr_lo][32 data]; rx = 1 crc.
                const uint16_t addr = static_cast<uint16_t>((blk[cmd_i + 1] << 8) | blk[cmd_i + 2]);
                std::array<uint8_t, 32> data{};
                for (int i = 0; i < 32; i++) {
                    data[i] = blk[cmd_i + 3 + i];
                }
                const int ret = librecomp::gbcart::write_block(chan, static_cast<uint16_t>(addr >> 5), data.data());
                if (ret == 0 && rx >= 1) {
                    blk[rx_i + 0] = data_crc(data.data());
                } else {
                    no_device = true;
                }
            } else if (cmd == JOY_STATE) {
                // Should be HLE'd at osContStartReadData; answer idle if reached.
                for (int i = 0; i < rx; i++) {
                    blk[rx_i + i] = 0;
                }
            }

            if (no_device) {
                blk[p + 1] = static_cast<uint8_t>(blk[p + 1] | RX_NO_DEVICE);
            }

            p = rx_i + rx;
            chan++;
        }

        for (int i = 0; i < 64; i++) {
            MEM_B(i, pif) = static_cast<int8_t>(blk[i]);
        }
    }
}

// void __osSiRawStartDma(s32 direction, void* dramAddr)
//   direction OS_READ(0): the joybus responses are about to be copied PIF->RDRAM.
//   We process the command block (still in the RDRAM buffer from the preceding
//   OS_WRITE) and fill the responses in place. OS_WRITE(1) is a no-op for HLE.
extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    LIBRECOMP_ULTRA_TRACE(ctx);
    const s32 direction = _arg<0, s32>(rdram, ctx);
    const gpr dram_addr = ctx->r5;
    if (direction == 0 /* OS_READ */) {
        run_joybus(rdram, dram_addr);
    }
    _return<s32>(ctx, 0);
}
