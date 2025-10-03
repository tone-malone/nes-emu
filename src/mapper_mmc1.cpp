// mapper_mmc1.cpp
#include "mapper_mmc1.h"

inline uint32_t chrSizeMask(uint32_t sz) {  // sz is bytes
    // chr.size() or chrRAM.size() can be non power-of-two; just modulo later is fine
    return sz - 1;
}

static inline uint32_t mmc1_map_chr(uint16_t a, uint8_t ctrl,
                                    uint8_t chr0, uint8_t chr1) {
    const bool chr4k = (ctrl & 0x10) != 0;  // 1=4KB banks, 0=8KB
    if (!chr4k) {
        // 8KB: chr0 selects an even 8KB bank (bit0 ignored)
        uint32_t base = uint32_t(chr0 & 0x1E) * 0x1000u;
        return base + (a & 0x1FFFu);
    } else {
        if (a < 0x1000) {
            uint32_t base = uint32_t(chr0 & 0x1F) * 0x1000u;  // 4KB steps
            return base + (a & 0x0FFFu);
        } else {
            uint32_t base = uint32_t(chr1 & 0x1F) * 0x1000u;
            return base + (a & 0x0FFFu);
        }
    }
}

// ------------------------
// MMC1 constructor
// ------------------------
MapperMMC1::MapperMMC1(std::vector<uint8_t> prg_, std::vector<uint8_t> chr_,
                       bool chrRam, uint8_t mir_, uint32_t prgRamKB)
    : prg(std::move(prg_)),
      chr(std::move(chr_)),  // keep CHR-ROM data here if present
      chrIsRAM(chrRam || chr.empty()),
      mir(mir_) {
    if (chrIsRAM) {
        if (chrRAM.empty()) {
            // If the loader already sized 'chr' for RAM, mirror that;
            // otherwise default to 8KB.
            size_t sz = chr.size() ? chr.size() : (8 * 1024);
            chrRAM.assign(sz, 0);
        }
    }

    prgRamPresent = true;
    const uint32_t sizeKB = (prgRamKB ? prgRamKB : 8);
    prgRAM.resize(sizeKB * 1024, 0);

    ctrl = (ctrl & ~0x0C) | 0x0C;   // force PRG mode=3; keep mirroring/CHR mode the same
    loadReg = 0;
    loadCount = 0;
    prgBank = 0;
    chrBank0 = 0;
    chrBank1 = 0;
    prgRamWriteEnabled = true;
}

// MMC1 mirroring decoding:
// ctrl[1:0] = 0: single A, 1: single B, 2: vertical, 3: horizontal
uint8_t MapperMMC1::mirroring() const {
    switch (ctrl & 0x03) {
        case 0:
            return 2;  // single-screen A
        case 1:
            return 3;  // single-screen B
        case 2:
            return 1;  // vertical
        case 3:
            return 0;  // horizontal
        default:
            return 1;
    }
}

// ------------------------
// CPU bus
// ------------------------
uint8_t MapperMMC1::cpuRead(uint16_t a) {
    if (a >= 0x6000 && a < 0x8000) {
        if (!prgRamPresent) return 0xFF;
        return prgRAM[a - 0x6000];
    }
    if (a < 0x8000) return 0xFF;

    const uint8_t prgMode = (ctrl >> 2) & 0x03;
    const size_t prgSize = prg.size();
    const size_t num16k = prgSize / 0x4000 ? prgSize / 0x4000 : 1;

    auto rd16 = [&](uint32_t bank, uint16_t off) -> uint8_t {
        uint32_t base = (bank % num16k) * 0x4000u;
        return prg[(base + (off & 0x3FFFu)) % prgSize];
    };

    if (prgMode <= 1) {
        uint32_t bank32 = (prgBank & 0x0F) & ~1u;
        return (a < 0xC000) ? rd16(bank32 + 0, (uint16_t)(a - 0x8000))
                            : rd16(bank32 + 1, (uint16_t)(a - 0xC000));
    } else if (prgMode == 2) {
        return (a < 0xC000) ? rd16(0, (uint16_t)(a - 0x8000))
                            : rd16(prgBank & 0x0F, (uint16_t)(a - 0xC000));
    } else {
        size_t last = num16k ? num16k - 1 : 0;
        return (a < 0xC000) ? rd16(prgBank & 0x0F, (uint16_t)(a - 0x8000))
                            : rd16((uint32_t)last, (uint16_t)(a - 0xC000));
    }
}

void MapperMMC1::cpuWrite(uint16_t a, uint8_t v) {
    if (a >= 0x6000 && a < 0x8000) {
        if (prgRamPresent && prgRamWriteEnabled) prgRAM[a - 0x6000] = v;
        return;
    }
    if (a < 0x8000) return;

    if (v & 0x80) {
        // Reset shift register + set safe control
        loadReg = 0;
        loadCount = 0;
        ctrl |= 0x0C;  // fix PRG banking to last page
        return;
    }

    // LSB-first into 5-bit load register
    loadReg = (uint8_t)((loadReg >> 1) | ((v & 1) << 4));
    loadCount++;

    if (loadCount < 5) return;

    // Commit 5 bits
    uint8_t data = loadReg & 0x1F;
// Correct: pick one of the four 8KB regions
int reg = ((a - 0x8000) >> 13) & 0x03;   // ✅ 0:ctrl, 1:CHR0, 2:CHR1, 3:PRG

    switch (reg) {
        case 0:
            ctrl = data;
            break;
        case 1:
            chrBank0 = data;
            break;
        case 2:
            chrBank1 = data;
            break;
        case 3: {
            prgBank = (uint8_t)(data & 0x0F);
            // Bit4 is PRG-RAM write-protect (1=disable writes)
            prgRamWriteEnabled = ((data & 0x10) == 0);
        } break;
    }
    // Clear for next sequence
    loadReg = 0;
    loadCount = 0;
}

// ------------------------
// PPU bus (CHR)
// READ
uint8_t MapperMMC1::ppuRead(uint16_t a) {
    if (a >= 0x2000) return 0;
    const uint32_t idx = mmc1_map_chr(a, ctrl, chrBank0, chrBank1);
    if (chrIsRAM)  return chrRAM[idx % chrRAM.size()];
    else           return chr[idx   % chr.size()];
}

// WRITE
void MapperMMC1::ppuWrite(uint16_t a, uint8_t v) {
    if (a >= 0x2000) return;
    if (!chrIsRAM)   return;                    // CHR-ROM ignores writes
    const uint32_t idx = mmc1_map_chr(a, ctrl, chrBank0, chrBank1);
    chrRAM[idx % chrRAM.size()] = v;
}