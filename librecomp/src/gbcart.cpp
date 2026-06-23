// N64ModernRuntime — Transfer Pak (GB Pak) shared cart model.
//
// Copyright (c) 2026 Matthew Stanley
//
// The Game Boy cartridge model (MBC1/3/5 + battery RAM + save persistence) and
// the Transfer Pak accessory protocol (enable/bank/status registers + the cart
// window), promoted from PokemonStadiumRecomp's app-level transfer_pak.cpp into
// the shared engine so every game emulates one Transfer Pak. The accessory and
// MBC logic is unchanged from that origin.
//
// ---------------------------------------------------------------------
#include "librecomp/gbcart.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#include "ultramodern/ultramodern.hpp"

namespace librecomp::gbcart {
namespace {
    constexpr int port_count = 4;
    constexpr int pfs_err_no_pak = 1;
    constexpr int pfs_err_invalid = 5;
    constexpr uint16_t transfer_pak_address_mask = 0x7FFF;
    constexpr uint16_t transfer_pak_cart_window = 0x4000;

    enum class Mbc {
        RomOnly,
        Mbc1,
        Mbc3,
        Mbc5,
        Unsupported,
    };

    std::vector<uint8_t> read_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {};
        }
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    size_t header_ram_size(uint8_t code) {
        switch (code) {
        case 0x00: return 0;
        case 0x01: return 2 * 1024;
        case 0x02: return 8 * 1024;
        case 0x03: return 32 * 1024;
        case 0x04: return 128 * 1024;
        case 0x05: return 64 * 1024;
        default: return 0;
        }
    }

    struct Cartridge {
        std::filesystem::path rom_path;
        std::filesystem::path save_path;
        std::vector<uint8_t> rom;
        std::vector<uint8_t> ram;
        Mbc mbc = Mbc::Unsupported;
        bool ram_enabled = false;
        bool dirty = false;
        uint8_t rom_bank_low = 1;
        uint8_t bank_high = 0;
        uint8_t ram_bank = 0;
        uint8_t mbc1_mode = 0;
        uint16_t mbc5_rom_bank = 1;

        bool load(const std::filesystem::path& configured_rom,
                  const std::filesystem::path& configured_save) {
            rom_path = configured_rom;
            save_path = configured_save;
            rom = read_file(rom_path);
            if (rom.size() < 0x150) {
                std::fprintf(stderr, "[gbcart] ROM is missing or too small: %s\n",
                             rom_path.string().c_str());
                return false;
            }

            const uint8_t type = rom[0x147];
            if (type == 0x00 || type == 0x08 || type == 0x09) {
                mbc = Mbc::RomOnly;
            } else if (type >= 0x01 && type <= 0x03) {
                mbc = Mbc::Mbc1;
            } else if (type >= 0x0F && type <= 0x13) {
                mbc = Mbc::Mbc3;
            } else if (type >= 0x19 && type <= 0x1E) {
                mbc = Mbc::Mbc5;
            } else {
                std::fprintf(stderr,
                             "[gbcart] unsupported Game Boy cart type 0x%02X: %s\n",
                             type, rom_path.string().c_str());
                return false;
            }

            if (!save_path.empty()) {
                ram = read_file(save_path);
            }
            const size_t declared_ram = header_ram_size(rom[0x149]);
            if (ram.size() < declared_ram) {
                ram.resize(declared_ram, 0xFF);
            }

            std::fprintf(stderr,
                         "[gbcart] loaded ROM %s (%zu bytes, type 0x%02X), save %s (%zu bytes)\n",
                         rom_path.string().c_str(), rom.size(), type,
                         save_path.empty() ? "(none)" : save_path.string().c_str(), ram.size());
            return true;
        }

        size_t rom_index(size_t bank, uint16_t address_in_bank) const {
            if (rom.empty()) {
                return 0;
            }
            const size_t banks = (rom.size() + 0x3FFF) / 0x4000;
            bank %= banks;
            return bank * 0x4000 + address_in_bank;
        }

        size_t selected_switchable_rom_bank() const {
            if (mbc == Mbc::Mbc5) {
                return mbc5_rom_bank;
            }
            if (mbc == Mbc::Mbc1) {
                size_t bank = (rom_bank_low & 0x1F) | ((bank_high & 0x03) << 5);
                if ((bank & 0x1F) == 0) {
                    bank++;
                }
                return bank;
            }
            size_t bank = rom_bank_low & 0x7F;
            return bank == 0 ? 1 : bank;
        }

