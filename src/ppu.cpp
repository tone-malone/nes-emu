// ppu.cpp

#include "ppu.h"

#include <algorithm>
#include <cstring>

#include "cartridge.h"
#include "mapper.h"

// --- NTSC palette (approx) ---
const std::array<uint32_t, 64> PPU::kNesPalette = {
    0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0700, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0E9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFFFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36CFF, 0xFFFF6EBC, 0xFFFF7D6A, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFFFF, 0xFFC0E0FF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFFC4EA, 0xFFFFC9C9, 0xFFF2D3A6,
    0xFFE5DE8A, 0xFFCCEA8E, 0xFFB7F4A5, 0xFFA9F4C7, 0xFFA7E9EE, 0xFFA8A8A8, 0xFF000000, 0xFF000000};

static inline uint16_t mirrorNT(uint16_t a, uint8_t mir) {
    // Map 0x2000..0x2FFF to 0..0x7FF with mirroring (0=horiz,1=vert)
    uint16_t nt = (a - 0x2000) & 0x0FFF;
    uint16_t table = nt / 0x0400;
    uint16_t off = nt & 0x03FF;
    if (mir == 0) {  // horizontal: [A,A,B,B]
        uint16_t phys = (table < 2) ? 0 : 1;
        return phys * 0x0400 + off;
    } else {  // vertical: [A,B,A,B]
        uint16_t phys = table & 1;
        return phys * 0x0400 + off;
    }
}

// Map 0x2000..0x2FFF into PPU nametable VRAM, honoring mapper mirroring.
// 0=horiz [A,A,B,B], 1=vert [A,B,A,B], 2=single A, 3=single B, 4=four-screen [A,B,C,D]
static inline uint16_t mapNT(uint16_t a, uint8_t mir) {
    uint16_t nt = (a - 0x2000) & 0x0FFF;  // 4K window
    uint16_t table = nt >> 10;            // 0..3
    uint16_t off = nt & 0x03FF;           // 0..0x3FF
    switch (mir) {
        case 0:
            return ((table & 2) ? 0x400 : 0x000) + off;  // horiz
        case 1:
            return ((table & 1) ? 0x400 : 0x000) + off;  // vert
        case 2:
            return 0x000 + off;  // single-screen A
        case 3:
            return 0x400 + off;  // single-screen B
        case 4:
            return (table * 0x400) + off;  // four-screen
        default:
            return ((table & 1) ? 0x400 : 0x000) + off;  // fallback: vert
    }
}

// Palette read with mirroring of $10/$14/$18/$1C to $00/$04/$08/$0C
static inline uint8_t readPaletteEntry(const uint8_t* pal, uint8_t idx) {
    idx &= 0x1F;
    if ((idx & 0x13) == 0x10) idx &= ~0x10;  // mirror $10/$14/$18/$1C
    return pal[idx] & 0x3F;
}

uint8_t PPU::ppuRead(uint16_t addr) {
    addr &= 0x3FFF;

    uint8_t m = cart && cart->mapper ? cart->mapper->mirroring() : cart->mirroring;

    if (addr < 0x2000) return cart->ppuRead(addr);
    if (addr < 0x3F00) return vram[mapNT(addr, m)];  // <-- use mapNT
    if (addr < 0x4000) {
        uint16_t p = (addr - 0x3F00) & 0x1F;
        if ((p & 0x13) == 0x10) p &= ~0x10;
        return palette[p] & 0x3F;
    }
    return 0;
}

void PPU::ppuWrite(uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;

    uint8_t m = cart && cart->mapper ? cart->mapper->mirroring() : cart->mirroring;

    if (addr < 0x2000) {
        cart->ppuWrite(addr, val);
        return;
    }
    if (addr < 0x3F00) {
        vram[mapNT(addr, m)] = val;
        return;
    }  // <-- use mapNT
    if (addr < 0x4000) {
        uint16_t p = (addr - 0x3F00) & 0x1F;
        if ((p & 0x13) == 0x10) p &= ~0x10;
        palette[p] = val;
        return;
    }
}

