// mapper_nrom.h
#pragma once
#include <vector>
#include "mapper.h"

struct MapperNROM : Mapper {
    std::vector<uint8_t> prg, chr;
    bool hasChrRam=false;
    uint8_t mir=0;

    MapperNROM(std::vector<uint8_t> prg_, std::vector<uint8_t> chr_, bool chrRam, uint8_t mir_)
      : prg(std::move(prg_)), chr(std::move(chr_)), hasChrRam(chrRam), mir(mir_) {}

    uint8_t cpuRead(uint16_t a) override;
    void    cpuWrite(uint16_t a, uint8_t v) override;
    uint8_t ppuRead(uint16_t a) override;
    void    ppuWrite(uint16_t a, uint8_t v) override;
    uint8_t mirroring() const override { return mir; }

    // NROM typically has no PRG-RAM; leave prgRamData/Size as defaults.
};
