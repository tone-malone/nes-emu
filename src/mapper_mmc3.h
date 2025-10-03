// mapper_mmc3.h

#pragma once
#include <array>
#include <vector>
#include "mapper.h"

struct MapperMMC3 : Mapper {
    std::vector<uint8_t> prg, chr;
    std::vector<uint8_t> prgRAM;  // $6000–7FFF
    bool chrIsRAM = false;        // if true, chr[] is RAM
    uint8_t mir = 0;

    uint8_t bankSelect = 0;
    std::array<uint8_t, 8> bank{};
    bool prgMode = false, chrMode = false;
    uint8_t prgRAMEnable = 0xA0;

    // IRQ state
    uint8_t irqLatch = 0, irqCounter = 0;
    bool irqEnable = false, irqReload = false, irqFlag = false;

    // A12 low-time filter (12 PPU cycles)
    bool prevA12 = false;
    int a12LowCycles = 0;
    bool sawRiseThisLine = false;


    MapperMMC3(std::vector<uint8_t> prg_, std::vector<uint8_t> chr_, uint8_t mir_, uint32_t prgRamKB);

    uint8_t cpuRead(uint16_t a) override;
    void cpuWrite(uint16_t a, uint8_t v) override;
    uint8_t ppuRead(uint16_t a) override;
    void ppuWrite(uint16_t a, uint8_t v) override;
    uint8_t mirroring() const override { return mir; }

    // IRQ hooks
    bool irqPending() const override { return irqFlag; }
    void irqAck() override { irqFlag = false; }
    void ppuA12Clock(bool level) override;
    void ppuOnScanlineDot260(bool rendering) override;

    // Save surface
    uint8_t* prgRamData() override { return prgRAM.data(); }
    size_t prgRamSize() const override { return prgRAM.size(); }
};