uint8_t PPU::cpuReadRegister(uint16_t addr) {
    switch (addr & 7) {
        case 2: {  // PPUSTATUS
            uint8_t v = PPUSTATUS;
            PPUSTATUS &= ~0x80;  // clear vblank
            addrLatch = false;
            nmi_occurred = false;
            return v;
        }
        case 4:
            return oam[OAMADDR];
        case 7: {
            uint8_t ret = vramReadBuffer;
            vramReadBuffer = ppuRead(v);
            if ((v & 0x3FFF) >= 0x3F00) ret = vramReadBuffer;
            v = (uint16_t)((v + incrementVRAMAddr()) & 0x7FFF);
            return ret;
        }
        default:
            return 0;  // PPUCTRL/PPUMASK/PPUSCROLL/PPUADDR are write-only
    }
}

void PPU::cpuWriteRegister(uint16_t addr, uint8_t val) {
    switch (addr & 7) {
        case 0:  // PPUCTRL
            PPUCTRL = val;
            t = (uint16_t)((t & 0xF3FF) | ((val & 3) << 10));
            break;
        case 1:
            PPUMASK = val;
            break;
        case 3:
            OAMADDR = val;
            break;
        case 4:
            oam[OAMADDR++] = val;
            break;
        case 5:  // PPUSCROLL
            if (!addrLatch) {
                fineX = val & 7;
                t = (uint16_t)((t & 0x7FE0) | ((val >> 3) & 0x1F));
                addrLatch = true;
            } else {
                t = (uint16_t)((t & 0x0C1F) | ((val & 0x07) << 12) | ((val & 0xF8) << 2));
                addrLatch = false;
            }
            break;
        case 6:  // PPUADDR
            if (!addrLatch) {
                t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8));
                addrLatch = true;
            } else {
                t = (uint16_t)((t & 0x7F00) | val);
                v = t;
                addrLatch = false;
            }
            break;
        case 7:
            ppuWrite(v, val);
            v = (uint16_t)((v + incrementVRAMAddr()) & 0x7FFF);
            break;
    }
}

void PPU::oamDMA(const std::function<uint8_t(uint8_t)>& fetch256) {
    uint8_t start = OAMADDR;  // hardware starts at OAMADDR and wraps
    for (int i = 0; i < 256; ++i) {
        oam[(uint8_t)(start + i)] = fetch256((uint8_t)i);
    }
    // After DMA, OAMADDR is effectively unchanged (wrap of +256)
}

uint32_t PPU::universalRGBA() const {
    return kNesPalette[universalIndex()];
}

void PPU::setVBlank() {
    PPUSTATUS |= 0x80;
    nmi_occurred = true;
}
void PPU::clearVBlank() {
    PPUSTATUS &= ~0x80;
    nmi_occurred = false;
}

void PPU::resetFrameState() {
    std::memset(lineBG, 0, sizeof(lineBG));
    std::memset(lineBGPix, 0, sizeof(lineBGPix));
    std::memset(lineSPColIdx, 0, sizeof(lineSPColIdx));
    std::memset(lineSPPix, 0, sizeof(lineSPPix));
    std::memset(lineSPPrio, 0, sizeof(lineSPPrio));
    std::memset(lineSP0Mask, 0, sizeof(lineSP0Mask));
}

void PPU::startScanline() {
    if (scanline == 261) {  // pre-render
        clearVBlank();
        PPUSTATUS &= ~0x40;  // clear sprite-0 hit
        PPUSTATUS &= ~0x20;  // clear sprite overflow
    }
    resetFrameState();
    secCount = 0;
    std::memset(secOAM, 0xFF, sizeof(secOAM));
}

