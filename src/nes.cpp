// nes.cpp
#include "nes.h"

#include <SDL2/SDL.h>

#include "apu.h"
#include "bus.h"
#include "cartridge.h"
#include "cpu.h"
#include "input.h"
#include "ppu.h"

bool NES::loadROM(const std::string& path) {
    cart = Cartridge::loadFromFile(path);
    return (bool)cart;
}

void NES::powerOn() {
    bus = std::make_unique<Bus>();
    cpu = std::make_unique<CPU>();
    ppu = std::make_unique<PPU>();
    apu = std::make_unique<APU>();
    input = std::make_unique<Input>();

    bus->cpu = cpu.get();
    bus->ppu = ppu.get();
    bus->apu = apu.get();
    bus->cart = cart.get();
    bus->input = input.get();

    cpu->bus = bus.get();
    ppu->connect(cart.get());
    // Initialize OAM to 0xFF per power-on expectations
    std::memset(ppu->oam, 0xFF, sizeof(ppu->oam));

    apu->init();
    apu->bus = bus.get();

    cpu->reset();
}

void NES::runFrame() {
    input->poll();
    bool frameDone = false;
    while (!frameDone) {
        // CPU executes one instruction (or 1 DMA-stall cycle)
        int cpuCycles = cpu->step();

        // APU ticks once per CPU cycle
        for (int i = 0; i < cpuCycles; i++) {
            apu->tickCPU();
        }

        // PPU runs 3x per CPU cycle
        for (int i = 0; i < cpuCycles * 3; i++) {
            ppu->tick();

            // PPU can request NMI at start of vblank
            bool nmiLevel = (ppu->nmi_occurred && ppu->nmi_output());
            if (nmiLevel && !nmiLinePrev) {
                cpu->nmi();  // post NMI edge
            }
            nmiLinePrev = nmiLevel;

            // We define a frame boundary when we wrap back to (0,0)
            if (ppu->scanline == 0 && ppu->dot == 0) {
                frameDone = true;
            }
        }
    }
}

NES::~NES() {
    if (apu) apu->shutdown();
    if (cart) cart->saveSave();
}
