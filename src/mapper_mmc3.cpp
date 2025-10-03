// mapper_mmc3.cpp
#include "mapper_mmc3.h"

static inline uint32_t prgBankAddr(const std::vector<uint8_t>& prg, uint32_t bank, uint32_t off) {
    if (prg.empty()) return 0;
    uint32_t bankCount = (uint32_t)prg.size() / 0x2000u;
    uint32_t b = bankCount ? (bank % bankCount) : 0;
    return (b * 0x2000u + (off & 0x1FFFu)) % (uint32_t)prg.size();
}

MapperMMC3::MapperMMC3(std::vector<uint8_t> prg_, std::vector<uint8_t> chr_, uint8_t mir_, uint32_t prgRamKB)
    : prg(std::move(prg_)), chr(std::move(chr_)), mir(mir_) {
    bank.fill(0);

    chrIsRAM = chr.empty();              // <-- true only if no CHR ROM in the file
    if (chrIsRAM) chr.resize(8 * 1024);  // allocate 8K CHR-RAM

    prgRAM.resize(prgRamKB ? prgRamKB * 1024 : 8 * 1024);
    prgRAMEnable = 0x80;
}

uint8_t MapperMMC3::cpuRead(uint16_t a) {
    if (a >= 0x6000 && a < 0x8000) {
        // Reads usually allowed even if writes are disabled
        return prgRAM[a - 0x6000];
    }
    if (a >= 0x8000) {
        uint32_t off = a & 0x1FFF;
        uint8_t b6 = bank[6] & 0x3F;
        uint8_t b7 = bank[7] & 0x3F;
        uint32_t last = (uint32_t)prg.size() / 0x2000 - 1;

        if (!prgMode) {
            if (a < 0xA000)
                return prg[prgBankAddr(prg, b6, off)];
            else if (a < 0xC000)
                return prg[prgBankAddr(prg, b7, off)];
            else if (a < 0xE000)
                return prg[prgBankAddr(prg, last - 1, off)];
            else
                return prg[prgBankAddr(prg, last, off)];
        } else {
            if (a < 0xA000)
                return prg[prgBankAddr(prg, last - 1, off)];
            else if (a < 0xC000)
                return prg[prgBankAddr(prg, b7, off)];
            else if (a < 0xE000)
                return prg[prgBankAddr(prg, b6, off)];
            else
                return prg[prgBankAddr(prg, last, off)];
        }
    }
    return 0xFF;
}

void MapperMMC3::cpuWrite(uint16_t a, uint8_t v) {
    if (a >= 0x6000 && a < 0x8000) {
        // Write only if enabled (bit7=1) and not write-protected (bit6=0)
        if ((prgRAMEnable & 0x80) && ((prgRAMEnable & 0x40) == 0))
            prgRAM[a - 0x6000] = v;
        return;
    }
    if (a < 0x8000) return;

    switch (a & 0xE001) {
        case 0x8000:
            bankSelect = v & 0x07;
            prgMode = (v & 0x40) != 0;
            chrMode = (v & 0x80) != 0;
            break;
        case 0x8001: {
            uint8_t idx = bankSelect & 7;
            uint8_t val = v;
            if (idx <= 1) val &= 0xFE;  // 2KB banks force even
            bank[idx] = val;
        } break;
        case 0xA000:
            // 0 = vertical, 1 = horizontal (our PPU uses 0=horiz,1=vert)
            mir = (v & 1) ? 0 : 1;
            break;
        case 0xA001:
            prgRAMEnable = v;
            break;
        case 0xC000:
            irqLatch = v;
            break;
        case 0xC001:
            irqReload = true;  // reload on next A12 rising edge
            break;
        case 0xE000:
            irqEnable = false;
            irqFlag = false;  // ACK on write
            break;
        case 0xE001:
            irqEnable = true;
            break;
    }
}

uint8_t MapperMMC3::ppuRead(uint16_t a) {
    if (a >= 0x2000) return 0;

    auto rd1k = [&](uint8_t b, uint32_t o) -> uint8_t {
        return chr[((uint32_t)b * 0x0400u + (o & 0x03FFu)) % chr.size()];
    };
    auto rd2k = [&](uint8_t bEven, uint32_t o) -> uint8_t {
        uint32_t base = (uint32_t)(bEven & 0xFE) * 0x0400u;
        return chr[(base + (o & 0x07FFu)) % chr.size()];
    };

    if (!chrMode) {
        if (a < 0x0800)
            return rd2k(bank[0], a & 0x07FF);
        else if (a < 0x1000)
            return rd2k(bank[1], a & 0x07FF);
        uint8_t idx = 2 + ((a - 0x1000) >> 10);  // 2..5
        return rd1k(bank[idx], a & 0x03FF);
    } else {
        if (a < 0x1000) {
            uint8_t idx = 2 + (a >> 10);  // 2..5
            return rd1k(bank[idx], a & 0x03FF);
        }
        if (a < 0x1800)
            return rd2k(bank[0], a & 0x07FF);
        else
            return rd2k(bank[1], a & 0x07FF);
    }
}

void MapperMMC3::ppuWrite(uint16_t a, uint8_t v) {
    if (a >= 0x2000 || !chrIsRAM) return;

    auto wr1k = [&](uint8_t b, uint32_t o, uint8_t val) {
        chr[((uint32_t)b * 0x0400u + (o & 0x03FFu)) % chr.size()] = val;
    };
    auto wr2k = [&](uint8_t bEven, uint32_t o, uint8_t val) {
        uint32_t base = (uint32_t)(bEven & 0xFE) * 0x0400u;
        chr[(base + (o & 0x07FFu)) % chr.size()] = val;
    };

    if (!chrMode) {
        if (a < 0x0800)
            wr2k(bank[0], a & 0x07FF, v);
        else if (a < 0x1000)
            wr2k(bank[1], a & 0x07FF, v);
        else
            wr1k(bank[2 + ((a - 0x1000) >> 10)], a & 0x03FF, v);
    } else {
        if (a < 0x1000)
            wr1k(bank[2 + (a >> 10)], a & 0x03FF, v);
        else if (a < 0x1800)
            wr2k(bank[0], a & 0x07FF, v);
        else
            wr2k(bank[1], a & 0x07FF, v);
    }
}

void MapperMMC3::ppuA12Clock(bool level) {
    if (!level) {
        if (a12LowCycles < 64) a12LowCycles++;
    } else {
        if (!prevA12) {                 // rising edge
            if (a12LowCycles >= 8) {    // 8-dot filter
                if (irqReload) { irqCounter = irqLatch; irqReload = false; }
                else if (irqCounter == 0) irqCounter = irqLatch;
                else irqCounter--;

                if (irqCounter == 0 && irqEnable) irqFlag = true;
                sawRiseThisLine = true;       // <-- mark it
            }
        }
        a12LowCycles = 0;
    }
    prevA12 = level;
}

void MapperMMC3::ppuOnScanlineDot260(bool rendering) {
    if (!rendering) { sawRiseThisLine = false; return; }  // reset when not rendering

    // Only synthesize the clock if no valid edge happened this line
    if (!sawRiseThisLine) {
        if (irqReload) { irqCounter = irqLatch; irqReload = false; }
        else if (irqCounter == 0) irqCounter = irqLatch;
        else irqCounter--;

        if (irqCounter == 0 && irqEnable) irqFlag = true;
    }
    sawRiseThisLine = false;  // prep for next line
}
