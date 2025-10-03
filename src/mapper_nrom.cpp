// mapper_nrom.cpp
#include "mapper_nrom.h"

uint8_t MapperNROM::cpuRead(uint16_t addr) {
    // Only respond to addresses in 0x8000-0xFFFF
    if (addr < 0x8000) return 0xFF;

    size_t prgSize = prg.size();
    // NROM-128: 16KB PRG, mirror 0x8000-0xBFFF to 0xC000-0xFFFF
    // NROM-256: 32KB PRG, no mirroring
    uint32_t prgAddr = (prgSize == 0x4000) ? ((addr - 0x8000) & 0x3FFF) : (addr - 0x8000);

    if (prgAddr < prgSize)
        return prg[prgAddr];
    return 0xFF;
}

void MapperNROM::cpuWrite(uint16_t, uint8_t) {
    // NROM has no CPU-mapped registers; writes are ignored
}

uint8_t MapperNROM::ppuRead(uint16_t addr) {
    // CHR ROM/RAM is mapped at 0x0000-0x1FFF
    if (addr < 0x2000 && addr < chr.size())
        return chr[addr];
    return 0;
}

void MapperNROM::ppuWrite(uint16_t addr, uint8_t value) {
    // Only allow writes if CHR RAM is present
    if (addr < 0x2000 && hasChrRam && addr < chr.size())
        chr[addr] = value;
}
