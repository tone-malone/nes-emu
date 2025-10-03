// cartridge.cpp
#include "cartridge.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "mapper.h"
#include "mapper_mmc1.h"
#include "mapper_mmc3.h"
#include "mapper_nrom.h"



static void readAll(std::ifstream& f, std::vector<uint8_t>& dst, size_t n) {
    dst.resize(n);
    f.read(reinterpret_cast<char*>(dst.data()), n);
    if ((size_t)f.gcount() != n) throw std::runtime_error("Short read");
}

static std::string savPathFor(const std::string& rom) {
    namespace fs = std::filesystem;
    fs::path p(rom);
    return (p.parent_path() / (p.stem().string() + ".sav")).string();
}

std::shared_ptr<Cartridge> Cartridge::loadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open ROM: " + path);
    std::vector<uint8_t> trainer;

    uint8_t h[16];
    f.read((char*)h, 16);
    if (f.gcount() != 16 || h[0] != 'N' || h[1] != 'E' || h[2] != 'S' || h[3] != 0x1A)
        throw std::runtime_error("Not an iNES file");

    uint8_t prgBanks = h[4], chrBanks = h[5];
    uint8_t f6 = h[6], f7 = h[7];
    bool nes2 = ((f7 & 0x0C) == 0x08);

    uint8_t mapperId = ((f7 & 0xF0) | (f6 >> 4));
    bool hasTrainer = (f6 & 0x04) != 0;


    // --- Sizes (defaults) ---
    uint32_t prgRamKB = 8;    // volatile PRG-RAM
    uint32_t prgNvramKB = 0;  // battery-backed PRG-NVRAM
    uint32_t chrRamKB = (chrBanks == 0) ? 8 : 0;

    if (nes2) {
        // NES 2.0: PRG-RAM/NVRAM exponents in h[10]; CHR-RAM exponent in h[11] (if CHR ROM absent)
        uint8_t ramExp = (h[10] & 0x0F);
        uint8_t nvrExp = (h[10] >> 4);
        prgRamKB = (ramExp == 0) ? 0 : (64u << (ramExp - 1));
        prgNvramKB = (nvrExp == 0) ? 0 : (64u << (nvrExp - 1));

        if (chrBanks == 0) {
            uint8_t chrRamExp = (h[11] & 0x0F);
            chrRamKB = (chrRamExp == 0) ? 8 : (64u << (chrRamExp - 1));
        }
    } else {
        prgRamKB = h[8] ? (h[8] * 8u) : 8u;
        prgNvramKB = (f6 & 0x02) ? 8u : 0u;
        if (chrBanks == 0) chrRamKB = 8u;
    }

    bool battery = nes2 ? (prgNvramKB > 0) : ((f6 & 0x02) != 0);

    if (hasTrainer) {
        trainer.resize(512);
        f.read(reinterpret_cast<char*>(trainer.data()), 512);
        if ((size_t)f.gcount() != 512) throw std::runtime_error("Trainer short read");
    }

    std::vector<uint8_t> prg, chr;
    readAll(f, prg, size_t(prgBanks) * 16 * 1024);
    if (chrBanks > 0)
        readAll(f, chr, size_t(chrBanks) * 8 * 1024);
    else
        chr.resize(chrRamKB * 1024, 0);

    auto cart = std::make_shared<Cartridge>();
    cart->mapperId = mapperId;
bool fourScreen = (f6 & 0x08) != 0;
uint8_t mir = fourScreen ? 4 : ((f6 & 1) ? 1 : 0);
cart->mirroring = mir;

    cart->batteryBacked = battery;
    cart->romPath = path;

    uint32_t prgRamForMapper = std::max(prgRamKB, prgNvramKB);
    switch (mapperId) {
        case 0:
            cart->mapper = std::make_shared<MapperNROM>(std::move(prg), std::move(chr), (chrBanks == 0), mir);
            break;
        case 1:
            cart->mapper = std::make_shared<MapperMMC1>(std::move(prg), std::move(chr), (chrBanks == 0), mir, prgRamForMapper);
            break;
        case 4:
            cart->mapper = std::make_shared<MapperMMC3>(std::move(prg), std::move(chr), mir, prgRamForMapper);
            break;
        default:
            cart->mapper = std::make_shared<MapperNROM>(std::move(prg), std::move(chr), (chrBanks == 0), mir);
            break;
    }
    // Inject trainer into PRG-RAM at $7000-$71FF if present
    if (!trainer.empty()) {
        if (auto* ram = cart->mapper->prgRamData()) {
            size_t rsz = cart->mapper->prgRamSize();
            if (rsz >= 0x1200) {
                std::memcpy(ram + 0x1000, trainer.data(), 512);  // $7000-$71FF
            }
        }
    }

    cart->loadSave();
    return cart;
}
uint8_t Cartridge::cpuRead(uint16_t a) { return mapper->cpuRead(a); }
void Cartridge::cpuWrite(uint16_t a, uint8_t v) { mapper->cpuWrite(a, v); }
uint8_t Cartridge::ppuRead(uint16_t a) { return mapper->ppuRead(a); }
void Cartridge::ppuWrite(uint16_t a, uint8_t v) { mapper->ppuWrite(a, v); }

void Cartridge::loadSave() {
    if (!batteryBacked || !mapper) return;
    auto* ram = mapper->prgRamData();
    size_t sz = mapper->prgRamSize();
    if (!ram || sz == 0) return;

    std::ifstream s(savPathFor(romPath), std::ios::binary);
    if (!s) return;
    s.read(reinterpret_cast<char*>(ram), sz);
}
void Cartridge::saveSave() {
    if (!batteryBacked || !mapper) return;
    auto* ram = mapper->prgRamData();
    size_t sz = mapper->prgRamSize();
    if (!ram || sz == 0) return;

    std::ofstream s(savPathFor(romPath), std::ios::binary | std::ios::trunc);
    if (!s) return;
    s.write(reinterpret_cast<const char*>(ram), sz);
}
