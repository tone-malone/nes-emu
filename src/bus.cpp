// bus.cpp
#include "bus.h"
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "cartridge.h"
#include "input.h"
#include "mapper.h"

// 2 KiB internal RAM, mirrored every 0x800 up to 0x1FFF
static uint8_t sysRAM[2048] = {};

uint8_t Bus::cpuRead(uint16_t a){
    if(a < 0x2000){
        return sysRAM[a & 0x07FF];
    }else if(a < 0x4000){
        // PPU registers mirrored every 8
        uint16_t r = 0x2000 + (a & 7);
        return ppu->cpuReadRegister(r);
    }else if(a == 0x4015){
        // APU status
        return apu->cpuRead(0x4015);
    }else if(a == 0x4016){
        // Controller 1
        return input ? input->read4016() : 0x40;
    }else if(a == 0x4017){
        // Controller 2 not implemented
        return 0x40;
    }else if(a >= 0x4000 && a <= 0x4017){
        // Reads of write-only APU regs typically return open-bus-ish; keep simple:
        return 0;
    }else if(a >= 0x4020){
        // Cartridge / mapper space (includes PRG-RAM, PRG-ROM)
        return cart ? cart->cpuRead(a) : 0xFF;
    }else{
        // $4018-$401F APU/IO test registers (unused)
        return 0;
    }
}

void Bus::cpuWrite(uint16_t a, uint8_t v){
    if(a < 0x2000){
        sysRAM[a & 0x07FF] = v;
    }else if(a < 0x4000){
        uint16_t r = 0x2000 + (a & 7);
        ppu->cpuWriteRegister(r, v);
    }else if(a == 0x4014){
        // OAM DMA: copy 256 bytes from page $xx00..$xxFF to PPU OAM
        uint16_t base = (uint16_t)v << 8;
        bool cpu_on_odd = (cpu->cycles & 1ull);
        // Stall CPU for 513 or 514 cycles
        cpu->dma_stall_cycles += 513 + (cpu_on_odd ? 1 : 0);

        // Perform the copy functionally now
        ppu->oamDMA([&](uint8_t i){ return cpuRead(base + i); });
    }else if(a == 0x4016){
        // Controller strobe
        if(input) input->setStrobe(v);
    }else if(a >= 0x4000 && a <= 0x4017){
        // APU + frame counter
        apu->cpuWrite(a, v);
    }else if(a >= 0x4020){
        if(cart) cart->cpuWrite(a, v);
    }else{
        // ignore
    }
}

bool Bus::mapperIRQ(){
    return cart && cart->mapper && cart->mapper->irqPending();
}
void Bus::mapperIRQAck(){
    if(cart && cart->mapper) cart->mapper->irqAck();
}
bool Bus::apuIRQ(){
    return apu && apu->irqLine();
}

inline uint8_t Bus::ppuRegRead(uint16_t a){
    return ppu->cpuReadRegister(a);
}
inline void Bus::ppuRegWrite(uint16_t a, uint8_t v){
    ppu->cpuWriteRegister(a, v);
}
