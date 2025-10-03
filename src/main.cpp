// main.cpp
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "input.h"
#include "nes.h"
#include "ppu.h"
#include "timgui.h"

namespace fs = std::filesystem;

// --------------------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------------------

static void uploadNESFrame(SDL_Texture* tex, const uint32_t* fb) {
    // Upload 256x240 RGBA8888 into the texture (no present here; caller decides order)
    SDL_UpdateTexture(tex, nullptr, fb, 256 * sizeof(uint32_t));
}

static SDL_Rect letterboxDest(SDL_Window* win, int baseW, int baseH, bool integerScale) {
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);

    float sx = (float)ww / (float)baseW;
    float sy = (float)wh / (float)baseH;
    float s = std::min(sx, sy);

    if (integerScale) s = std::max(1.0f, std::floor(s));

    int w = (int)std::round(baseW * s);
    int h = (int)std::round(baseH * s);
    SDL_Rect r;
    r.w = w;
    r.h = h;
    r.x = (ww - w) / 2;
    r.y = (wh - h) / 2;
    return r;
}

static void setScaleQuality(bool linear) {
    // nearest / linear hint for SDL texture filtering
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "1" : "0");
}

static std::vector<std::string> listNES(const std::string& folder) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) return out;
    for (auto& e : fs::directory_iterator(folder, ec)) {
        if (e.is_regular_file()) {
            auto p = e.path();
            auto ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".nes") out.push_back(p.string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::string baseName(const std::string& path) {
    fs::path p(path);
    return p.filename().string();
}

static std::string defaultFontPath() {
#if defined(_WIN32)
    return "C:\\Windows\\Fonts\\arial.ttf";
#elif defined(__APPLE__)
    return "/System/Library/Fonts/Supplemental/Arial.ttf";
#else
    return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#endif
}

// --------------------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // ------------------ CLI / initial ROM path ------------------
    std::string initialRomPath;
    if (argc >= 2) {
        initialRomPath = argv[1];
    }

    // ------------------ Window / Renderer ------------------
    const int baseW = 256, baseH = 240;  // NES output
    SDL_Window* win = SDL_CreateWindow(
        "NES (C++/SDL + timgui)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        baseW * 3, baseH * 3,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }

    Uint32 rflags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, rflags);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 3;
    }

    // We’ll handle scaling manually; no logical size (so GUI is crisp at native pixels).
    // SDL_RenderSetLogicalSize(ren, baseW, baseH); // <- leave disabled

    // NES texture (ARGB8888 matches your framebuffer)
    setScaleQuality(false);  // nearest by default
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, baseW, baseH);
    if (!tex) {
        std::fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 4;
    }

    // ------------------ NES core ------------------
    NES nes;
    auto loadAndBoot = [&](const std::string& romPath) -> bool {
        try {
            if (romPath.empty()) return false;
            if (!nes.loadROM(romPath)) throw std::runtime_error("loadROM failed");
            nes.powerOn();
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error: %s\n", e.what());
            return false;
        }
    };

    bool hasGame = false;
    if (!initialRomPath.empty()) hasGame = loadAndBoot(initialRomPath);

    // Open first available controller (optional)
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            // Note: NES::powerOn creates Input; we open handle into nes.input
            if (nes.input) {
                nes.input->controller = SDL_GameControllerOpen(i);
            }
            break;
        }
    }

    // ------------------ timgui ------------------
    timgui::CreateContext();
    timgui::Init(ren, defaultFontPath().c_str(), 16);
    SDL_StartTextInput();

    // UI state
    bool showUI = true;
    bool paused = false;
    bool integerScale = true;
    int scaleFilter = 0;  // 0=nearest, 1=linear

    std::string romFolder = initialRomPath.empty() ? fs::current_path().string()
                                                   : fs::path(initialRomPath).parent_path().string();
    auto romList = listNES(romFolder);
    int selectedRom = 0;
    if (!romList.empty()) {
        // try to pick the passed one if present
        if (!initialRomPath.empty()) {
            auto it = std::find(romList.begin(), romList.end(), fs::absolute(initialRomPath).string());
            if (it != romList.end()) selectedRom = int(it - romList.begin());
        }
    }

    // FPS counter (simple)
    Uint64 ticksPrev = SDL_GetPerformanceCounter();
    double fps = 0.0;
    bool browserOpen = true;
    bool running = true;
    while (running) {
        // ------------------ Begin UI frame ------------------
        timgui::NewFrame();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Feed UI first
            timgui::HandleSDLEvent(e);

            if (e.type == SDL_QUIT) running = false;

            if (e.type == SDL_KEYDOWN) {
                const SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_ESCAPE) {  // ESC toggles UI
                    showUI = !showUI;
                } else if (key == SDLK_F5) {  // F5 pause
                    paused = !paused;
                } else if (key == SDLK_F1) {  // F1 reset (power cycle is safer here)
                    if (hasGame) hasGame = loadAndBoot(initialRomPath.empty() ? romList.empty() ? "" : romList[selectedRom]
                                                                              : initialRomPath);
                } else if (key == SDLK_F2) {  // NEW: Toggle ROM Browser window only
                    browserOpen = !browserOpen;
                }
            }

            // Controller hotplug
            if (e.type == SDL_CONTROLLERDEVICEADDED) {
                if (nes.input && !nes.input->controller && SDL_IsGameController(e.cdevice.which)) {
                    nes.input->controller = SDL_GameControllerOpen(e.cdevice.which);
                }
            }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (nes.input && nes.input->controller) {
                    SDL_GameControllerClose(nes.input->controller);
                    nes.input->controller = nullptr;
                }
            }
        }

        // ------------------ Emulator step ------------------
        if (hasGame && !paused) {
            nes.runFrame();
        }

        // Upload the current framebuffer (even if paused)
        if (nes.ppu) {
            uploadNESFrame(tex, nes.ppu->framebuffer);
        }

        // ------------------ Build UI ------------------
        if (showUI) {
            // Menu bar (overlay-style in timgui)
            if (timgui::BeginMenuBar()) {
                if (timgui::BeginMenu("File")) {
                    if (timgui::MenuItem("Open folder…")) {
                        // Just focus the ROM Browser window; enter a path there
                        // (No native OS dialog here; keep it portable)
                    }
                    if (timgui::MenuItem("Reset", hasGame)) {
                        if (hasGame) {
                            // power cycle is simplest & safest
                            hasGame = loadAndBoot(initialRomPath.empty() ? (romList.empty() ? "" : romList[selectedRom])
                                                                         : initialRomPath);
                        }
                    }
                    timgui::MenuSeparator();
                    if (timgui::MenuItem("Quit")) running = false;
                    timgui::EndMenu();
                }

                if (timgui::BeginMenu("View")) {
                    // Toggle integer scaling
                    if (timgui::MenuItem("Integer scaling", true, integerScale ? "On" : "Off",
                                         "Pixel-perfect 1x/2x/3x…")) {
                        integerScale = !integerScale;
                    }

                    // NEW: Toggle ROM Browser window
                    if (timgui::MenuItem(browserOpen ? "Hide ROM Browser (F2)"
                                                     : "Show ROM Browser (F2)")) {
                        browserOpen = !browserOpen;
                    }

                    // Scale filter submenu (unchanged)
                    int prevFilter = scaleFilter;
                    if (timgui::BeginSubMenu("Scale filter")) {
                        (void)timgui::RadioButton("Nearest (sharp)", &scaleFilter, 0);
                        (void)timgui::RadioButton("Linear (smooth)", &scaleFilter, 1);
                        timgui::EndSubMenu();
                    }
                    if (prevFilter != scaleFilter) {
                        setScaleQuality(scaleFilter == 1);
                    }

                    timgui::EndMenu();
                }

                if (timgui::BeginMenu("Emulator")) {
                    if (timgui::MenuItem(paused ? "Resume (F5)" : "Pause (F5)", hasGame)) {
                        paused = !paused;
                    }
                    timgui::MenuSeparator();
                    timgui::TextF("FPS: %.1f", fps);
                    timgui::EndMenu();
                }

                if (timgui::BeginMenu("Help")) {
                    (void)timgui::MenuItem("F5 = Pause/Resume");
                    (void)timgui::MenuItem("F1 = Reset current ROM");
                    (void)timgui::MenuItem("Esc = Toggle UI");
                    timgui::EndMenu();
                }
                timgui::EndMenuBar();
            }

            // ROM Browser window
            if (browserOpen && timgui::Begin("ROM Browser", &browserOpen, 20, 60, 520, 520)) {
                timgui::TextWrapped("Enter a folder and pick a .nes file. Click Load to power on.");
                timgui::NewLine();
                // Folder input + Scan
                static char folderBuf[1024];
                static bool folderInit = false;
                if (!folderInit) {
                    std::snprintf(folderBuf, sizeof(folderBuf), "%s", romFolder.c_str());
                    folderInit = true;
                }

                if (timgui::InputText("Folder", folderBuf, sizeof(folderBuf))) {
                    // edited in-place; user still needs to press Scan
                }
                timgui::NewLine();
                if (timgui::Button("Scan")) {
                    romFolder = std::string(folderBuf);
                    romList = listNES(romFolder);
                    selectedRom = 0;
                }

                timgui::Separator();

                // Build short names and show a clean ListBox (no child panel — avoids overlap)
                std::vector<std::string> shortNames;
                shortNames.reserve(romList.size());
                for (auto& s : romList) shortNames.push_back(baseName(s));

                // Choose a sensible number of visible rows (min 6, max 22)
                int visibleRows = std::clamp((int)shortNames.size(), 6, 22);
                if (timgui::ListBox("ROMs", &selectedRom, shortNames, visibleRows)) {
                    // selection changed (optional: preview or status)
                }

                timgui::Separator();

                // Robust 3-column button layout (prevents overlap with the list)
                timgui::Columns(3);
                {
                    if (timgui::Button("Load")) {
                        if (selectedRom >= 0 && selectedRom < (int)romList.size()) {
                            initialRomPath.clear();
                            hasGame = loadAndBoot(romList[selectedRom]);
                            paused = false;
                        }
                    }
                    timgui::NextColumn();

                    if (timgui::Button(paused ? "Resume" : "Pause")) {
                        if (hasGame) paused = !paused;
                    }
                    timgui::NextColumn();

                    if (timgui::Button("Reset")) {
                        if (hasGame) {
                            hasGame = loadAndBoot(initialRomPath.empty()
                                                      ? (romList.empty() ? "" : romList[selectedRom])
                                                      : initialRomPath);
                            paused = false;
                        }
                    }
                }
                timgui::EndColumns();

                timgui::Separator();

                if (hasGame) {
                    timgui::TextF("Running: %s",
                                  initialRomPath.empty()
                                      ? ((selectedRom >= 0 && selectedRom < (int)romList.size())
                                             ? baseName(romList[selectedRom]).c_str()
                                             : "(unknown)")
                                      : baseName(initialRomPath).c_str());
                } else {
                    timgui::Text("No game loaded.");
                }
            }
            timgui::End();

        }  // showUI

        timgui::EndFrame();

        // ------------------ Render ------------------
        SDL_SetRenderDrawColor(ren, 12, 12, 14, 255);
        SDL_RenderClear(ren);

        // NES frame as the "background"
        SDL_Rect dst = letterboxDest(win, baseW, baseH, integerScale);
        SDL_RenderCopy(ren, tex, nullptr, &dst);

        // Overlay GUI
        timgui::RenderSDL();

        SDL_RenderPresent(ren);

        // ------------------ FPS calc ------------------
        Uint64 ticksNow = SDL_GetPerformanceCounter();
        double dt = (double)(ticksNow - ticksPrev) / (double)SDL_GetPerformanceFrequency();
        ticksPrev = ticksNow;
        // Simple EMA for stability
        double inst = (dt > 0.0) ? (1.0 / dt) : 0.0;
        fps = fps * 0.9 + inst * 0.1;
    }

    // ------------------ Shutdown ------------------
    timgui::DestroyContext();
    SDL_StopTextInput();

    if (nes.input && nes.input->controller) {
        SDL_GameControllerClose(nes.input->controller);
        nes.input->controller = nullptr;
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
