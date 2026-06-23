// N64ModernRuntime — Transfer Pak (GB Pak) shared cart model.
//
// Copyright (c) 2026 Matthew Stanley
//
// One game-agnostic implementation of the N64 Transfer Pak's Game Boy cartridge
// for every recompiled game. The per-game HLE layers — the libultra accessory
// API (gbpak.cpp) and the SI/joybus processor (si_joybus.cpp) — and the app
// (cart registration via set_cart) sit on top of this. See
// PocketMonstersStadiumRecomp/docs/transfer_pak_universal_design.md.
//
// ---------------------------------------------------------------------
#ifndef LIBRECOMP_GBCART_HPP
#define LIBRECOMP_GBCART_HPP

#include <cstdint>
#include <filesystem>

namespace librecomp::gbcart {
    // libultra accessory block size (bytes) — the unit of read_block/write_block.
    constexpr int block_size = 32;

    // Mount (or, with empty paths, clear) the Game Boy cart in the Transfer Pak
    // on N64 controller port `port` (0..3). Called by the app — e.g. the
    // launcher — from its per-port config. Thread-safe.
    void set_cart(int port, const std::filesystem::path& rom, const std::filesystem::path& save);

    // True iff a cart is mounted + loaded on this port.
    bool has_pak(int port);

    // Read/write one accessory block (block_size bytes) at libultra block
    // address `block_address`. `out`/`data` point to block_size bytes. Returns 0
    // on success, or a libultra PFS error code (1 = no pak, 5 = invalid arg).
    int read_block(int port, uint16_t block_address, uint8_t* out);
    int write_block(int port, uint16_t block_address, const uint8_t* data);

    // Persist any dirty cart RAM to disk. Writes are already write-through;
    // this is a shutdown backstop (call from the quit path).
    void flush_all();

    // Diagnostic: dump the always-on block-I/O ring (last `tail` entries + a
    // summary of which ROM banks were read) to `path`. Lets a probe see exactly
    // which cart addresses/banks a feature (e.g. GB Tower's full-cart read) walks
    // and where it stalls, without perturbing boot timing.
    void ring_dump(const char* path, int tail);
}

#endif // LIBRECOMP_GBCART_HPP
