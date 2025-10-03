// apu.h
#pragma once
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdint>

#include "apu_clock.h"

struct Bus;  // for DMC memory fetch

struct APU {
    // SDL audio device
    SDL_AudioDeviceID dev = 0;
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int BUFFER_SAMPLES = 4096;

    // CPU coupling (for DMC memory reads)
    Bus* bus = nullptr;

    // ----- Frame sequencer (exact cadence) -----
    bool mode5 = false;       // 5-step if true
    bool irqInhibit = false;  // inhibit frame IRQs if true
    bool frameIRQ = false;    // raised at end of 4-step sequence (if not inhibited)
    double fcCycle = 0.0;     // cycles since sequence start
    int fcStep = 0;           // current step index
    void resetFrameSequencer(bool fiveStep, bool inhibitIRQ, bool immediateClock);
    void frameSequencerStep();  // maintained internally via tickCPU exact timing

    // ----- Length table -----
    static const uint8_t lengthTable[32];

    // ----- Pulse channels -----
    struct Pulse {
        bool enabled = false;
        uint8_t dutySel = 0;
        uint8_t envelope = 0;  // $4000/$4004
        uint8_t sweep = 0;     // $4001/$4005
        uint8_t timerLo = 0;   // $4002/$4006
        uint8_t length = 0;    // $4003/$4007 (index)
        uint8_t timerHi = 0;

        // Internals
        uint16_t timer = 0, period = 0;
        uint8_t seqIndex = 0;
        // Envelope
        bool envStart = false;
        uint8_t envVol = 0, envDivider = 0;
        bool constantVol = false;
        uint8_t vol = 0;
        // Length
        uint8_t lengthCtr = 0;
        // Sweep
        bool sweepNeg = false;
        uint8_t sweepShift = 0;
        uint8_t sweepDivider = 0;
        bool sweepEnable = false;
        bool sweepReload = false;

        void write0(uint8_t v);
        void write1(uint8_t v);
        void write2(uint8_t v);
        void write3(uint8_t v);

        void quarterFrame();  // envelope
        void halfFrame(bool isPulse1);
        void clockTimer();

        int targetPeriod(bool isPulse1) const;
        float sample(bool isPulse1) const;
    } pulse1, pulse2;

    // ----- Triangle -----
    struct Triangle {
        bool enabled = false;
        uint8_t linReg = 0;  // $4008
        uint8_t timerLo = 0;
        uint8_t timerHi = 0;

        uint16_t timer = 0, period = 0;
        uint8_t step = 0;
        uint8_t lengthCtr = 0;
        uint8_t linCtr = 0;
        bool linReload = false;
        bool control = false;  // halt length if set

        void write0(uint8_t v);
        void write2(uint8_t v);
        void write3(uint8_t v);

        void quarterFrame();  // linear counter
        void halfFrame();     // length counter
        void clockTimer();
        float sample() const;
    } tri;

    // ----- Noise -----
    struct Noise {
        bool enabled = false;
        uint8_t envReg = 0;   // $400C
        uint8_t modeReg = 0;  // $400E
        uint8_t lenReg = 0;   // $400F

        uint16_t lfsr = 1;
        uint16_t period = 0;
        uint16_t timer = 0;

        // Envelope
        bool envStart = false;
        uint8_t envVol = 0, envDivider = 0;
        bool constantVol = false;
        uint8_t vol = 0;
        uint8_t lengthCtr = 0;

        void writeC(uint8_t v);
        void writeE(uint8_t v);
        void writeF(uint8_t v);

        void quarterFrame();
        void halfFrame();
        void clockTimer();
        float sample() const;
    } noise;

    // ----- DMC -----
    struct DMC {
        bool enabled = false;
        uint8_t reg0 = 0, reg1 = 0, reg2 = 0, reg3 = 0;  // $4010..$4013
        uint16_t rate = 0;
        uint16_t timer = 0;
        uint16_t addr = 0;
        uint16_t length = 0;
        uint16_t curAddr = 0;
        uint16_t bytesLeft = 0;
        uint8_t shift = 0;
        uint8_t bits = 8;
        bool silence = true;
        uint8_t output = 0x40;  // 7-bit DAC midpoint

        void write0(uint8_t v);
        void write1(uint8_t v);
        void write2(uint8_t v);
        void write3(uint8_t v);
        void restartSample();
        void clockTimer(APU* apu);  // pulls bytes from CPU Bus

        float sample() const { return (output / 127.0f) - 0.5f; }
    } dmc;

    // DMC / Frame-IRQ flags exposed to CPU
    bool dmcIRQ = false;
    inline bool irqLine() const { return frameIRQ || dmcIRQ; }

    // Mixer
    float mix() const;

    // Audio resampling/output
    ClockFrac resampFrac;
    int sampleRate = SAMPLE_RATE;
    double samplesPerCpu = (double)SAMPLE_RATE / 1789773.0;  // NTSC CPU
    int16_t outBuf[BUFFER_SAMPLES]{};
    int outPos = 0;

    // API
    void init();
    void shutdown();
    void tickCPU();       // call once per CPU cycle
    void quarterFrame();  // triggered by sequencer
    void halfFrame();     // triggered by sequencer

    // CPU I/O
    uint8_t cpuRead(uint16_t a);           // $4015
    void cpuWrite(uint16_t a, uint8_t v);  // $4000..$4017
};
