// nes.h
#pragma once
#include <memory>
#include <string>

#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "bus.h"
#include "input.h"
#include "cartridge.h"

struct NES {
    std::unique_ptr<CPU>  cpu;
    std::unique_ptr<PPU>  ppu;
    std::unique_ptr<APU>  apu;
    std::unique_ptr<Bus>  bus;
    std::unique_ptr<Input> input;
    std::shared_ptr<Cartridge> cart;
    bool nmiLinePrev = false; 
    bool loadROM(const std::string& path);
    void powerOn();
    void runFrame();
    ~NES();
};
