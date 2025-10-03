// apu.cpp
#include "apu.h"

#include <cmath>
#include <cstring>

#include "bus.h"

// Length counter table
const uint8_t APU::lengthTable[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

// ===== Pulse =====
void APU::Pulse::write0(uint8_t v) {
    dutySel = (v >> 6) & 3;
    constantVol = (v & 0x10);
    vol = v & 0x0F;
    envelope = v;
    envStart = true;
}
void APU::Pulse::write1(uint8_t v) {
    sweep = v;
    sweepEnable = (v & 0x80) != 0;
    sweepDivider = (v >> 4) & 7;
    sweepNeg = (v & 0x08);
    sweepShift = v & 7;
    sweepReload = true;
}
void APU::Pulse::write2(uint8_t v) {
    timerLo = v;
    period = (period & 0xFF00) | v;
}
void APU::Pulse::write3(uint8_t v) {
    timerHi = v & 7;
    period = ((uint16_t)(v & 7) << 8) | timerLo;
    lengthCtr = APU::lengthTable[(v >> 3) & 31];
    envStart = true;
    seqIndex = 0;
}
void APU::Pulse::quarterFrame() {
    if (envStart) {
        envStart = false;
        envVol = 15;
        envDivider = (envelope & 0x0F);
    } else {
        if (envDivider == 0) {
            envDivider = (envelope & 0x0F);
            if (envVol > 0)
                envVol--;
            else if (envelope & 0x20)
                envVol = 15;
        } else
            envDivider--;
    }
}
int APU::Pulse::targetPeriod(bool isPulse1) const {
    if (sweepShift == 0) return period;
    int change = period >> sweepShift;
    if (sweepNeg) change = -change - (isPulse1 ? 1 : 0);
    return period + change;
}
void APU::Pulse::halfFrame(bool isPulse1) {
    if (!((envelope & 0x20)) && lengthCtr > 0) lengthCtr--;
    if (sweepEnable && sweepShift) {
        if (sweepDivider == 0) {
            int tp = targetPeriod(isPulse1);
            if (tp >= 8 && tp <= 0x7FF) period = (uint16_t)tp;
            sweepDivider = (sweep >> 4) & 7;
        } else {
            sweepDivider--;
        }
        if (sweepReload) {
            sweepDivider = (sweep >> 4) & 7;
            sweepReload = false;
        }
    }
}
void APU::Pulse::clockTimer() {
    if (timer == 0) {
        timer = period;
        seqIndex = (seqIndex + 1) & 7;
    } else
        timer--;
}
float APU::Pulse::sample(bool) const {
    if (!enabled || lengthCtr == 0 || period < 8 || period > 0x7FF) return 0.0f;
    static const uint8_t duty[4][8] = {
        {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
    uint8_t bit = duty[dutySel][seqIndex];
    uint8_t level = constantVol ? vol : envVol;
    return bit ? (level / 15.0f) : 0.0f;
}

// ===== Triangle =====
void APU::Triangle::write0(uint8_t v) {
    control = (v & 0x80) != 0;
    linReg = v & 0x7F;
}
void APU::Triangle::write2(uint8_t v) { period = (period & 0xFF00) | v; }
void APU::Triangle::write3(uint8_t v) {
    period = ((uint16_t)(v & 7) << 8) | (period & 0x00FF);
    lengthCtr = APU::lengthTable[(v >> 3) & 31];
    linReload = true;
}
void APU::Triangle::quarterFrame() {
    if (linReload)
        linCtr = linReg;
    else if (linCtr > 0)
        linCtr--;
    if (!control) linReload = false;
}
void APU::Triangle::halfFrame() {
    if (!control && lengthCtr > 0) lengthCtr--;
}
void APU::Triangle::clockTimer() {
    if (lengthCtr == 0 || linCtr == 0) return;
    if (timer == 0) {
        timer = period;
        step = (step + 1) & 31;
    } else
        timer--;
}
float APU::Triangle::sample() const {
    static const uint8_t wav[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    if (lengthCtr == 0 || linCtr == 0 || period < 2) return 0.0f;
    return (wav[step] / 15.0f - 0.5f) * 0.8f;
}

// ===== Noise =====
static const uint16_t noisePeriod[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
void APU::Noise::writeC(uint8_t v) {
    envReg = v;
    constantVol = v & 0x10;
    vol = v & 0x0F;
    envStart = true;
}
void APU::Noise::writeE(uint8_t v) {
    modeReg = v;
    period = noisePeriod[v & 0x0F];
}
void APU::Noise::writeF(uint8_t v) {
    lengthCtr = APU::lengthTable[(v >> 3) & 31];
    envStart = true;
}
void APU::Noise::quarterFrame() {
    if (envStart) {
        envStart = false;
        envVol = 15;
        envDivider = (envReg & 0x0F);
    } else {
        if (envDivider == 0) {
            envDivider = (envReg & 0x0F);
            if (envVol > 0)
                envVol--;
            else if (envReg & 0x20)
                envVol = 15;
        } else
            envDivider--;
    }
}
void APU::Noise::halfFrame() {
    if (!(envReg & 0x20) && lengthCtr > 0) lengthCtr--;
}
void APU::Noise::clockTimer() {
    if (timer == 0) {
        timer = period;
        uint16_t feedback = ((lfsr ^ (lfsr >> ((modeReg & 0x80) ? 6 : 1))) & 1);
        lfsr = (uint16_t)((lfsr >> 1) | (feedback << 14));
    } else
        timer--;
}
float APU::Noise::sample() const {
    if (lengthCtr == 0) return 0.0f;
    uint8_t level = constantVol ? vol : envVol;
    float v = (lfsr & 1) ? 1.0f : -1.0f;
    return v * (level / 15.0f) * 0.25f;
}

// ===== DMC =====
static const uint16_t dmcRate[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 85, 72, 54};
void APU::DMC::write0(uint8_t v) {
    reg0 = v;
    rate = dmcRate[v & 0x0F];
}
void APU::DMC::write1(uint8_t v) { reg1 = v; }
void APU::DMC::write2(uint8_t v) {
    reg2 = v;
    addr = (uint16_t)(0xC000 + 64 * v);
}
void APU::DMC::write3(uint8_t v) {
    reg3 = v;
    length = (uint16_t)(1 + 16 * v);
}
void APU::DMC::restartSample() {
    curAddr = addr;
    bytesLeft = length;
}
void APU::DMC::clockTimer(APU* apu) {
    if (timer > 0) {
        timer--;
        return;
    }
    timer = rate;

    if (!silence) {
        if ((shift & 1) && output <= 125)
            output += 2;
        else if (!(shift & 1) && output >= 2)
            output -= 2;
    }
    shift >>= 1;
    bits--;

    if (bits == 0) {
        bits = 8;
        if (bytesLeft == 0) {
            if (reg0 & 0x40) {  // loop
                restartSample();
            } else {
                if (reg0 & 0x80) apu->dmcIRQ = true;  // IRQ enable
                silence = true;
            }
        }
        if (bytesLeft > 0) {
            uint8_t b = apu->bus->cpuRead(curAddr);
            curAddr = (uint16_t)(curAddr + 1);
            bytesLeft--;
            shift = b;
            silence = false;
        }
    }
}

// ===== Mixer =====
float APU::mix() const {
    float p1 = pulse1.sample(true);
    float p2 = pulse2.sample(false);
    float t = tri.sample();
    float n = noise.sample();
    float d = dmc.sample() * 0.8f;

    // Non-linear mixing approximations (Nesdev)
    float pulseOut = (p1 == 0.0f && p2 == 0.0f) ? 0.0f
                                                : (95.88f / ((8128.0f / (p1 * 15.0f + p2 * 15.0f)) + 100.0f));

    float tndIn = (t / 0.8f) * 8227.0f + (n / 0.25f) * 12241.0f + d * 22638.0f;
    float tndOut = (tndIn == 0.0f) ? 0.0f : (159.79f / (100.0f + tndIn));

    return pulseOut + tndOut;  // ~0..1
}

// ===== Sequencer / API =====
void APU::resetFrameSequencer(bool fiveStep, bool inhibitIRQ, bool immediateClock) {
    mode5 = fiveStep;
    irqInhibit = inhibitIRQ;
    frameIRQ = false;
    fcCycle = 0.0;
    fcStep = 0;

    if (mode5 && immediateClock) {
        // Writing $4017 with bit7=1 clocks both Q & H immediately
        quarterFrame();
        halfFrame();
    }
}

void APU::init() {
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;  // 48k mixes well on Linux
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 2048;      // bigger buffer = fewer xruns
    want.callback = nullptr;  // <<< queued audio, no callback

    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev) return;

    // Match resampler to the actual device rate
    sampleRate = have.freq;  // add member: int sampleRate (or reuse existing)
    samplesPerCpu = double(sampleRate) / 1789773.0;

    SDL_PauseAudioDevice(dev, 0);
}

void APU::shutdown() {
    if (dev) {
        SDL_ClearQueuedAudio(dev);
        SDL_CloseAudioDevice(dev);
    }
    dev = 0;
}

void APU::quarterFrame() {
    pulse1.quarterFrame();
    pulse2.quarterFrame();
    tri.quarterFrame();
    noise.quarterFrame();
}
void APU::halfFrame(){
    pulse1.halfFrame(/*isPulse1=*/true);
    pulse2.halfFrame(/*isPulse1=*/false);
    tri.halfFrame();
    noise.halfFrame();
}
void APU::tickCPU() {
    // channel timers
    pulse1.clockTimer();
    pulse2.clockTimer();
    tri.clockTimer();
    noise.clockTimer();
    dmc.clockTimer(this);

    // audio resampling
    int emit = resampFrac.step(samplesPerCpu);
    static int16_t chunk[2048];
    static int chunkPos = 0;

    while (emit--) {
        float s = std::clamp(mix(), 0.0f, 1.0f);
        int16_t q = (int16_t)((s * 2.0f - 1.0f) * 12000);

        chunk[chunkPos++] = q;
        // Flush in reasonable batches, and keep device topped up
        if (chunkPos >= 512) {
            if (dev) {
                SDL_QueueAudio(dev, chunk, chunkPos * sizeof(int16_t));
            }
            chunkPos = 0;
        }
    }

    // exact frame sequencer cadence
    fcCycle += 1.0;
    if (!mode5) {
        // 4-step: Q at 3729.5, 7457.5, 11186.5, 14915; H at 7457.5, 14915
        if (fcCycle >= 3729.5 && fcStep == 0) {
            quarterFrame();
            fcStep = 1;
        }
        if (fcCycle >= 7457.5 && fcStep <= 1) {
            quarterFrame();
            halfFrame();
            fcStep = 2;
        }
        if (fcCycle >= 11186.5 && fcStep <= 2) {
            quarterFrame();
            fcStep = 3;
        }
        if (fcCycle >= 14915.0 && fcStep <= 3) {
            quarterFrame();
            halfFrame();
            if (!irqInhibit) frameIRQ = true;
            fcCycle -= 14915.0;
            fcStep = 0;
        }
    } else {
        // 5-step: Q at 3729.5, 7457.5, 11186.5, 14915.5, 18641; H at 7457.5, 14915.5; no IRQ
        if (fcCycle >= 3729.5 && fcStep == 0) {
            quarterFrame();
            fcStep = 1;
        }
        if (fcCycle >= 7457.5 && fcStep <= 1) {
            quarterFrame();
            halfFrame();
            fcStep = 2;
        }
        if (fcCycle >= 11186.5 && fcStep <= 2) {
            quarterFrame();
            fcStep = 3;
        }
        if (fcCycle >= 14915.5 && fcStep <= 3) {
            quarterFrame();
            halfFrame();
            fcStep = 4;
        }
        if (fcCycle >= 18641.0 && fcStep <= 4) {
            fcCycle -= 18641.0;
            fcStep = 0;
        }
    }
}

uint8_t APU::cpuRead(uint16_t a) {
    if (a == 0x4015) {
        uint8_t s = 0;
        if (pulse1.lengthCtr > 0) s |= 0x01;
        if (pulse2.lengthCtr > 0) s |= 0x02;
        if (tri.lengthCtr > 0) s |= 0x04;
        if (noise.lengthCtr > 0) s |= 0x08;
        if (dmc.enabled && (dmc.bytesLeft > 0)) s |= 0x10;
        if (frameIRQ) s |= 0x40;
        if (dmcIRQ) s |= 0x80;
        // reading $4015 clears frame & DMC IRQs
        frameIRQ = false;
        dmcIRQ = false;
        return s;
    }
    return 0;
}

void APU::cpuWrite(uint16_t a, uint8_t v) {
    switch (a) {
        // Pulse 1
        case 0x4000:
            pulse1.write0(v);
            break;
        case 0x4001:
            pulse1.write1(v);
            break;
        case 0x4002:
            pulse1.write2(v);
            break;
        case 0x4003:
            pulse1.write3(v);
            break;
        // Pulse 2
        case 0x4004:
            pulse2.write0(v);
            break;
        case 0x4005:
            pulse2.write1(v);
            break;
        case 0x4006:
            pulse2.write2(v);
            break;
        case 0x4007:
            pulse2.write3(v);
            break;
        // Triangle
        case 0x4008:
            tri.write0(v);
            break;
        case 0x400A:
            tri.write2(v);
            break;
        case 0x400B:
            tri.write3(v);
            break;
        // Noise
        case 0x400C:
            noise.writeC(v);
            break;
        case 0x400E:
            noise.writeE(v);
            break;
        case 0x400F:
            noise.writeF(v);
            break;
        // DMC
        case 0x4010:
            dmc.write0(v);
            if (!(v & 0x80)) dmcIRQ = false;
            break;
        case 0x4011:
            dmc.write1(v);
            break;
        case 0x4012:
            dmc.write2(v);
            break;
        case 0x4013:
            dmc.write3(v);
            break;

        case 0x4015: {
            pulse1.enabled = (v & 1);
            if (!pulse1.enabled) pulse1.lengthCtr = 0;
            pulse2.enabled = (v & 2);
            if (!pulse2.enabled) pulse2.lengthCtr = 0;
            tri.enabled = (v & 4);
            if (!tri.enabled) tri.lengthCtr = 0;
            noise.enabled = (v & 8);
            if (!noise.enabled) noise.lengthCtr = 0;
            if (v & 0x10) {
                dmc.enabled = true;
                if (dmc.bytesLeft == 0) dmc.restartSample();
            } else {
                dmc.enabled = false;
                dmc.bytesLeft = 0;
                dmcIRQ = false;
            }
        } break;

        case 0x4017: {
            bool five = (v & 0x80) != 0;
            bool inh = (v & 0x40) != 0;
            resetFrameSequencer(five, inh, /*immediateClock=*/five);
        } break;

        default:
            break;
    }
}
