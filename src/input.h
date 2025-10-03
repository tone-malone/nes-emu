// input.h
#pragma once
#include <cstdint>
#include <SDL2/SDL.h>

struct Input {
    // NES pad latch/shift registers (controller 1 only here)
    uint8_t shift1 = 0;
    uint8_t strobe = 0;

    // Aggregated state for this frame (A,B,Select,Start,Up,Down,Left,Right) active-high in bits 0..7
    uint8_t padState = 0;

    // Optional SDL game controller
    SDL_GameController* controller = nullptr;

    void setStrobe(uint8_t v){ strobe = v & 1; if(strobe) shift1 = padState; }
    uint8_t read4016(){ // typical serial read
        uint8_t bit = (shift1 & 1);
        if(!strobe) shift1 = (uint8_t)((shift1 >> 1) | 0x80); // ones when shifted out
        return bit | 0x40; // upper bits open bus-ish; ensure bit6 set per many emus
    }

    // Called once per frame to gather inputs (keyboard + controller)
    void poll();
};
