// mapper.h
#pragma once
#include <cstdint>
#include <cstddef>

struct Mapper {
    virtual ~Mapper() = default;

    // CPU/PPU mapping
    virtual uint8_t cpuRead(uint16_t a) = 0;
    virtual void    cpuWrite(uint16_t a, uint8_t v) = 0;
    virtual uint8_t ppuRead(uint16_t a) = 0;
    virtual void    ppuWrite(uint16_t a, uint8_t v) = 0;

    // PPU nametable mirroring (0=horiz, 1=vert; extend if you add 4-screen/single-screen)
    virtual uint8_t mirroring() const = 0;

    // IRQ + PPU A12 clocking (MMC3)
    virtual bool    irqPending() const { return false; }
    virtual void    irqAck() {}
    virtual void ppuA12Clock(bool /*level*/) {}
    virtual void ppuOnScanlineDot260(bool /*rendering*/) {}  // default no-op

    // PRG-RAM surface for battery saves
    virtual uint8_t* prgRamData() { return nullptr; }
    virtual size_t   prgRamSize() const { return 0; }
};