void PPU::endScanline() {
    if (scanline >= 0 && scanline < HEIGHT) {
        const bool showBG = (PPUMASK & 0x08) != 0;
        const bool showSP = (PPUMASK & 0x10) != 0;
        const bool bgLeft8 = (PPUMASK & 0x02) != 0;
        const bool spLeft8 = (PPUMASK & 0x04) != 0;

        for (int x = 0; x < WIDTH; x++) {
            bool bgMasked = (!bgLeft8 && x < 8);
            bool spMasked = (!spLeft8 && x < 8);

            uint8_t bgCol = (showBG && !bgMasked) ? lineBG[x] : universalIndex();
            uint8_t bgRaw = (showBG && !bgMasked) ? lineBGPix[x] : 0;

            uint8_t spCol = (showSP && !spMasked) ? lineSPColIdx[x] : 0;
            uint8_t spRaw = (showSP && !spMasked) ? lineSPPix[x] : 0;
            bool behind = lineSPPrio[x] != 0;

            uint8_t out = bgCol;
            if (spRaw && (!behind || bgRaw == 0)) {
                out = spCol;
            }
            framebuffer[scanline * WIDTH + x] = kNesPalette[out];

            // sprite-0 hit: requires nonzero raw BG+SP pixels, visible, obey left-8 masks
            if (lineSP0Mask[x] && (bgRaw != 0) && (spRaw != 0)) {
                bool bgLeftOK = bgLeft8 || (x >= 8);
                bool spLeftOK = spLeft8 || (x >= 8);
                if (showBG && showSP && bgLeftOK && spLeftOK) PPUSTATUS |= 0x40;
            }
        }
    }

    // Vertical increment/copies handled in tick() at exact dots
}

void PPU::evaluateSpritesWindow() {
    // We keep the coarse behavior (select <=8 sprites for NEXT line). Dot-perfect later.
    if (scanline < 0 || scanline >= HEIGHT) return;

    if (secCount == 0) {
        std::memset(secOAM, 0xFF, sizeof(secOAM));
    }

    int sprH = (PPUCTRL & 0x20) ? 16 : 8;
    // One pass selection; overflow when >8
    int found = 0;
    for (int i = 0; i < 64 && found < 8; i++) {
        uint8_t y = oam[i * 4 + 0];
        int top = (int)y + 1;
        if (scanline >= top && scanline < top + sprH) {
            std::memcpy(&secOAM[found * 4], &oam[i * 4], 4);

            found++;
        }
    }
    secCount = found;
    if (found >= 8) {
        PPUSTATUS |= 0x20;  // overflow (simplified)
    }
}