        size_t selected_ram_offset(uint16_t address) const {
            const size_t offset = address - 0xA000;
            const size_t selected_bank =
                (mbc == Mbc::Mbc1 && mbc1_mode == 0) ? 0 : (ram_bank & 0x03);
            return selected_bank * 0x2000 + offset;
        }

        uint8_t read(uint16_t address) const {
            if (address < 0x4000) {
                size_t bank = 0;
                if (mbc == Mbc::Mbc1 && mbc1_mode != 0) {
                    bank = (bank_high & 0x03) << 5;
                }
                const size_t index = rom_index(bank, address);
                return index < rom.size() ? rom[index] : 0xFF;
            }
            if (address < 0x8000) {
                const size_t index = rom_index(selected_switchable_rom_bank(), address - 0x4000);
                return index < rom.size() ? rom[index] : 0xFF;
            }
            if (address >= 0xA000 && address < 0xC000 && ram_enabled) {
                const size_t index = selected_ram_offset(address);
                return index < ram.size() ? ram[index] : 0xFF;
            }
            return 0xFF;
        }

        void write(uint16_t address, uint8_t value) {
            if (mbc == Mbc::RomOnly) {
                if (address >= 0xA000 && address < 0xC000 && !ram.empty()) {
                    const size_t index = address - 0xA000;
                    if (index < ram.size() && ram[index] != value) {
                        ram[index] = value;
                        dirty = true;
                    }
                }
                return;
            }

            if (address < 0x2000) {
                ram_enabled = (value & 0x0F) == 0x0A;
                return;
            }
            if (address < 0x4000) {
                if (mbc == Mbc::Mbc1) {
                    rom_bank_low = value & 0x1F;
                    if (rom_bank_low == 0) {
                        rom_bank_low = 1;
                    }
                } else if (mbc == Mbc::Mbc5) {
                    if (address < 0x3000) {
                        mbc5_rom_bank = static_cast<uint16_t>(
                            (mbc5_rom_bank & 0x100) | value);
                    } else {
                        mbc5_rom_bank = static_cast<uint16_t>(
                            (mbc5_rom_bank & 0x0FF) | ((value & 1) << 8));
                    }
                } else {
                    rom_bank_low = value & 0x7F;
                    if (rom_bank_low == 0) {
                        rom_bank_low = 1;
                    }
                }
                return;
            }
            if (address < 0x6000) {
                if (mbc == Mbc::Mbc1) {
                    bank_high = value & 0x03;
                    ram_bank = bank_high;
                } else if (mbc == Mbc::Mbc5) {
                    ram_bank = value & 0x0F;
                } else if (value <= 0x03) {
                    ram_bank = value;
                }
                return;
            }
            if (address < 0x8000) {
                if (mbc == Mbc::Mbc1) {
                    mbc1_mode = value & 1;
                }
                return;
            }
            if (address >= 0xA000 && address < 0xC000 && ram_enabled) {
                const size_t index = selected_ram_offset(address);
                if (index < ram.size() && ram[index] != value) {
                    ram[index] = value;
                    dirty = true;
                }
            }
        }

        void flush_save() {
            if (!dirty || save_path.empty()) {
                return;
            }
            std::ofstream file(save_path, std::ios::binary | std::ios::trunc);
            if (!file) {
                std::fprintf(stderr, "[gbcart] failed to write save: %s\n",
                             save_path.string().c_str());
                return;
            }
            file.write(reinterpret_cast<const char*>(ram.data()),
                       static_cast<std::streamsize>(ram.size()));
            if (file) {
                dirty = false;
            }
        }
    };

    struct Port {
        bool configured = false;
        bool pak_enabled = false;
        bool cart_enabled = false;
        uint8_t address_bank = 3;
        uint8_t reset_state = 0;
        Cartridge cart;

        uint8_t status() {
            uint8_t value = 0;
            value |= cart_enabled ? 0x01 : 0;
            value |= static_cast<uint8_t>((reset_state & 0x03) << 2);
            value |= configured ? 0 : 0x40;
            value |= pak_enabled ? 0x80 : 0;

            if (cart_enabled && reset_state == 3) {
                reset_state = 2;
            } else if (!cart_enabled && reset_state == 2) {
                reset_state = 1;
            } else if (!cart_enabled && reset_state == 1) {
                reset_state = 0;
            }
            return value;
        }

