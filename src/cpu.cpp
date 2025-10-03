// cpu.cpp
#include "cpu.h"

#include <cstdint>
#include <cstdio>
#include <utility>

#include "bus.h"

// 6502 JMP (ind) page-wrap bug helper
static inline uint16_t read16_bug(const CPU* c, uint16_t addr) {
    uint8_t lo = c->bus->cpuRead(addr);
    uint8_t hi = c->bus->cpuRead((uint16_t)((addr & 0xFF00) | ((addr + 1) & 0x00FF)));
    return (uint16_t)(lo | (hi << 8));
}

// Memory helpers
uint8_t CPU::rd(uint16_t a) const { return bus->cpuRead(a); }
void CPU::wr(uint16_t a, uint8_t v) { bus->cpuWrite(a, v); }

void CPU::push8(uint8_t v) {
    wr((uint16_t)(0x0100 | S), v);
    S--;
}
uint8_t CPU::pull8() {
    S++;
    return rd((uint16_t)(0x0100 | S));
}

// Reset/IRQs
void CPU::reset() {
    uint16_t lo = rd(0xFFFC), hi = rd(0xFFFD);
    PC = (uint16_t)(lo | (hi << 8));
    A = 0;
    X = 0;
    Y = 0;
    S = 0xFD;
    P = 0x24;  // I & Z set like power-on
    cycles = 0;
    pending_irq = pending_nmi = false;
    dma_stall_cycles = 0;
}
void CPU::powerOn() { reset(); }
void CPU::nmi() { pending_nmi = true; }
void CPU::irq() { pending_irq = true; }