void PPU::renderSpritesForLine() {
    if (scanline < 0 || scanline >= HEIGHT) return;

    const bool showSP = (PPUMASK & 0x10) != 0;
    const bool spLeft8 = (PPUMASK & 0x04) != 0;

    std::memset(lineSPColIdx, 0, sizeof(lineSPColIdx));
    std::memset(lineSPPix, 0, sizeof(lineSPPix));
    std::memset(lineSPPrio, 0, sizeof(lineSPPrio));
    std::memset(lineSP0Mask, 0, sizeof(lineSP0Mask));

    if (!showSP) return;

    int sprH = (PPUCTRL & 0x20) ? 16 : 8;

    auto fetchRow = [&](uint8_t tile, int row, bool flipV) -> std::pair<uint8_t, uint8_t> {
        uint16_t base;
        if (sprH == 16) {
            uint8_t tableSel = tile & 1;
            uint8_t topTile = tile & 0xFE;
            int r = flipV ? (15 - row) : row;
            if (r < 8)
                base = (tableSel ? 0x1000 : 0x0000) + (uint16_t)topTile * 16 + r;
            else
                base = (tableSel ? 0x1000 : 0x0000) + (uint16_t)(topTile + 1) * 16 + (r - 8);
        } else {
            int r = flipV ? (7 - row) : row;
            base = ((PPUCTRL & 0x08) ? 0x1000 : 0x0000) + (uint16_t)tile * 16 + r;
        }
        curChrAddr = base;
        uint8_t p0 = ppuRead(base);
        curChrAddr = base + 8;
        uint8_t p1 = ppuRead(base + 8);

        return {p0, p1};
    };

    for (int s = 0; s < secCount; ++s) {
        uint8_t y = secOAM[s * 4 + 0];
        uint8_t tile = secOAM[s * 4 + 1];
        uint8_t attr = secOAM[s * 4 + 2];
        uint8_t x = secOAM[s * 4 + 3];

        int row = scanline - ((int)y + 1);
        if (row < 0 || row >= sprH) continue;  // skip if not in sprite row

        bool flipH = (attr & 0x40) != 0;
        bool flipV = (attr & 0x80) != 0;
        bool behind = (attr & 0x20) != 0;
        uint8_t pal = (attr & 0x03);

        auto bits = fetchRow(tile, row, flipV);
        uint8_t p0 = bits.first, p1 = bits.second;

        for (int c = 0; c < 8; c++) {
            int bit = flipH ? c : (7 - c);
            int sx = x + c;
            if (sx < 0 || sx >= WIDTH) continue;
            if (!spLeft8 && sx < 8) continue;

            uint8_t lo = (p0 >> bit) & 1;
            uint8_t hi = (p1 >> bit) & 1;
            uint8_t pix = (uint8_t)((hi << 1) | lo);
            if (pix == 0) continue;       // transparent
            if (lineSPPix[sx]) continue;  // OAM priority test must use *raw* spr pix, not color

            uint8_t palIndex = (uint8_t)(0x10 + pal * 4 + pix);
            if ((palIndex & 0x1F) == 0x10) palIndex = 0x00;  // $3F10 mirrors $3F00
            uint8_t idx = palette[palIndex & 0x1F] & 0x3F;

            lineSPPix[sx] = pix;     // <-- raw 1..3
            lineSPColIdx[sx] = idx;  // final color index
            lineSPPrio[sx] = behind ? 1 : 0;
            if (s == 0) lineSP0Mask[sx] = 1;
        }
    }
}

