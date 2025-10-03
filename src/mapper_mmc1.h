// mapper_mmc1.h
#pragma once
#include <vector>

#include "mapper.h"

struct MapperMMC1 : Mapper {
    std::vector<uint8_t> prg, chr;
    std::vector<uint8_t> chrRAM;
    std::vector<uint8_t> prgRAM;

    bool chrIsRAM = false;
    uint8_t mir = 0;

    uint8_t ctrl = 0x0C;
    uint8_t chrBank0 = 0, chrBank1 = 0, prgBank = 0;

    uint8_t loadReg = 0;    // collects 5 bits (LSB-first)
    uint8_t loadCount = 0;  // 0..5

    // PRG-RAM control (bit4 of PRG bank reg on many SxROM: 0=enable, 1=disable writes)
    bool prgRamPresent = false;
    bool prgRamWriteEnabled = true;

    MapperMMC1(std::vector<uint8_t> prg_, std::vector<uint8_t> chr_, bool chrRam, uint8_t mir_, uint32_t prgRamKB);

    uint8_t cpuRead(uint16_t a) override;
    void cpuWrite(uint16_t a, uint8_t v) override;
    uint8_t ppuRead(uint16_t a) override;
    void ppuWrite(uint16_t a, uint8_t v) override;
    uint8_t mirroring() const override;

    // Save surface
    uint8_t* prgRamData() override { return prgRAM.empty() ? nullptr : prgRAM.data(); }
    size_t prgRamSize() const override { return prgRAM.size(); }
};
