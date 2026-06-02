#pragma once

#include <cstdint>

namespace recomp::rdp {

struct DpRegisters {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t current = 0;
    uint32_t status = 0;
    uint32_t clock = 0;
    uint32_t bufbusy = 0;
    uint32_t pipebusy = 0;
    uint32_t tmem = 0;
};

DpRegisters& dp_registers();

} // namespace recomp::rdp