        uint8_t read(uint16_t accessory_address) {
            accessory_address &= transfer_pak_address_mask;
            if (!pak_enabled) {
                return 0;
            }
            if (accessory_address <= 0x1FFF) {
                return 0x84;
            }
            if (accessory_address <= 0x2FFF) {
                return address_bank;
            }
            if (accessory_address <= 0x3FFF) {
                return status();
            }
            if (!cart_enabled) {
                return 0;
            }
            const uint16_t bus_address = static_cast<uint16_t>(
                transfer_pak_cart_window * address_bank +
                accessory_address - transfer_pak_cart_window);
            return cart.read(bus_address);
        }

        void write(uint16_t accessory_address, uint8_t value) {
            accessory_address &= transfer_pak_address_mask;
            if (accessory_address <= 0x1FFF) {
                const bool was_enabled = pak_enabled;
                if (value == 0x84) {
                    pak_enabled = true;
                } else if (value == 0xFE) {
                    pak_enabled = false;
                }
                if (!was_enabled && pak_enabled) {
                    address_bank = 3;
                    cart_enabled = false;
                    reset_state = 0;
                }
                return;
            }
            if (!pak_enabled) {
                return;
            }
            if (accessory_address <= 0x2FFF) {
                address_bank = value > 3 ? 0 : value;
                return;
            }
            if (accessory_address <= 0x3FFF) {
                const bool was_enabled = cart_enabled;
                cart_enabled = (value & 1) != 0;
                if (!was_enabled && cart_enabled) {
                    // Settle to reset_state 2 = RSTB_DETECTION(0x08) SET, RSTB_STATUS(0x04)
                    // CLEAR. On real HW the GB cart's reset line de-asserts (0x04 clears)
                    // before the N64 reads status, while 0x08 latches "a reset happened".
                    // The GB-Tower cart-check needs BOTH: FUN_8000bdc0 requires status&0x08
                    // set, and the SRAM self-test verify (FUN_8000ba20) requires status&0x04
                    // CLEAR. State 3 (both set) fails the self-test -> red-X "save corrupt".
                    reset_state = 2;
                }
                return;
            }
            if (!cart_enabled) {
                return;
            }
            const uint16_t bus_address = static_cast<uint16_t>(
                transfer_pak_cart_window * address_bank +
                accessory_address - transfer_pak_cart_window);
            cart.write(bus_address, value);
        }
    };

    std::array<Port, port_count> ports;
    std::mutex port_mutex;

    // ---- Always-on diagnostic ring (Transfer Pak block I/O) -------------------
    // Records every read_block/write_block with the cart's bank state so a probe
    // can see exactly which ROM banks / addresses a feature walks (e.g. GB Tower's
    // full-cart read for emulation) and where it stalls — WITHOUT perturbing boot
    // timing the way per-event stderr logging does. Queried via ring_dump().
    struct RingEntry {
        uint32_t seq;
        uint32_t tstamp_us;   // microseconds since first ring op (timing / yield gaps)
        uint32_t thread_ptr;  // OSThread* (guest addr) that issued this op (race attribution)
        uint16_t block_addr;  // libultra block address (x32 = accessory byte addr)
        uint16_t rom_bank;    // cart's currently-selected switchable ROM bank
        uint8_t  port;
        uint8_t  kind;        // 0 = read, 1 = write
        uint8_t  addr_bank;   // Transfer Pak bank register (0..3)
        uint8_t  flags;       // bit0 pak_enabled, bit1 cart_enabled, bit2 ram_enabled
        uint8_t  ram_bank;
        uint8_t  mbc1_mode;   // MBC1 banking mode (0 = ROM, 1 = RAM) — bank-race relevant
        uint8_t  b0;          // first data byte read/written
    };
    constexpr size_t ring_cap = 65536;
    RingEntry g_ring[ring_cap];
    uint32_t g_ring_seq = 0;   // total appended (== next seq); guarded by port_mutex

    uint32_t ring_now_us() {
        static const auto t0 = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }

