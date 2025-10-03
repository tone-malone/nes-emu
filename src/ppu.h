// ppu.h
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>

struct Cartridge;

struct PPU {
    Cartridge* cart = nullptr;

    // Nametable VRAM (4 KiB) — 
    uint8_t vram[4096]{};

    // Primary & secondary OAM
    uint8_t oam[256]{};
    uint8_t secOAM[32]{};
    int secCount = 0;

    // Palette RAM $3F00..$3F1F
    uint8_t palette[32]{};

    // Registers
    uint8_t PPUCTRL = 0, PPUMASK = 0, PPUSTATUS = 0x00, OAMADDR = 0;

    // Loopy regs (v/t/fineX) & toggle for PPUSCROLL/PPUADDR
    uint16_t v = 0, t = 0;
    uint8_t fineX = 0;
    bool addrLatch = false;

    // PPUDATA buffered read
    uint8_t vramReadBuffer = 0;

    // Timing state
    int scanline = 261;  // -1=>pre-render (we use 261)
    int dot = 0;         // 0..340
    bool frame_odd = false;
    bool a12ThisDot = false;  // true if any CHR fetch (addr < $2000) with A12=1 happened this dot

    // NMI edge state
    bool nmi_output() const { return (PPUCTRL & 0x80) != 0; }
    bool nmi_occurred = false;

    // Frame buffer (RGBA8888)
    static constexpr int WIDTH = 256, HEIGHT = 240;
    uint32_t framebuffer[WIDTH * HEIGHT];

    // Per-scanline BG/SP staging (indices into kNesPalette)
    uint8_t lineBG[WIDTH]{};
    uint8_t lineSPColIdx[WIDTH]{};
    uint8_t lineSPPrio[WIDTH]{};
    uint8_t lineSP0Mask[WIDTH]{};
    uint8_t lineBGPix[WIDTH]{};  // raw BG pixel value 0..3 (0 = transparent)
    uint8_t lineSPPix[WIDTH]{};  // raw SPR pixel value 0..3 (0 = transparent)

    // --- BG pipeline shifters (dot-exact) ---
    uint16_t bgShiftLo = 0, bgShiftHi = 0;
    uint16_t attrShiftLo = 0, attrShiftHi = 0;
    uint8_t ntLatch = 0, atLatch = 0, patLoLatch = 0, patHiLatch = 0;
    uint16_t curChrAddr = 0;  // last CHR fetch addr (for mapper A12 clocking)

    // Public API
    void connect(Cartridge* c) { cart = c; }
    uint8_t cpuReadRegister(uint16_t addr);
    void cpuWriteRegister(uint16_t addr, uint8_t v);
    void oamDMA(const std::function<uint8_t(uint8_t)>& fetch256);

    // Ticking
    void tick();             // advance 1 PPU dot
    void resetFrameState();  // clear per-line buffers

    // PPU memory
    uint8_t ppuRead(uint16_t addr);
    void ppuWrite(uint16_t addr, uint8_t val);

    // NES palette (approx)
    static const std::array<uint32_t, 64> kNesPalette;

   private:
    // Nametable mirroring helpers
    uint8_t readNametable(uint16_t a);
    void writeNametable(uint16_t a, uint8_t v);

    // Scanline phases
    void startScanline();
    void endScanline();

    // BG & Sprite pipelines
    void evaluateSpritesWindow();  // dots 65..256: select ≤8 sprites for next line into secOAM
    void renderSpritesForLine();   // dot 257: build sprite line buffers from secOAM

    // VBlank / NMI
    void setVBlank();
    void clearVBlank();

    // Rendering toggles & scroll copies
    inline uint16_t incrementVRAMAddr() const {
        return (PPUCTRL & 0x04) ? 32 : 1;  // $2000 bit2
    }

    inline bool renderingEnabled() const {
        return (PPUMASK & 0x18) != 0;  // BG or SPR enabled
    }
    inline void copyHorizontal() {
        // v: ....F.. ...EDCBA  -> copy coarse X + nametable X
        // t: ....F.. ...EDCBA
        v = (uint16_t)((v & 0x7BE0) | (t & 0x041F));
    }

    inline void copyVertical() {
        // v: IHGF... ..543.. -> copy fine Y + coarse Y + nametable Y
        // t: IHGF... ..543..
        v = (uint16_t)((v & 0x041F) | (t & 0x7BE0));
    }

    // Universal background
    inline uint8_t universalIndex() const { return palette[0] & 0x3F; }
    uint32_t universalRGBA() const;

    // Attribute shifter refill
    inline void reloadAttrShifters(uint8_t pal2) {
        // pal2: 2-bit attribute for the tile (0..3)
        uint16_t lo = (pal2 & 1) ? 0x00FF : 0x0000;
        uint16_t hi = (pal2 & 2) ? 0x00FF : 0x0000;
        attrShiftLo = (attrShiftLo & 0xFF00) | lo;
        attrShiftHi = (attrShiftHi & 0xFF00) | hi;
    }
};
