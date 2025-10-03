// cpu.h
#pragma once
#include <cstdint>

struct Bus;

struct CPU {
    Bus* bus = nullptr;

    // Registers
    uint8_t A = 0, X = 0, Y = 0;  // Accumulator, Index
    uint8_t S = 0xFD;             // Stack pointer
    uint16_t PC = 0xC000;         // Program counter
    uint8_t P = 0x24;             // Status (NV-BDIZC)

    // Cycle counters
    uint64_t cycles = 0;

    // Interrupt lines (edge/level latched)
    bool pending_nmi = false;
    bool pending_irq = false;

    int irq_delay = 0;   // suppress maskable IRQ for one instruction boundary

    // DMA stall (OAM DMA via $4014)
    int dma_stall_cycles = 0;

    // Flags
    enum Flag { C = 0,
                Z = 1,
                I = 2,
                D = 3,
                B = 4,
                U = 5,
                V = 6,
                N = 7 };
    inline bool getf(Flag f) const { return (P >> f) & 1; }
    inline void setf(Flag f, bool v) {
        if (v)
            P |= (1u << f);
        else
            P &= ~(1u << f);
    }

    // Lifecycle
    void reset();
    void powerOn();  // optional distinct from reset if you need it
    int step();      // execute one instruction (or 1 stalled cycle), returns CPU cycles taken

    // Interrupt requests (edge)
    void nmi();
    void irq();

   private:
    // Helpers (implemented in cpu.cpp)
    uint8_t rd(uint16_t a) const;
    void wr(uint16_t a, uint8_t v);
    int exec(uint8_t opcode);
    void push8(uint8_t v);
    uint8_t pull8();

    // Common flag setters
    inline void setZN(uint8_t v) {
        setf(Z, v == 0);
        setf(N, v & 0x80);
    }
};