void PPU::tick() {
    a12ThisDot = false;  // reset for this PPU dot

    // ----- BG pixel composition (sample first) -----
    if (scanline >= 0 && scanline < HEIGHT && dot >= 1 && dot <= 256) {
        int x = dot - 1;
        uint8_t bgPix = 0, pal2 = 0;

        if ((PPUMASK & 0x08) != 0) {
            uint8_t lo = (uint8_t)((bgShiftLo >> (15 - fineX)) & 1);
            uint8_t hi = (uint8_t)((bgShiftHi >> (15 - fineX)) & 1);
            bgPix = (uint8_t)((hi << 1) | lo);

            uint8_t alo = (uint8_t)((attrShiftLo >> (15 - fineX)) & 1);
            uint8_t ahi = (uint8_t)((attrShiftHi >> (15 - fineX)) & 1);
            bgShiftLo <<= 1;
            bgShiftHi <<= 1;
            attrShiftLo <<= 1;
            attrShiftHi <<= 1;
            pal2 = (uint8_t)((ahi << 1) | alo);

            if (!(PPUMASK & 0x02) && x < 8) bgPix = 0;  // left-8 BG mask
        }

        lineBGPix[x] = bgPix;  // <-- raw 0..3
                               // store the *final* BG color index too, for fast compositing when bgPix!=0
        lineBG[x] = (bgPix == 0) ? universalIndex()
                                 : (palette[(pal2 << 2) + bgPix] & 0x3F);
    }

    // reload the shifters for tile 0 only. Do NOT increment coarse X here.
if (renderingEnabled() && dot == 1) {
    bgShiftLo = (bgShiftLo & 0xFF00) | patLoLatch;
    bgShiftHi = (bgShiftHi & 0xFF00) | patHiLatch;
    reloadAttrShifters(atLatch);
}

    // ----- BG fetch pipeline (1..256 and 321..340) -----
    bool fetchWindow = renderingEnabled() &&
                       ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 340));

    if (fetchWindow) {
        uint8_t fineY = (uint8_t)((v >> 12) & 7);
        uint16_t patBase = (PPUCTRL & 0x10) ? 0x1000 : 0x0000;

        switch (dot % 8) {
            case 1: /* NT */ {
                uint16_t ntAddr = 0x2000 | (v & 0x0FFF);
                ntLatch = ppuRead(ntAddr);
            } break;
            case 3: /* AT */ {
                uint16_t atAddr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
                uint8_t at = ppuRead(atAddr);
                int shift = ((((v >> 5) & 2) | ((v >> 1) & 1)) * 2);
                atLatch = (uint8_t)((at >> shift) & 0x03);
            } break;
            case 5: /* PAT0 */ {
                uint16_t p0 = patBase + ntLatch * 16 + fineY;
                curChrAddr = p0;
                patLoLatch = ppuRead(p0);
                if (dot <= 256) a12ThisDot |= (p0 & 0x1000) != 0;
            } break;
            case 7: /* PAT1 */ {
                uint16_t p1 = patBase + ntLatch * 16 + fineY + 8;
                curChrAddr = p1;
                patHiLatch = ppuRead(p1);
                if (dot <= 256) a12ThisDot |= (p1 & 0x1000) != 0;
            } break;
            case 0: /* tile boundary */ {
                // reload and THEN increment coarse X (this is for dots 8,16,24,...)
                bgShiftLo = (bgShiftLo & 0xFF00) | patLoLatch;
                bgShiftHi = (bgShiftHi & 0xFF00) | patHiLatch;
                reloadAttrShifters(atLatch);

                if ((v & 0x001F) == 31) {
                    v &= ~0x001F;
                    v ^= 0x0400;
                } else {
                    v += 1;
                }
            } break;
        }
    }

    if (scanline >= 0 && scanline < HEIGHT && dot == 260) {
        if (cart && cart->mapper) {
            cart->mapper->ppuOnScanlineDot260(renderingEnabled());
        }
    }

    // ----- Sprites (same coarse behavior) -----
    if (scanline >= 0 && scanline < HEIGHT) {
        if (dot == 65) {
            secCount = 0;
            std::memset(secOAM, 0xFF, sizeof(secOAM));
            PPUSTATUS &= ~0x20;
            evaluateSpritesWindow();
        }
        if (dot == 257) {
            renderSpritesForLine();
        }

        // Vertical increment at 256
        if (renderingEnabled() && dot == 256) {
            if ((v & 0x7000) != 0x7000) {
                v += 0x1000;
            } else {
                v &= ~0x7000;
                uint16_t y = (v & 0x03E0) >> 5;
                if (y == 29) {
                    y = 0;
                    v ^= 0x0800;
                } else if (y == 31) {
                    y = 0;
                } else {
                    y += 1;
                }
                v = (uint16_t)((v & ~0x03E0) | (y << 5));
            }
        }
    }

    // Horizontal copy at 257 MUST run on every scanline (incl. pre-render)
    if (renderingEnabled() && dot == 257) copyHorizontal();

    // Pre-render line (261): vertical copy 280..304; odd-frame skip at 339
    if (scanline == 261 && renderingEnabled()) {
        if (dot >= 280 && dot <= 304) copyVertical();
        if (frame_odd && dot == 339) {
            // make sure the mapper sees a low (no CHR fetch) sample on this skipped dot
            if (cart && cart->mapper) cart->mapper->ppuA12Clock(false);
            dot = 0;
            endScanline();
            scanline = 0;
            startScanline();
            return;
        }
    }

    // Advance time
    dot++;

    // VBlank starts at (241,1)
    if (scanline == 241 && dot == 1) setVBlank();

    // End scanline after dot 340 (0..340 inclusive)
    if (dot > 340) {
        dot = 0;
        endScanline();
        scanline++;
        if (scanline > 261) {
            scanline = 0;
            frame_odd = !frame_odd;
        }
        startScanline();
    }

    // One A12 sample per PPU dot, based on last CHR fetch address.
    if (cart && cart->mapper) {
        cart->mapper->ppuA12Clock(a12ThisDot);
    }
}
