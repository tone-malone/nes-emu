// cartridge.h
#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <string>

struct Mapper;

struct Cartridge {
    std::shared_ptr<Mapper> mapper;

    uint8_t mapperId=0;
    uint8_t mirroring=0;     // cached (0=horiz,1=vert)
    bool    batteryBacked=false;
    std::string romPath;

    static std::shared_ptr<Cartridge> loadFromFile(const std::string& path);

    // CPU/PPU bus
    uint8_t cpuRead(uint16_t a);
    void    cpuWrite(uint16_t a, uint8_t v);
    uint8_t ppuRead(uint16_t a);
    void    ppuWrite(uint16_t a, uint8_t v);

    // Save RAM
    void loadSave();
    void saveSave();
};