    // Caller holds port_mutex. `port` is valid + configured.
    void ring_append(int port, uint8_t kind, uint16_t block_addr, uint8_t b0) {
        const Port& p = ports[port];
        RingEntry& e = g_ring[g_ring_seq % ring_cap];
        e.seq = g_ring_seq;
        e.tstamp_us = ring_now_us();
        e.thread_ptr = static_cast<uint32_t>(ultramodern::this_thread());
        e.block_addr = block_addr;
        e.rom_bank = static_cast<uint16_t>(p.cart.selected_switchable_rom_bank());
        e.port = static_cast<uint8_t>(port);
        e.kind = kind;
        e.addr_bank = p.address_bank;
        e.flags = static_cast<uint8_t>((p.pak_enabled ? 1 : 0) | (p.cart_enabled ? 2 : 0) |
                                       (p.cart.ram_enabled ? 4 : 0));
        e.ram_bank = p.cart.ram_bank;
        e.mbc1_mode = static_cast<uint8_t>(p.cart.mbc1_mode);
        e.b0 = b0;
        g_ring_seq++;
    }
}

void set_cart(int port, const std::filesystem::path& rom, const std::filesystem::path& save) {
    if (port < 0 || port >= port_count) {
        return;
    }
    std::scoped_lock lock(port_mutex);
    Port& p = ports[port];
    if (rom.empty()) {
        p = Port{}; // clear
        return;
    }
    p = Port{};
    p.configured = p.cart.load(rom, save);
    if (p.configured) {
        std::fprintf(stderr, "[gbcart] port %d configured\n", port + 1);
    }
}

bool has_pak(int port) {
    std::scoped_lock lock(port_mutex);
    return port >= 0 && port < port_count && ports[port].configured;
}

