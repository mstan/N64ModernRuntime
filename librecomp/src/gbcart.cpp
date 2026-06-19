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
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

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
                    reset_state = 3;
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

int read_block(int port, uint16_t block_address, uint8_t* out) {
    std::scoped_lock lock(port_mutex);
    if (port < 0 || port >= port_count || !ports[port].configured) {
        return pfs_err_no_pak;
    }
    if (out == nullptr) {
        return pfs_err_invalid;
    }
    const uint16_t byte_address = static_cast<uint16_t>(block_address * block_size);
    for (int i = 0; i < block_size; i++) {
        out[i] = ports[port].read(static_cast<uint16_t>(byte_address + i));
    }
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

} // namespace librecomp::gbcart