int CPU::exec(uint8_t op) {
    auto fetch8 = [&]() { return rd(PC++); };
    auto fetch16 = [&]() { uint8_t lo=fetch8(); uint8_t hi=fetch8(); return (uint16_t)(lo | (hi<<8)); };

    auto setZN8 = [&](uint8_t v) { setf(Z, v==0); setf(N, v&0x80); };

    auto adc = [&](uint8_t v) {
        // NES 2A03 ignores decimal mode; use binary
        uint16_t sum = (uint16_t)A + v + (getf(C) ? 1 : 0);
        setf(C, sum > 0xFF);
        uint8_t res = (uint8_t)sum;
        setf(V, (~(A ^ v) & (A ^ res) & 0x80) != 0);
        A = res;
        setZN8(A);
    };
    auto sbc = [&](uint8_t v) {
        // A = A - v - (1-C)
        uint16_t diff = (uint16_t)A - v - (getf(C) ? 0 : 1);
        setf(C, diff < 0x100);
        uint8_t res = (uint8_t)diff;
        setf(V, ((A ^ v) & (A ^ res) & 0x80) != 0);
        A = res;
        setZN8(A);
    };

    auto asl_mem = [&](uint16_t a) {
        uint8_t v = rd(a);
        setf(C, v & 0x80);
        v <<= 1;
        wr(a, v);
        setZN8(v);
    };
    auto lsr_mem = [&](uint16_t a) {
        uint8_t v = rd(a);
        setf(C, v & 1);
        v >>= 1;
        wr(a, v);
        setZN8(v);
    };
    auto rol_mem = [&](uint16_t a) {
        uint8_t v = rd(a);
        uint8_t c = getf(C) ? 1 : 0;
        setf(C, v & 0x80);
        v = (uint8_t)((v << 1) | c);
        wr(a, v);
        setZN8(v);
    };
    auto ror_mem = [&](uint16_t a) {
        uint8_t v = rd(a);
        uint8_t c = getf(C) ? 1 : 0;
        setf(C, v & 1);
        v = (uint8_t)((v >> 1) | (c << 7));
        wr(a, v);
        setZN8(v);
    };

    auto branch = [&](bool cond) {
        int add = 2;
        int8_t off = (int8_t)fetch8();
        if (cond) {
            add++;
            uint16_t old = PC;
            PC = (uint16_t)(PC + off);
            if ((old & 0xFF00) != (PC & 0xFF00)) add++;  // page cross
        }
        return add;
    };

    // Addressing computations returning (addr, extraCycleIfPageCrossed)
    auto zp = [&]() { return (uint16_t)fetch8(); };
    auto zpx = [&]() { return (uint16_t)((fetch8() + X) & 0xFF); };
    auto zpy = [&]() { return (uint16_t)((fetch8() + Y) & 0xFF); };
    auto abs_ = [&]() { return fetch16(); };
    auto absx = [&]() {
        uint16_t b = fetch16();
        uint16_t a = (uint16_t)(b + X);
        return std::pair<uint16_t, bool>(a, ((b & 0xFF00) != (a & 0xFF00)));
    };
    auto absy = [&]() {
        uint16_t b = fetch16();
        uint16_t a = (uint16_t)(b + Y);
        return std::pair<uint16_t, bool>(a, ((b & 0xFF00) != (a & 0xFF00)));
    };
    auto indx = [&]() {
        uint8_t zpaddr = (uint8_t)(fetch8() + X);
        uint16_t a = (uint16_t)(rd(zpaddr) | (rd((uint8_t)(zpaddr + 1)) << 8));
        return a;
    };
    auto indy = [&]() {
        uint8_t zpaddr = fetch8();
        uint16_t base = (uint16_t)(rd(zpaddr) | (rd((uint8_t)(zpaddr + 1)) << 8));
        uint16_t a = (uint16_t)(base + Y);
        return std::pair<uint16_t, bool>(a, ((base & 0xFF00) != (a & 0xFF00)));
    };

    // Decode + execute
    switch (op) {
        // --------- Load/Store ---------
        case 0xA9: {
            A = fetch8();
            setZN8(A);
            return 2;
        }  // LDA #imm
        case 0xA5: {
            A = rd(zp());
            setZN8(A);
            return 3;
        }  // LDA zp
        case 0xB5: {
            A = rd(zpx());
            setZN8(A);
            return 4;
        }  // LDA zpx
        case 0xAD: {
            A = rd(abs_());
            setZN8(A);
            return 4;
        }  // LDA abs
        case 0xBD: {
            auto p = absx();
            A = rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }  // LDA abs,X
        case 0xB9: {
            auto p = absy();
            A = rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }  // LDA abs,Y
        case 0xA1: {
            A = rd(indx());
            setZN8(A);
            return 6;
        }  // LDA (ind,X)
        case 0xB1: {
            auto p = indy();
            A = rd(p.first);
            setZN8(A);
            return 5 + (p.second ? 1 : 0);
        }  // LDA (ind),Y

        case 0xA2: {
            X = fetch8();
            setZN8(X);
            return 2;
        }  // LDX #imm
        case 0xA6: {
            X = rd(zp());
            setZN8(X);
            return 3;
        }  // LDX zp
        case 0xB6: {
            X = rd(zpy());
            setZN8(X);
            return 4;
        }  // LDX zp,Y
        case 0xAE: {
            X = rd(abs_());
            setZN8(X);
            return 4;
        }  // LDX abs
        case 0xBE: {
            auto p = absy();
            X = rd(p.first);
            setZN8(X);
            return 4 + (p.second ? 1 : 0);
        }  // LDX abs,Y

        case 0xA0: {
            Y = fetch8();
            setZN8(Y);
            return 2;
        }  // LDY #imm
        case 0xA4: {
            Y = rd(zp());
            setZN8(Y);
            return 3;
        }  // LDY zp
        case 0xB4: {
            Y = rd(zpx());
            setZN8(Y);
            return 4;
        }  // LDY zp,X
        case 0xAC: {
            Y = rd(abs_());
            setZN8(Y);
            return 4;
        }  // LDY abs
        case 0xBC: {
            auto p = absx();
            Y = rd(p.first);
            setZN8(Y);
            return 4 + (p.second ? 1 : 0);
        }  // LDY abs,X

        case 0x85: {
            wr(zp(), A);
            return 3;
        }  // STA zp
        case 0x95: {
            wr(zpx(), A);
            return 4;
        }  // STA zp,X
        case 0x8D: {
            wr(abs_(), A);
            return 4;
        }  // STA abs
        case 0x9D: {
            auto p = absx();
            wr(p.first, A);
            return 5;
        }  // STA abs,X
        case 0x99: {
            auto p = absy();
            wr(p.first, A);
            return 5;
        }  // STA abs,Y
        case 0x81: {
            wr(indx(), A);
            return 6;
        }  // STA (ind,X)
        case 0x91: {
            auto p = indy();
            wr(p.first, A);
            return 6;
        }  // STA (ind),Y

        case 0x86: {
            wr(zp(), X);
            return 3;
        }  // STX zp
        case 0x96: {
            wr(zpy(), X);
            return 4;
        }  // STX zp,Y
        case 0x8E: {
            wr(abs_(), X);
            return 4;
        }  // STX abs
        case 0x84: {
            wr(zp(), Y);
            return 3;
        }  // STY zp
        case 0x94: {
            wr(zpx(), Y);
            return 4;
        }  // STY zp,X
        case 0x8C: {
            wr(abs_(), Y);
            return 4;
        }  // STY abs

        // --------- Transfers ---------
        case 0xAA: {
            X = A;
            setZN8(X);
            return 2;
        }  // TAX
        case 0xA8: {
            Y = A;
            setZN8(Y);
            return 2;
        }  // TAY
        case 0xBA: {
            X = S;
            setZN8(X);
            return 2;
        }  // TSX
        case 0x8A: {
            A = X;
            setZN8(A);
            return 2;
        }  // TXA
        case 0x9A: {
            S = X;
            return 2;
        }  // TXS
        case 0x98: {
            A = Y;
            setZN8(A);
            return 2;
        }  // TYA

        // --------- Arithmetic/Logic ---------
        case 0x69: {
            adc(fetch8());
            return 2;
        }  // ADC #imm
        case 0x65: {
            adc(rd(zp()));
            return 3;
        }
        case 0x75: {
            adc(rd(zpx()));
            return 4;
        }
        case 0x6D: {
            adc(rd(abs_()));
            return 4;
        }
        case 0x7D: {
            auto p = absx();
            adc(rd(p.first));
            return 4 + (p.second ? 1 : 0);
        }
        case 0x79: {
            auto p = absy();
            adc(rd(p.first));
            return 4 + (p.second ? 1 : 0);
        }
        case 0x61: {
            adc(rd(indx()));
            return 6;
        }
        case 0x71: {
            auto p = indy();
            adc(rd(p.first));
            return 5 + (p.second ? 1 : 0);
        }

        case 0xE9:
        case 0xEB: {
            sbc(fetch8());
            return 2;
        }  // SBC #imm (0xEB = unofficial but common)
        case 0xE5: {
            sbc(rd(zp()));
            return 3;
        }
        case 0xF5: {
            sbc(rd(zpx()));
            return 4;
        }
        case 0xED: {
            sbc(rd(abs_()));
            return 4;
        }
        case 0xFD: {
            auto p = absx();
            sbc(rd(p.first));
            return 4 + (p.second ? 1 : 0);
        }
        case 0xF9: {
            auto p = absy();
            sbc(rd(p.first));
            return 4 + (p.second ? 1 : 0);
        }
        case 0xE1: {
            sbc(rd(indx()));
            return 6;
        }
        case 0xF1: {
            auto p = indy();
            sbc(rd(p.first));
            return 5 + (p.second ? 1 : 0);
        }

        case 0x29: {
            A &= fetch8();
            setZN8(A);
            return 2;
        }  // AND #imm
        case 0x25: {
            A &= rd(zp());
            setZN8(A);
            return 3;
        }
        case 0x35: {
            A &= rd(zpx());
            setZN8(A);
            return 4;
        }
        case 0x2D: {
            A &= rd(abs_());
            setZN8(A);
            return 4;
        }
        case 0x3D: {
            auto p = absx();
            A &= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x39: {
            auto p = absy();
            A &= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x21: {
            A &= rd(indx());
            setZN8(A);
            return 6;
        }
        case 0x31: {
            auto p = indy();
            A &= rd(p.first);
            setZN8(A);
            return 5 + (p.second ? 1 : 0);
        }

        case 0x49: {
            A ^= fetch8();
            setZN8(A);
            return 2;
        }  // EOR
        case 0x45: {
            A ^= rd(zp());
            setZN8(A);
            return 3;
        }
        case 0x55: {
            A ^= rd(zpx());
            setZN8(A);
            return 4;
        }
        case 0x4D: {
            A ^= rd(abs_());
            setZN8(A);
            return 4;
        }
        case 0x5D: {
            auto p = absx();
            A ^= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x59: {
            auto p = absy();
            A ^= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x41: {
            A ^= rd(indx());
            setZN8(A);
            return 6;
        }
        case 0x51: {
            auto p = indy();
            A ^= rd(p.first);
            setZN8(A);
            return 5 + (p.second ? 1 : 0);
        }

        case 0x09: {
            A |= fetch8();
            setZN8(A);
            return 2;
        }  // ORA
        case 0x05: {
            A |= rd(zp());
            setZN8(A);
            return 3;
        }
        case 0x15: {
            A |= rd(zpx());
            setZN8(A);
            return 4;
        }
        case 0x0D: {
            A |= rd(abs_());
            setZN8(A);
            return 4;
        }
        case 0x1D: {
            auto p = absx();
            A |= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x19: {
            auto p = absy();
            A |= rd(p.first);
            setZN8(A);
            return 4 + (p.second ? 1 : 0);
        }
        case 0x01: {
            A |= rd(indx());
            setZN8(A);
            return 6;
        }
        case 0x11: {
            auto p = indy();
            A |= rd(p.first);
            setZN8(A);
            return 5 + (p.second ? 1 : 0);
        }

        case 0x24: {  // BIT zp
            uint8_t v = rd(zp());
            setf(Z, (A & v) == 0);
            setf(V, v & 0x40);
            setf(N, v & 0x80);
            return 3;
        }
        case 0x2C: {  // BIT abs
            uint8_t v = rd(abs_());
            setf(Z, (A & v) == 0);
            setf(V, v & 0x40);
            setf(N, v & 0x80);
            return 4;
        }

        // --------- Shifts & Rotates ---------
        case 0x0A: {
            setf(C, A & 0x80);
            A <<= 1;
            setZN8(A);
            return 2;
        }  // ASL A
        case 0x06: {
            asl_mem(zp());
            return 5;
        }
        case 0x16: {
            asl_mem(zpx());
            return 6;
        }
        case 0x0E: {
            asl_mem(abs_());
            return 6;
        }
        case 0x1E: {
            auto p = absx();
            asl_mem(p.first);
            return 7;
        }

        case 0x4A: {
            setf(C, A & 1);
            A >>= 1;
            setZN8(A);
            return 2;
        }  // LSR A
        case 0x46: {
            lsr_mem(zp());
            return 5;
        }
        case 0x56: {
            lsr_mem(zpx());
            return 6;
        }
        case 0x4E: {
            lsr_mem(abs_());
            return 6;
        }
        case 0x5E: {
            auto p = absx();
            lsr_mem(p.first);
            return 7;
        }

        case 0x2A: {
            uint8_t c = getf(C) ? 1 : 0;
            setf(C, A & 0x80);
            A = (uint8_t)((A << 1) | c);
            setZN8(A);
            return 2;
        }  // ROL A
        case 0x26: {
            rol_mem(zp());
            return 5;
        }
        case 0x36: {
            rol_mem(zpx());
            return 6;
        }
        case 0x2E: {
            rol_mem(abs_());
            return 6;
        }
        case 0x3E: {
            auto p = absx();
            rol_mem(p.first);
            return 7;
        }

        case 0x6A: {
            uint8_t c = getf(C) ? 1 : 0;
            setf(C, A & 1);
            A = (uint8_t)((A >> 1) | (c << 7));
            setZN8(A);
            return 2;
        }  // ROR A
        case 0x66: {
            ror_mem(zp());
            return 5;
        }
        case 0x76: {
            ror_mem(zpx());
            return 6;
        }
        case 0x6E: {
            ror_mem(abs_());
            return 6;
        }
        case 0x7E: {
            auto p = absx();
            ror_mem(p.first);
            return 7;
        }

        // --------- INC/DEC ---------
        case 0xE6: {
            uint16_t a = zp();
            uint8_t v = (uint8_t)(rd(a) + 1);
            wr(a, v);
            setZN8(v);
            return 5;
        }
        case 0xF6: {
            uint16_t a = zpx();
            uint8_t v = (uint8_t)(rd(a) + 1);
            wr(a, v);
            setZN8(v);
            return 6;
        }
        case 0xEE: {
            uint16_t a = abs_();
            uint8_t v = (uint8_t)(rd(a) + 1);
            wr(a, v);
            setZN8(v);
            return 6;
        }
        case 0xFE: {
            auto p = absx();
            uint8_t v = (uint8_t)(rd(p.first) + 1);
            wr(p.first, v);
            setZN8(v);
            return 7;
        }

        case 0xC6: {
            uint16_t a = zp();
            uint8_t v = (uint8_t)(rd(a) - 1);
            wr(a, v);
            setZN8(v);
            return 5;
        }
        case 0xD6: {
            uint16_t a = zpx();
            uint8_t v = (uint8_t)(rd(a) - 1);
            wr(a, v);
            setZN8(v);
            return 6;
        }
        case 0xCE: {
            uint16_t a = abs_();
            uint8_t v = (uint8_t)(rd(a) - 1);
            wr(a, v);
            setZN8(v);
            return 6;
        }
        case 0xDE: {
            auto p = absx();
            uint8_t v = (uint8_t)(rd(p.first) - 1);
            wr(p.first, v);
            setZN8(v);
            return 7;
        }

        case 0xE8: {
            X++;
            setZN8(X);
            return 2;
        }
        case 0xC8: {
            Y++;
            setZN8(Y);
            return 2;
        }
        case 0xCA: {
            X--;
            setZN8(X);
            return 2;
        }
        case 0x88: {
            Y--;
            setZN8(Y);
            return 2;
        }

        // --------- Compare ---------
        case 0xC9: {
            uint8_t v = fetch8();
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 2;
        }  // CMP
        case 0xC5: {
            uint8_t v = rd(zp());
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 3;
        }
        case 0xD5: {
            uint8_t v = rd(zpx());
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 4;
        }
        case 0xCD: {
            uint8_t v = rd(abs_());
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 4;
        }
        case 0xDD: {
            auto p = absx();
            uint8_t v = rd(p.first);
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 4 + (p.second ? 1 : 0);
        }
        case 0xD9: {
            auto p = absy();
            uint8_t v = rd(p.first);
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 4 + (p.second ? 1 : 0);
        }
        case 0xC1: {
            uint8_t v = rd(indx());
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 6;
        }
        case 0xD1: {
            auto p = indy();
            uint8_t v = rd(p.first);
            setf(C, A >= v);
            setZN8((uint8_t)(A - v));
            return 5 + (p.second ? 1 : 0);
        }

        case 0xE0: {
            uint8_t v = fetch8();
            setf(C, X >= v);
            setZN8((uint8_t)(X - v));
            return 2;
        }  // CPX
        case 0xE4: {
            uint8_t v = rd(zp());
            setf(C, X >= v);
            setZN8((uint8_t)(X - v));
            return 3;
        }
        case 0xEC: {
            uint8_t v = rd(abs_());
            setf(C, X >= v);
            setZN8((uint8_t)(X - v));
            return 4;
        }

        case 0xC0: {
            uint8_t v = fetch8();
            setf(C, Y >= v);
            setZN8((uint8_t)(Y - v));
            return 2;
        }  // CPY
        case 0xC4: {
            uint8_t v = rd(zp());
            setf(C, Y >= v);
            setZN8((uint8_t)(Y - v));
            return 3;
        }
        case 0xCC: {
            uint8_t v = rd(abs_());
            setf(C, Y >= v);
            setZN8((uint8_t)(Y - v));
            return 4;
        }

        // --------- Branches ---------
        case 0x90:
            return branch(!getf(C));  // BCC
        case 0xB0:
            return branch(getf(C));  // BCS
        case 0xF0:
            return branch(getf(Z));  // BEQ
        case 0x30:
            return branch(getf(N));  // BMI
        case 0xD0:
            return branch(!getf(Z));  // BNE
        case 0x10:
            return branch(!getf(N));  // BPL
        case 0x50:
            return branch(!getf(V));  // BVC
        case 0x70:
            return branch(getf(V));  // BVS

        // --------- Jumps & Subroutines ---------
        case 0x4C: {
            PC = abs_();
            return 3;
        }  // JMP abs
        case 0x6C: {
            uint16_t ptr = abs_();
            PC = read16_bug(this, ptr);
            return 5;
        }  // JMP (ind)
        case 0x20: {
            uint16_t addr = abs_();
            uint16_t ret = (uint16_t)(PC - 1);
            push8((uint8_t)((ret >> 8) & 0xFF));
            push8((uint8_t)(ret & 0xFF));
            PC = addr;
            return 6;
        }

        case 0x60: {
            uint8_t lo = pull8(), hi = pull8();
            PC = (uint16_t)((hi << 8) | lo);
            PC++;
            return 6;
        }

            // --------- Stack & Flags ---------
        case 0x00: {
            PC++;  // BRK increments PC before pushing
            push8((uint8_t)((PC >> 8) & 0xFF));
            push8((uint8_t)(PC & 0xFF));
            uint8_t flags = (uint8_t)(P | 0x10);
            push8(flags);
            setf(I, true);
            uint16_t lo = rd(0xFFFE), hi = rd(0xFFFF);
            PC = (uint16_t)(lo | (hi << 8));
            return 7;
        }
        case 0x40: {  // RTI
            uint8_t pf = pull8();
            uint8_t lo = pull8();
            uint8_t hi = pull8();
            P = (uint8_t)(pf & ~0x10);
            setf(U, true);
            PC = (uint16_t)(lo | (hi << 8));
            irq_delay = 1;
            return 6;
        }
        case 0x48: {
            push8(A);
            return 3;
        }
        case 0x68: {
            A = pull8();
            setZN8(A);
            return 4;
        }
        case 0x08: {
            push8((uint8_t)(P | 0x10));
            return 3;
        }
        case 0x28: {  // PLP
            P = (uint8_t)((pull8() & ~0x10) | 0x20);
            // I may have changed -> delay recognition by one instruction
            irq_delay = 1;
            setZN8(A);
            return 4;
        }
        case 0x18: {
            setf(C, false);
            return 2;
        }  // CLC
        case 0x38: {
            setf(C, true);
            return 2;
        }  // SEC
        case 0x58: {  // CLI
            setf(I, false);
            irq_delay = 1;
            return 2;
        }
        case 0x78: {  // SEI
            setf(I, true);
            irq_delay = 1;
            return 2;
        }
        case 0xB8: {
            setf(V, false);
            return 2;
        }  // CLV
        case 0xD8: {
            setf(D, false);
            return 2;
        }  // CLD (decimal ignored anyway)
        case 0xF8: {
            setf(D, true);
            return 2;
        }  // SED

        // --------- NOPs ---------
        case 0xEA:
            return 2;  // NOP
        // many unofficial NOPs behave as 2/3/4-cycle NOPs; treat generically:
        case 0x1A:
        case 0x3A:
        case 0x5A:
        case 0x7A:
        case 0xDA:
        case 0xFA:
            return 2;
        case 0x04:
        case 0x44:
        case 0x64: {
            fetch8();
            return 3;
        }  // NOP zp
        case 0x0C: {
            fetch16();
            return 4;
        }  // NOP abs
        case 0x14:
        case 0x34:
        case 0x54:
        case 0x74:
        case 0xD4:
        case 0xF4: {
            fetch8();
            return 4;
        }  // NOP zpx
        case 0x1C:
        case 0x3C:
        case 0x5C:
        case 0x7C:
        case 0xDC:
        case 0xFC: {
            auto p = absx();
            (void)p;
            return 4;
        }

        // --------- Fallback (treat as 2-cycle NOP) ---------
        default:
            // printf("Unhandled opcode %02X at %04X\n", op, PC-1);
            return 2;
    }
}

int CPU::step() {
    // DMA stall (OAM DMA)
    if (dma_stall_cycles) {
        dma_stall_cycles--;
        cycles++;
        return 1;
    }

    bool suppress_irq = (irq_delay != 0);
    if (irq_delay) irq_delay = 0;  // only the next boundary is suppressed

    // Re-sample level IRQs each boundary, but only latch if not suppressed
    if (!suppress_irq) {
        if (bus->mapperIRQ() && !getf(I)) pending_irq = true;
        if (bus->apuIRQ() && !getf(I)) pending_irq = true;
    }



    // NMI edge (non-maskable)
    if (pending_nmi) {
        pending_nmi = false;
        uint16_t ret = PC;
        // push PC hi, lo, then P (B=0 on stack)
        push8((uint8_t)((ret >> 8) & 0xFF));
        push8((uint8_t)(ret & 0xFF));
        push8(P & ~0x10);

        setf(I, true);
        uint16_t lo = rd(0xFFFA), hi = rd(0xFFFB);
        PC = (uint16_t)(lo | (hi << 8));
        cycles += 7;
        return 7;
    }

    // IRQ (maskable)
    if (pending_irq && !getf(I)) {
        pending_irq = false;

        uint16_t ret = PC;
        push8((uint8_t)((ret >> 8) & 0xFF));
        push8((uint8_t)(ret & 0xFF));
        push8(P & ~0x10);
        setf(I, true);

        uint16_t lo = rd(0xFFFE), hi = rd(0xFFFF);
        PC = (uint16_t)(lo | (hi << 8));

        // ********** ACK the mapper IRQ here **********
        if (bus) bus->mapperIRQAck();

        cycles += 7;
        return 7;
    }

    // Normal fetch/execute
    uint8_t opcode = rd(PC++);
    int c = exec(opcode);
    cycles += c;
    return c;
}