// Diagnostic: env-gated trace of the GB-Tower SRAM self-test (PSR_GBSELFTEST=1).
static bool gbselftest_trace() {
    static const bool on = [] {
        const char* e = std::getenv("PSR_GBSELFTEST");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return on;
}

int read_block(int port, uint16_t block_address, uint8_t* out) {
    std::scoped_lock lock(port_mutex);
    if (port < 0 || port >= port_count || !ports[port].configured) {
        return pfs_err_no_pak;
    }
    if (out == nullptr) {
        return pfs_err_invalid;
    }
    const uint16_t byte_address = static_cast<uint16_t>(block_address * block_size);
    // Transfer Pak control registers (enable/bank/status, accessory < 0xC000 =>
    // masked <= 0x3FFF) are single-byte registers MIRRORED across the whole
    // 32-byte block on real hardware, and reading the status register advances
    // its reset-ack state machine exactly ONCE per read. Read once and replicate.
    // Reading them per-byte would call status() 32x for a single status read,
    // draining reset_state and clearing the 0x08 "reset-ready" bit by byte 0x1f
    // -> PMS-J's GB-Tower cart-check (FUN_8000bdc0: redX if (status[0x1f]&8)==0)
    // wrongly reports every cart "corrupt". The GB memory window stays per-byte.
    if ((byte_address & transfer_pak_address_mask) <= 0x3FFF) {
        const uint8_t v = ports[port].read(byte_address);
        for (int i = 0; i < block_size; i++) {
            out[i] = v;
        }
        if (gbselftest_trace() && (byte_address & transfer_pak_address_mask) >= 0x3000) {
            const Port& pp = ports[port];
            std::fprintf(stderr,
                "[gbself] RD STATUS val=0x%02x (08=%d 04=%d 80=%d 40=%d) cart=%d reset=%d\n",
                v, (v&8)!=0, (v&4)!=0, (v&0x80)!=0, (v&0x40)!=0, pp.cart_enabled, pp.reset_state);
        }
    } else {
        for (int i = 0; i < block_size; i++) {
            out[i] = ports[port].read(static_cast<uint16_t>(byte_address + i));
        }
        const Port& pp = ports[port];
        if (gbselftest_trace() && pp.address_bank == 2) {  // SRAM window read (self-test read-back)
            char hx[80]; int n = 0;
            for (int i = 0; i < block_size; i++)
                n += std::snprintf(hx + n, sizeof(hx) - n, "%02x", out[i]);
            std::fprintf(stderr, "[gbself] RD SRAM ram_en=%d rb%u gb=0x%04x %s\n",
                pp.cart.ram_enabled, pp.cart.ram_bank, byte_address, hx);
        }
    }
    ring_append(port, 0, block_address, out[0]);
    return 0;
}

int write_block(int port, uint16_t block_address, const uint8_t* data) {
    std::scoped_lock lock(port_mutex);
    if (port < 0 || port >= port_count || !ports[port].configured) {
        return pfs_err_no_pak;
    }
    if (data == nullptr) {
        return pfs_err_invalid;
    }
    const uint16_t byte_address = static_cast<uint16_t>(block_address * block_size);
    for (int i = 0; i < block_size; i++) {
        ports[port].write(static_cast<uint16_t>(byte_address + i), data[i]);
    }
    const Port& wp = ports[port];
    if (gbselftest_trace() && wp.address_bank == 2 &&
        (byte_address & transfer_pak_address_mask) >= 0x4000) {  // SRAM window write (self-test)
        char hx[80]; int n = 0;
        for (int i = 0; i < block_size; i++)
            n += std::snprintf(hx + n, sizeof(hx) - n, "%02x", data[i]);
        std::fprintf(stderr, "[gbself] WR SRAM ram_en=%d rb%u gb=0x%04x %s\n",
            wp.cart.ram_enabled, wp.cart.ram_bank, byte_address, hx);
    }
    ring_append(port, 1, block_address, data[0]);
    ports[port].cart.flush_save();
    return 0;
}

void flush_all() {
    std::scoped_lock lock(port_mutex);
    for (Port& p : ports) {
        if (p.configured) {
            p.cart.flush_save();
        }
    }
}

void ring_dump(const char* path, int tail) {
    std::scoped_lock lock(port_mutex);
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        return;
    }
    const uint32_t total = g_ring_seq;
    const uint32_t avail = total < ring_cap ? total : static_cast<uint32_t>(ring_cap);
    if (tail <= 0 || static_cast<uint32_t>(tail) > avail) {
        tail = static_cast<int>(avail);
    }

    // Summary over the whole live window: which ROM banks were read (full-cart
    // walk detection), read/write counts, and the cart-window byte-address span.
    uint32_t reads = 0, writes = 0;
    uint64_t bank_seen = 0;       // bitmask of rom_banks 0..63 touched by reads
    uint32_t max_bank = 0;
    uint32_t max_blk = 0;
    const uint32_t start = total > ring_cap ? total - static_cast<uint32_t>(ring_cap) : 0;
    for (uint32_t s = start; s < total; s++) {
        const RingEntry& e = g_ring[s % ring_cap];
        if (e.kind == 0) {
            reads++;
            if (e.rom_bank < 64) bank_seen |= (1ull << e.rom_bank);
            if (e.rom_bank > max_bank) max_bank = e.rom_bank;
            if (e.block_addr > max_blk) max_blk = e.block_addr;
        } else {
            writes++;
        }
    }
    f << "=== gbcart block-I/O ring (total=" << total << " avail=" << avail
      << " reads=" << reads << " writes=" << writes << ") ===\n";
    f << "rom_banks_read (max=" << max_bank << "): ";
    for (int b = 0; b < 64; b++) {
        if (bank_seen & (1ull << b)) f << b << ' ';
    }
    f << "\nmax read block_addr=0x" << std::hex << max_blk << std::dec
      << " (acc byte 0x" << std::hex << (max_blk * block_size) << std::dec << ")\n";
    f << "--- last " << tail << " entries (acc=accessory byte addr, bus=GB bus addr; "
         "thr=OSThread* (map via dump_sched), us=microseconds, m=MBC1 mode) ---\n";
    f << "seq us thr port kind tpb rom_bank ram_bank mbc1 acc bus b0 flags(P/C/R)\n";
    const uint32_t first = total - static_cast<uint32_t>(tail);
    for (uint32_t s = first; s < total; s++) {
        const RingEntry& e = g_ring[s % ring_cap];
        const uint32_t acc = static_cast<uint32_t>(e.block_addr) * block_size;
        const uint32_t accm = acc & 0x7FFF;
        const uint32_t bus = (accm >= 0x4000)
            ? (0x4000u * e.addr_bank + accm - 0x4000u)
            : accm;
        char buf[224];
        std::snprintf(buf, sizeof(buf),
            "%u us=%u thr=%08x P%u %s tpb%u rom%-3u rb%u m%u 0x%04x  0x%04x  %02x  %c%c%c\n",
            e.seq, e.tstamp_us, e.thread_ptr, e.port, e.kind == 0 ? "RD" : "WR",
            e.addr_bank, e.rom_bank, e.ram_bank, e.mbc1_mode, acc, bus, e.b0,
            (e.flags & 1) ? 'P' : '-', (e.flags & 2) ? 'C' : '-', (e.flags & 4) ? 'R' : '-');
        f << buf;
    }
}

} // namespace librecomp::gbcart
