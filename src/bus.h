// bus.h
#pragma once
#include <cstdint>
#include <memory>

struct CPU;
struct PPU;
struct APU;
struct Cartridge;
struct Input;

struct Bus {
    CPU*       cpu = nullptr;
    PPU*       ppu = nullptr;
    APU*       apu = nullptr;
    Cartridge* cart = nullptr;
    Input*     input = nullptr;

    // CPU-visible memory map
    uint8_t cpuRead (uint16_t a);
    void    cpuWrite(uint16_t a, uint8_t v);

    // IRQ lines from subsystems
    bool mapperIRQ();
    void mapperIRQAck();
    bool apuIRQ();

    // Convenience passthroughs (optional)
    inline uint8_t ppuRegRead(uint16_t a);
    inline void    ppuRegWrite(uint16_t a, uint8_t v);
};
