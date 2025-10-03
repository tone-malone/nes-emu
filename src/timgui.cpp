#include "timgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>

namespace timgui {
// -----------------------------
// Global context + menu state
// -----------------------------
static Context *g_ctx = nullptr;

struct MenuState {
    size_t activeMenu = 0;
    bool isOpen = false;

    std::unordered_map<size_t, float> itemY;
    std::unordered_map<size_t, float> originX;
    std::unordered_map<size_t, Rect> dropRect;
    std::unordered_map<size_t, bool> subOpen;
    std::unordered_map<size_t, size_t> parentMenu;
    std::vector<size_t> menuStack;                    // which menu we’re currently building into
    std::unordered_map<size_t, Rect> parentItemRect;  // rect of the submenu’s parent row
} g_menu;

// Close all open submenus
static inline void CloseAllSubMenus() {
    g_menu.subOpen.clear();
    g_menu.parentMenu.clear();
    g_menu.parentItemRect.clear();
    // keep dropRect/originX/itemY maps; they’ll be rebuilt when reopened
}

// True if submenu `sid` is under the current active top-level menu
static inline bool IsUnderActive(size_t sid) {
    size_t cur = sid;
    // climb up parents until we hit a top-level
    while (g_menu.parentMenu.count(cur)) cur = g_menu.parentMenu[cur];
    return (g_menu.activeMenu != 0 && cur == g_menu.activeMenu);
}

// -----------------------------
// Helpers
// -----------------------------
static inline bool HitTest(const Rect &r, float mx, float my) {
    return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
}

static size_t GenerateID(const char *label) {
    auto &ctx = GetContext();
    std::string key = ctx.currentWindowTitle;
    for (auto &sub : ctx.idStack) key += "/" + sub;
    key += "/";
    key += label;
    return std::hash<std::string>{}(key);
}

// UTF-8 caret helpers (codepoint step)
static int utf8_prev_cp_start(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}
static int utf8_next_cp_end(const char *s, int len, int i) {
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

// -----------------------------
// Context & lifecycle
// -----------------------------
void CreateContext() {
    if (g_ctx) return;
    g_ctx = new Context();

    auto &s = g_ctx->style;
    // default flat style
    s.windowBg = {0.18f, 0.18f, 0.18f, 1.0f};
    s.button = {0.22f, 0.22f, 0.25f, 1.0f};
    s.buttonHover = {0.28f, 0.28f, 0.32f, 1.0f};
    s.sliderTrack = {0.20f, 0.20f, 0.23f, 1.0f};
    s.sliderHandle = {0.35f, 0.75f, 0.95f, 1.0f};
    s.text = {0.92f, 0.92f, 0.92f, 1.0f};
    s.framePadding = 6.0f;
    s.itemSpacing = 4.0f;
    s.menuBarBg = {0.20f, 0.20f, 0.22f, 1.0f};
    s.menuItemBg = {0.22f, 0.22f, 0.25f, 1.0f};
    s.menuItemHoverBg = {0.30f, 0.55f, 0.85f, 1.0f};

    // menuBarHeight & menuItemHeight will be finalized after font loads in Init()
    s.menuBarHeight = 22.0f;
    s.menuItemHeight = 22.0f;

    g_ctx->baseStyle = g_ctx->style;
}

void DestroyContext() {
    if (!g_ctx) return;

    // free text cache textures
    for (auto &kv : g_ctx->textCache) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    g_ctx->textCache.clear();

    if (g_ctx->font) {
        TTF_CloseFont(g_ctx->font);
        g_ctx->font = nullptr;
    }
    if (TTF_WasInit()) TTF_Quit();
    SaveLayout("timgui_layout.txt");
    delete g_ctx;
    g_ctx = nullptr;
}

Context &GetContext() { return *g_ctx; }

bool Init(SDL_Renderer *ren, const char *fontPath, int fontSize) {
    if (!g_ctx) return false;
    if (TTF_WasInit() == 0) {
        if (TTF_Init() != 0) {
            SDL_Log("TTF_Init error: %s", TTF_GetError());
            return false;
        }
    }
    g_ctx->renderer = ren;
    g_ctx->font = TTF_OpenFont(fontPath, fontSize);
    if (!g_ctx->font) {
        SDL_Log("TTF_OpenFont error: %s", TTF_GetError());
        return false;
    }
    // compute a reliable font "height"
    g_ctx->fontSize = TTF_FontHeight(g_ctx->font);

    // finalize menu sizes using actual font height
    auto &s = g_ctx->style;
    s.menuBarHeight = s.framePadding * 2 + float(g_ctx->fontSize);
    s.menuItemHeight = s.framePadding * 2 + float(g_ctx->fontSize);

    LoadLayout("timgui_layout.txt");
    return true;
}

void HandleKeyboardNav() {
    auto &ctx = GetContext();
    auto &fo = ctx.focusOrder;
    if (fo.empty()) return;

    if (ctx.io.keyTab) {
        // find current focused index
        int idx = -1;
        for (int i = 0; i < (int)fo.size(); ++i)
            if (fo[i] == ctx.focusedItem) {
                idx = i;
                break;
            }
        int delta = ctx.io.keyShift ? -1 : 1;
        int next = (idx < 0) ? (delta > 0 ? 0 : (int)fo.size() - 1)
                             : ((idx + delta + (int)fo.size()) % (int)fo.size());
        ctx.prevFocusedItem = ctx.focusedItem;
        ctx.focusedItem = fo[next];
    }
}

void NewFrame() {
    auto &ctx = GetContext();
    ctx.commands.clear();
    ctx.overlayCommands.clear();
    ctx.tooltipCommands.clear();
    ctx.insideWindow = false;

    ctx.hotItem = 0;
    ctx.idStack.clear();

    // text input (edge-triggered)
    ctx.io.inputChars.clear();
    ctx.io.backspace = false;

    // nav keys (edge-triggered)
    ctx.io.keyLeft = ctx.io.keyRight = ctx.io.keyHome = ctx.io.keyEnd = false;
    ctx.io.keyUp = ctx.io.keyDown = ctx.io.keyPageUp = ctx.io.keyPageDown = false;
    ctx.io.keyEnter = false;
    ctx.io.keyCtrlV = false;
    ctx.io.keyTab = false;
    ctx.io.keyShift = false;
    ctx.io.keySpace = false;

    // tooltip + overlay
    ctx.tooltip.want = false;     // will be set by Tooltip()/TooltipOverlay this frame
    ctx.overlayHovering = false;  // menus will flip this true if hovered

    // next-item hints
    ctx.nextItem.clear();

    // per-frame focus order
    ctx.focusOrder.clear();
}

static void FinalizeTooltipsAndPushDraw() {
    auto &ctx = GetContext();
    auto &tp = ctx.tooltip;
    Uint32 now = SDL_GetTicks();
    float dt = (tp.lastTickMs == 0) ? 16.0f : float(now - tp.lastTickMs);
    tp.lastTickMs = now;

    // Target alpha: show after delay if someone requested it this frame
    float targetAlpha = 0.0f;
    if (tp.want) {
        Uint32 since = now - tp.lastChangeMs;
        if (since >= (Uint32)ctx.tooltipDelayMs)
            targetAlpha = 1.0f;
    }

    // Approach target alpha with simple linear fade
    float fadeStep = (ctx.tooltipFadeMs > 0.0f) ? (dt / ctx.tooltipFadeMs) : 1.0f;
    if (tp.alpha < targetAlpha)
        tp.alpha = std::min(1.0f, tp.alpha + fadeStep);
    else if (tp.alpha > targetAlpha)
        tp.alpha = std::max(0.0f, tp.alpha - fadeStep);

    // If visible, push draw commands (use tooltipCommands list)
    if (tp.alpha > 0.0f && !tp.text.empty()) {
        float pad = ctx.style.framePadding;
        int tw = 0, th = 0;
        if (ctx.font) TTF_SizeUTF8(ctx.font, tp.text.c_str(), &tw, &th);
        Rect bg{tp.x, tp.y, float(tw) + pad * 2.0f, float(th) + pad * 2.0f};

        // Modulate colors by alpha
        auto mod = [&](Color c) { c.a *= tp.alpha; return c; };

        ctx.tooltipCommands.push_back({CmdType::Rect, bg, "", mod(ctx.style.button)});
        Rect tr{tp.x + pad, tp.y + pad, float(tw), float(th)};
        ctx.tooltipCommands.push_back({CmdType::Text, tr, tp.text, mod(ctx.style.text)});
    }

    // Reset the per-frame "want" flag (alpha persists for fade-out)
    tp.want = false;
}

void EndFrame() {
    auto &ctx = GetContext();

    // keyboard focus traversal already happens here in your build; keep it
    HandleKeyboardNav();

    // finalize tooltips (delay + fade + draw)
    FinalizeTooltipsAndPushDraw();

    // end-of-frame resets
    ctx.io.mouseClicked = false;
    ctx.io.mouseReleased = false;
    ctx.io.mouseWheelY = 0.0f;
    ctx.lastMouseX = ctx.io.mouseX;
    ctx.lastMouseY = ctx.io.mouseY;
}

void HandleSDLEvent(const SDL_Event &e) {
    auto &io = GetContext().io;
    switch (e.type) {
        case SDL_MOUSEMOTION:
            io.mouseX = float(e.motion.x);
            io.mouseY = float(e.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                io.mouseDown = true;
                io.mouseClicked = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                io.mouseDown = false;
                io.mouseReleased = true;
            }
            break;
        case SDL_MOUSEWHEEL:
            io.mouseWheelY += (e.wheel.y > 0) ? 1.0f : (e.wheel.y < 0 ? -1.0f : 0.0f);
            break;
        case SDL_TEXTINPUT:
            io.inputChars += e.text.text;
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_BACKSPACE) io.backspace = true;
            if (e.key.keysym.sym == SDLK_LEFT) io.keyLeft = true;
            if (e.key.keysym.sym == SDLK_RIGHT) io.keyRight = true;
            if (e.key.keysym.sym == SDLK_UP) io.keyUp = true;
            if (e.key.keysym.sym == SDLK_DOWN) io.keyDown = true;
            if (e.key.keysym.sym == SDLK_PAGEUP) io.keyPageUp = true;
            if (e.key.keysym.sym == SDLK_PAGEDOWN) io.keyPageDown = true;
            if (e.key.keysym.sym == SDLK_HOME) io.keyHome = true;
            if (e.key.keysym.sym == SDLK_END) io.keyEnd = true;
            if (e.key.keysym.sym == SDLK_TAB) {
                io.keyTab = true;
                io.keyShift = (e.key.keysym.mod & KMOD_SHIFT) != 0;
            }
            if (e.key.keysym.sym == SDLK_SPACE) io.keySpace = true;
            if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) io.keyEnter = true;
            if ((e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym == SDLK_v) io.keyCtrlV = true;
            break;
        default:
            break;
    }
}

// -----------------------------
// Styling stack
// -----------------------------
void PushStyleColor(StyleColor which, Color col) {
    auto &ctx = GetContext();
    ctx.styleStack.push_back(ctx.style);
    switch (which) {
        case StyleColor::WindowBg:
            ctx.style.windowBg = col;
            break;
        case StyleColor::Button:
            ctx.style.button = col;
            break;
        case StyleColor::ButtonHover:
            ctx.style.buttonHover = col;
            break;
        case StyleColor::SliderTrack:
            ctx.style.sliderTrack = col;
            break;
        case StyleColor::SliderHandle:
            ctx.style.sliderHandle = col;
            break;
        case StyleColor::Text:
            ctx.style.text = col;
            break;
        case StyleColor::MenuBarBg:
            ctx.style.menuBarBg = col;
            break;
        case StyleColor::MenuItemBg:
            ctx.style.menuItemBg = col;
            break;
        case StyleColor::MenuItemHoverBg:
            ctx.style.menuItemHoverBg = col;
            break;
    }
}
void PopStyleColor() {
    auto &ctx = GetContext();
    if (ctx.styleStack.empty()) return;
    ctx.style = ctx.styleStack.back();
    ctx.styleStack.pop_back();
}
void PushStyleVar(float *var_ptr, float new_value) {
    auto &ctx = GetContext();
    ctx.styleStack.push_back(ctx.style);
    if (var_ptr == &ctx.style.framePadding)
        ctx.style.framePadding = new_value;
    else if (var_ptr == &ctx.style.itemSpacing)
        ctx.style.itemSpacing = new_value;
    else
        SDL_Log("Unknown style var: %p", var_ptr);
}
void PopStyleVar() {
    auto &ctx = GetContext();
    if (ctx.styleStack.empty()) return;
    auto &s = ctx.styleStack.back();
    ctx.style.framePadding = s.framePadding;
    ctx.style.itemSpacing = s.itemSpacing;
    ctx.styleStack.pop_back();
}

void ResetStyle() {
    auto &ctx = GetContext();
    ctx.style = ctx.baseStyle;
    ctx.styleStack.clear();
}

// -----------------------------
// ID stack
// -----------------------------
void PushID(const char *str_id) { GetContext().idStack.push_back(str_id); }
void PopID() {
    auto &s = GetContext().idStack;
    if (!s.empty()) s.pop_back();
}

// -----------------------------
// Rendering
// -----------------------------
void Render(const std::function<void(const DrawCmd &)> &cb) {
    auto &ctx = GetContext();
    for (auto &cmd : ctx.commands) cb(cmd);
    for (auto &cmd : ctx.overlayCommands) cb(cmd);
    for (auto &cmd : ctx.tooltipCommands) cb(cmd);
}

// clipping helpers
void PushClipRect(float x, float y, float w, float h) {
    GetContext().commands.push_back({CmdType::PushClip, {x, y, w, h}, "", {}});
}
void PopClipRect() {
    GetContext().commands.push_back({CmdType::PopClip, {}, "", {}});
}

void RenderSDL() {
    auto &ctx = GetContext();
    SDL_SetRenderDrawBlendMode(ctx.renderer, SDL_BLENDMODE_BLEND);

    // --- local clip stack, independent of ctx ---
    std::vector<SDL_Rect> clipStack;
    auto set_clip = [&]() {
        if (clipStack.empty())
            SDL_RenderSetClipRect(ctx.renderer, nullptr);
        else
            SDL_RenderSetClipRect(ctx.renderer, &clipStack.back());
    };

    // --- text cache ---
    auto get_text_tex = [&](const std::string &s, Color c) -> Context::TextCacheEntry {
        Uint32 rgba = (Uint32(c.r * 255) << 24) | (Uint32(c.g * 255) << 16) | (Uint32(c.b * 255) << 8) | Uint32(c.a * 255);
        Context::TextKey key{ctx.font, rgba, s};
        auto it = ctx.textCache.find(key);
        if (it != ctx.textCache.end()) {
            it->second.age = ++ctx.cacheAge;
            return it->second;
        }
        SDL_Color sc{Uint8(c.r * 255), Uint8(c.g * 255), Uint8(c.b * 255), Uint8(c.a * 255)};
        SDL_Surface *surf = TTF_RenderUTF8_Blended(ctx.font, s.c_str(), sc);
        if (!surf) return {};
        SDL_Texture *tex = SDL_CreateTextureFromSurface(ctx.renderer, surf);
        Context::TextCacheEntry e{tex, surf->w, surf->h, ++ctx.cacheAge};
        SDL_FreeSurface(surf);
        ctx.textCache.emplace(key, e);
        if ((int)ctx.textCache.size() > ctx.cacheBudget) {
            auto victim = std::min_element(ctx.textCache.begin(), ctx.textCache.end(),
                                           [](auto &a, auto &b) { return a.second.age < b.second.age; });
            if (victim != ctx.textCache.end()) {
                if (victim->second.tex) SDL_DestroyTexture(victim->second.tex);
                ctx.textCache.erase(victim);
            }
        }
        return e;
    };

    auto draw_stream = [&](const DrawList &list) {
        for (const DrawCmd &cmd : list) {
            switch (cmd.type) {
                case CmdType::PushClip: {
                    SDL_Rect r{int(cmd.rect.x), int(cmd.rect.y), int(cmd.rect.w), int(cmd.rect.h)};
                    clipStack.push_back(r);
                    set_clip();
                    break;
                }
                case CmdType::PopClip: {
                    if (!clipStack.empty()) clipStack.pop_back();
                    set_clip();
                    break;
                }
                case CmdType::Rect: {
                    set_clip();
                    SDL_SetRenderDrawColor(ctx.renderer,
                                           Uint8(cmd.color.r * 255),
                                           Uint8(cmd.color.g * 255),
                                           Uint8(cmd.color.b * 255),
                                           Uint8(cmd.color.a * 255));
                    SDL_Rect r{int(cmd.rect.x), int(cmd.rect.y), int(cmd.rect.w), int(cmd.rect.h)};
                    SDL_RenderFillRect(ctx.renderer, &r);
                    break;
                }
                case CmdType::Text: {
                    if (cmd.text.empty()) break;
                    set_clip();
                    auto e = get_text_tex(cmd.text, cmd.color);
                    if (e.tex) {
                        SDL_Rect dst{int(cmd.rect.x), int(cmd.rect.y), e.w, e.h};
                        SDL_RenderCopy(ctx.renderer, e.tex, nullptr, &dst);
                    }
                    break;
                }
            }
        }
    };

    // 1) normal widgets (window/child content)
    draw_stream(ctx.commands);

    // 2) menu drop-down solid backdrops (not clipped)
    clipStack.clear();
    set_clip();
    if (g_menu.isOpen) {
        auto it = g_menu.dropRect.find(g_menu.activeMenu);
        if (it != g_menu.dropRect.end()) {
            Rect &dr = it->second;
            if (dr.w > 0 && dr.h > 0) {
                SDL_SetRenderDrawColor(ctx.renderer,
                                       Uint8(ctx.style.menuItemBg.r * 255),
                                       Uint8(ctx.style.menuItemBg.g * 255),
                                       Uint8(ctx.style.menuItemBg.b * 255),
                                       255);
                SDL_Rect r{int(dr.x), int(dr.y), int(dr.w), int(dr.h)};
                SDL_RenderFillRect(ctx.renderer, &r);
            }
        }
        for (auto &p : g_menu.subOpen) {
            if (!p.second) continue;
            size_t sid = p.first;
            if (!IsUnderActive(sid)) continue;  // draw only submenus under the current top-level

            Rect &dr = g_menu.dropRect[sid];
            if (dr.w > 0 && dr.h > 0) {
                SDL_SetRenderDrawColor(ctx.renderer,
                                       Uint8(ctx.style.menuItemBg.r * 255),
                                       Uint8(ctx.style.menuItemBg.g * 255),
                                       Uint8(ctx.style.menuItemBg.b * 255),
                                       255);
                SDL_Rect r{int(dr.x), int(dr.y), int(dr.w), int(dr.h)};
                SDL_RenderFillRect(ctx.renderer, &r);
            }
        }
    }

    // 3) overlay items (menus etc.)
    clipStack.clear();
    set_clip();
    draw_stream(ctx.overlayCommands);

    // 4) tooltips last (no clipping)
    clipStack.clear();
    set_clip();
    draw_stream(ctx.tooltipCommands);

    // reset any clip at the very end
    SDL_RenderSetClipRect(ctx.renderer, nullptr);
}

// -----------------------------
// Windowing
// -----------------------------
bool Begin(const char *title, bool *p_open, float x, float y, float w, float h) {
    auto &ctx = GetContext();
    if (p_open && !*p_open) return false;

    size_t winID = GenerateID(title);
    size_t closeID = GenerateID((std::string(title) + "#CLOSE").c_str());
    size_t gripID = GenerateID((std::string(title) + "#RESIZE").c_str());

    if (!ctx.windowPositions.count(title)) {
        ctx.windowPositions[title] = {x, y, w, h};
    }
    Rect &wp = ctx.windowPositions[title];

    Rect titleBar{wp.x, wp.y, wp.w, 20.0f};

    bool overBar = HitTest(titleBar, ctx.io.mouseX, ctx.io.mouseY);
    if (overBar && ctx.io.mouseClicked && ctx.activeItem == 0) {
        ctx.activeItem = winID;
    }
    if (ctx.activeItem == winID && ctx.io.mouseDown) {
        wp.x += ctx.io.mouseX - ctx.lastMouseX;
        wp.y += ctx.io.mouseY - ctx.lastMouseY;
    }
    if (ctx.activeItem == winID && ctx.io.mouseReleased) {
        ctx.activeItem = 0;
    }

    Rect grip{wp.x + wp.w - 16.0f, wp.y + wp.h - 16.0f, 16.0f, 16.0f};
    bool overGrip = HitTest(grip, ctx.io.mouseX, ctx.io.mouseY);
    if (overGrip && ctx.io.mouseClicked && ctx.resizeItem == 0) {
        ctx.resizeItem = gripID;
    }
    if (ctx.resizeItem == gripID && ctx.io.mouseDown) {
        float dx = ctx.io.mouseX - ctx.lastMouseX;
        float dy = ctx.io.mouseY - ctx.lastMouseY;
        wp.w = std::max(50.0f, wp.w + dx);
        wp.h = std::max(50.0f, wp.h + dy);
    }
    if (ctx.resizeItem == gripID && ctx.io.mouseReleased) {
        ctx.resizeItem = 0;
    }

    ctx.currentWindowTitle = title;
    ctx.currentWindowRect = wp;
    ctx.insideWindow = true;

    auto &L = ctx.layouts[title];
    L.cursorX = wp.x + ctx.style.framePadding;
    L.cursorY = wp.y + 20.0f;
    L.lastW = L.lastH = 0.0f;
    L.sameLine = false;
    L.rowMaxH = 0.0f;

    // shadow
    Rect shadow = {wp.x + 4, wp.y + 4, wp.w, wp.h};
    ctx.commands.push_back({CmdType::Rect, shadow, "", {0, 0, 0, 0.25f}});

    // window + title bar
    ctx.commands.push_back({CmdType::Rect, wp, "", ctx.style.windowBg});
    ctx.commands.push_back({CmdType::Rect, titleBar, "", ctx.style.button});

    // close button
    float closeSize = 16.0f;
    Rect closeR{
        wp.x + wp.w - closeSize - ctx.style.framePadding * 0.5f,
        wp.y + (20.0f - closeSize) * 0.5f,
        closeSize, closeSize};
    bool overX = HitTest(closeR, ctx.io.mouseX, ctx.io.mouseY);
    Color col = overX ? ctx.style.buttonHover : ctx.style.button;
    ctx.commands.push_back({CmdType::Rect, closeR, "", col});
    ctx.commands.push_back({CmdType::Text,
                            {closeR.x + 2, closeR.y - 1, closeSize, closeSize},
                            "×",
                            ctx.style.text});
    if (overX && ctx.io.mouseReleased && p_open) {
        *p_open = false;
        ctx.activeItem = 0;
        return false;
    }

    // resize grip
    ctx.commands.push_back({CmdType::Rect,
                            grip,
                            "",
                            {ctx.style.button.r * 0.5f, ctx.style.button.g * 0.5f, ctx.style.button.b * 0.5f, 1.0f}});

    // clip all window contents to the window rectangle (excluding overlays/menus)
    GetContext().commands.push_back({CmdType::PushClip, ctx.currentWindowRect, "", {}});

    return true;
}

void End() {
    auto &ctx = GetContext();
    // pop the window clip we pushed in Begin()
    ctx.commands.push_back({CmdType::PopClip, {}, "", {}});
    ctx.insideWindow = false;
}

bool BeginChild(const char *id, float w, float h, bool border) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;

    float totalW = ctx.currentWindowRect.w - pad * 2.0f;
    float x = L.sameLine ? L.cursorX : startX;
    float cw = (w > 0.0f) ? std::min(w, totalW)
                          : (L.columns > 1 ? (totalW - spacing * (L.columns - 1)) / std::max(1, L.columns)
                                           : totalW);
    float cy = L.cursorY;
    Rect childR{x, cy, cw, h};

    // bg + optional border
    Color bg{ctx.style.windowBg.r * 0.90f, ctx.style.windowBg.g * 0.90f, ctx.style.windowBg.b * 0.90f, 1.0f};
    ctx.commands.push_back({CmdType::Rect, childR, "", bg});
    if (border) {
        Rect br{childR.x - 1, childR.y - 1, childR.w + 2, childR.h + 2};
        ctx.commands.push_back({CmdType::Rect, br, "", {0, 0, 0, 0.35f}});
    }

    // scroll id + input
    size_t cid = GenerateID((std::string(id) + "##child").c_str());
    float &scroll = ctx.childScrollY[cid];

    bool hovered = HitTest(childR, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) {
        // mouse wheel
        if (ctx.io.mouseWheelY != 0.0f)
            scroll -= ctx.io.mouseWheelY * 30.0f;

        // keyboard scroll
        float lineStep = float(ctx.fontSize) + 4.0f;
        if (ctx.io.keyUp) scroll -= lineStep;
        if (ctx.io.keyDown) scroll += lineStep;
        if (ctx.io.keyPageUp) scroll -= (childR.h - 2.0f * pad);
        if (ctx.io.keyPageDown) scroll += (childR.h - 2.0f * pad);
    }
    if (scroll < 0.0f) scroll = 0.0f;  // temporary clamp; final clamp in EndChild

    // record frame to compute content height at EndChild
    ctx.childStack.push_back({cid, childR, L.cursorX, L.cursorY, L.cursorY});

    // queue clip for this child
    PushClipRect(childR.x, childR.y, childR.w, childR.h);

    // child-local layout start (scroll applied)
    L.cursorX = childR.x + pad;
    L.cursorY = childR.y + pad - scroll;
    L.lastW = L.lastH = 0.0f;
    L.sameLine = false;
    return true;
}

void EndChild() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    if (ctx.childStack.empty()) {
        PopClipRect();
        return;
    }

    auto fr = ctx.childStack.back();
    ctx.childStack.pop_back();

    float pad = ctx.style.framePadding;
    float &scroll = ctx.childScrollY[fr.id];

    // compute used height in logical space (without clip)
    float usedHeight = (L.cursorY - (fr.rect.y + pad - scroll));
    float visibleH = std::max(0.0f, fr.rect.h - 2.0f * pad);
    float maxScroll = std::max(0.0f, usedHeight - visibleH);
    if (scroll > maxScroll) scroll = maxScroll;

    // pop the child clip
    PopClipRect();

    // advance parent layout below the child
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;
    L.cursorY = fr.rect.y + fr.rect.h + spacing;
    L.cursorX = startX;
    L.lastW = fr.rect.w;
    L.lastH = fr.rect.h;
    L.rowMaxH = std::max(L.rowMaxH, fr.rect.h);
    L.sameLine = false;
}

// -----------------------------
// Layout
// -----------------------------
void SameLine(float spacing) {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    if (spacing < 0.0f) spacing = ctx.style.itemSpacing;

    if (!L.sameLine) {
        L.cursorX = ctx.currentWindowRect.x + ctx.style.framePadding + L.lastW + spacing;
    } else {
        L.cursorX += L.lastW + spacing;
    }
    L.sameLine = true;
}

void SameLineItemCount(int count, float spacing) {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    if (spacing < 0.0f) spacing = ctx.style.itemSpacing;
    L.sameLineCount = std::max(1, count);
    L.sameLineIndex = 0;
    L.sameLineSpacing = spacing;
    L.sameLine = true;
}

void SetNextItemWidth(float w) {
    auto &ctx = GetContext();
    ctx.nextItem.hasWidth = true;
    ctx.nextItem.width = w;
}
void SetNextItemXOffset(float xoff) {
    auto &ctx = GetContext();
    ctx.nextItem.hasXOffset = true;
    ctx.nextItem.xoff = xoff;
}

void NewLine() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    L.cursorY += L.lastH + ctx.style.itemSpacing;
    L.cursorX = ctx.currentWindowRect.x + ctx.style.framePadding;
    L.sameLine = false;
    L.lastW = L.lastH = 0.0f;
}

void Separator() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    Rect r{
        ctx.currentWindowRect.x + ctx.style.framePadding,
        L.cursorY + 5.0f,
        ctx.currentWindowRect.w - 20.0f,
        1.0f};
    ctx.commands.push_back({CmdType::Rect, r, "", ctx.style.button});
    L.cursorY += 10.0f;
    L.lastW = r.w;
    L.lastH = 1.0f;
    L.rowMaxH = std::max(L.rowMaxH, L.lastH);
}

void CalcTextSize(const char *text, int &out_w, int &out_h) {
    auto &ctx = GetContext();
    if (ctx.font)
        TTF_SizeUTF8(ctx.font, text, &out_w, &out_h);
    else
        out_w = out_h = 0;
}

void Columns(int count) {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    L.columns = std::max(1, count);
    L.columnIndex = 0;
    L.sameLine = false;
    L.rowMaxH = 0.0f;
}

void NextColumn() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    if (L.columns > 1) {
        L.columnIndex = (L.columnIndex + 1) % L.columns;
        float pad = ctx.style.framePadding;
        float spacing = ctx.style.itemSpacing;
        float totalW = ctx.currentWindowRect.w - pad * 2.0f;
        float colW = (totalW - spacing * (L.columns - 1)) / L.columns;
        L.cursorX = ctx.currentWindowRect.x + pad + L.columnIndex * (colW + spacing);
        if (L.columnIndex == 0) {
            // wrapped to next row
            L.cursorY += L.rowMaxH + spacing;
            L.rowMaxH = 0.0f;
        }
    }
}

void EndColumns() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    if (L.columns > 1 && L.columnIndex != 0) {
        L.cursorY += L.rowMaxH + ctx.style.itemSpacing;
    }
    L.cursorX = ctx.currentWindowRect.x + ctx.style.framePadding;
    L.columns = 0;
    L.columnIndex = 0;
    L.rowMaxH = 0.0f;
    L.sameLine = false;
}

// -----------------------------
// Widgets
// -----------------------------
void Text(const char *txt) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    int tw = 0, th = 0;
    TTF_SizeUTF8(ctx.font, txt, &tw, &th);

    const float pad = ctx.style.framePadding;
    const float startX = ctx.currentWindowRect.x + pad;
    const float totalW = ctx.currentWindowRect.w - pad * 2.0f;

    float availW = L.sameLine ? (startX + totalW - L.cursorX) : totalW;

    // If text is wider than the available space, fall back to wrapping behavior.
    if (float(tw) > availW + 0.5f) {
        TextWrapped(txt, availW);
        return;
    }

    // No wrap needed: draw normally
    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;
    Rect r{x, y, float(tw), float(th)};
    ctx.commands.push_back({CmdType::Text, r, txt, ctx.style.text});

    if (L.sameLine) {
        L.cursorX = x + r.w + ctx.style.itemSpacing;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    } else {
        L.cursorY += r.h + ctx.style.itemSpacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    }
    L.lastW = r.w;
    L.lastH = r.h;
    if (L.sameLineCount == 0) L.sameLine = false;
}

void TextF(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Text(buf);
}

// Greedy UTF-8 word-wrapper using TTF_SizeUTF8
static void WrapTextUTF8(TTF_Font *font, const char *text, float maxWidth, std::vector<std::string> &outLines) {
    outLines.clear();
    if (!text || !*text) return;

    const char *s = text;
    const char *lineStart = s;
    const char *lastSpace = nullptr;

    int w = 0, h = 0;

    auto flushLine = [&](const char *a, const char *b) {
        if (b <= a) return;
        outLines.emplace_back(a, b);
    };

    while (*s) {
        // newline hard break
        if (*s == '\n') {
            flushLine(lineStart, s);
            ++s;
            lineStart = s;
            lastSpace = nullptr;
            continue;
        }
        if (*s == ' ') lastSpace = s;

        // try measure current candidate [lineStart, s]
        std::string candidate(lineStart, s + 1);
        if (TTF_SizeUTF8(font, candidate.c_str(), &w, &h) != 0) {
            w = 0;
        }

        if (w > maxWidth && s > lineStart) {
            if (lastSpace && lastSpace >= lineStart) {
                flushLine(lineStart, lastSpace);  // break at last space
                s = lastSpace + 1;                // continue after space
            } else {
                flushLine(lineStart, s);  // hard break mid-word
            }
            lineStart = s;
            lastSpace = nullptr;
        } else {
            ++s;
        }
    }
    // remaining tail
    if (s > lineStart) flushLine(lineStart, s);
}

void TextWrapped(const char *txt, float wrap_width) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;

    // available width: either user width, column width, or remaining row width
    float totalW = ctx.currentWindowRect.w - pad * 2.0f;
    float availW;
    if (wrap_width > 0.0f) {
        availW = wrap_width;
    } else if (L.columns > 1) {
        float colW = (totalW - ctx.style.itemSpacing * (L.columns - 1)) / std::max(1, L.columns);
        availW = colW;
    } else if (L.sameLine) {
        // remaining space on this line
        availW = (startX + totalW) - L.cursorX;
    } else {
        availW = totalW;
    }

    std::vector<std::string> lines;
    WrapTextUTF8(ctx.font, txt, std::max(1.0f, availW), lines);

    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;

    int tw = 0, th = 0;
    float lineH = float(ctx.fontSize);
    for (auto &ln : lines) {
        if (TTF_SizeUTF8(ctx.font, ln.c_str(), &tw, &th) != 0) {
            tw = 0;
            th = ctx.fontSize;
        }
        ctx.commands.push_back({CmdType::Text, {x, y, float(tw), float(th)}, ln, ctx.style.text});
        y += th;  // stack lines vertically without extra spacing between them
    }

    // advance layout (as a block)
    float usedH = y - L.cursorY;
    if (L.sameLine) {
        L.cursorX = x + availW + spacing;  // move to end of wrapped block
        L.rowMaxH = std::max(L.rowMaxH, usedH);
    } else {
        L.cursorY += usedH + spacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, usedH);
    }

    L.lastW = availW;
    L.lastH = usedH;
    if (L.sameLineCount == 0) L.sameLine = false;
}

bool Button(const char *label) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    float pad = ctx.style.framePadding;
    float spacing = ctx.style.itemSpacing;
    float totalW = ctx.currentWindowRect.w - pad * 2.0f;
    float h = 30.0f;

    // width/x computation
    float w, x;
    if (L.sameLineCount > 0) {
        int N = L.sameLineCount;
        float sp = L.sameLineSpacing;
        w = (totalW - sp * (N - 1)) / N;
        x = ctx.currentWindowRect.x + pad + L.sameLineIndex * (w + sp);
    } else if (ctx.nextItem.hasWidth) {
        w = std::min(ctx.nextItem.width, totalW);
        x = ctx.currentWindowRect.x + pad + (ctx.nextItem.hasXOffset ? ctx.nextItem.xoff : 0.0f);
    } else if (L.columns > 1) {
        w = (totalW - spacing * (L.columns - 1)) / L.columns;
        x = ctx.currentWindowRect.x + pad + L.columnIndex * (w + spacing);
    } else {
        w = L.sameLine ? (totalW - (L.cursorX - (ctx.currentWindowRect.x + pad))) : totalW;
        x = L.sameLine ? L.cursorX : (ctx.currentWindowRect.x + pad);
    }
    ctx.nextItem.clear();

    float y = L.cursorY;
    Rect r{x, y, w, h};

    size_t id = GenerateID(label);
    // register focusable
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);
    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    Color bg = (ctx.activeItem == id)
                   ? ctx.style.buttonHover
                   : (hovered ? ctx.style.buttonHover : ctx.style.button);
    ctx.commands.push_back({CmdType::Rect, r, "", bg});
    // subtle outline
    Color outline{std::min(bg.r + 0.05f, 1.f), std::min(bg.g + 0.05f, 1.f), std::min(bg.b + 0.05f, 1.f), 1.0f};
    Rect br{r.x - 1, r.y - 1, r.w + 2, r.h + 2};
    ctx.commands.push_back({CmdType::Rect, br, "", outline});

    int tw = 0, th = 0;
    TTF_SizeUTF8(ctx.font, label, &tw, &th);
    float tx = r.x + (r.w - tw) * 0.5f;
    float ty = r.y + (r.h - th) * 0.5f;
    ctx.commands.push_back({CmdType::Text, {tx, ty, float(tw), float(th)}, label, ctx.style.text});

    bool clicked = false;
    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        clicked = hovered;
        ctx.activeItem = 0;
    }
    // keyboard activate when focused
    if (!clicked && ctx.focusedItem == id && (ctx.io.keyEnter || ctx.io.keySpace)) {
        clicked = true;
    }

    if (L.columns > 1) {
        L.rowMaxH = std::max(L.rowMaxH, h);
        NextColumn();
    } else if (L.sameLineCount > 0) {
        L.rowMaxH = std::max(L.rowMaxH, h);
        L.sameLineIndex++;
        if (L.sameLineIndex >= L.sameLineCount) {
            L.sameLineCount = 0;
            L.sameLineIndex = 0;
            L.cursorY += L.rowMaxH + spacing;
            L.cursorX = ctx.currentWindowRect.x + pad;
            L.rowMaxH = 0.0f;
            L.sameLine = false;
        } else {
            // keep on same row
            L.sameLine = true;
        }
    } else if (L.sameLine) {
        L.rowMaxH = std::max(L.rowMaxH, h);
        L.cursorX = x + w + spacing;
        // keep sameLine true for caller control
    } else {
        L.cursorY += h + spacing;
        L.cursorX = ctx.currentWindowRect.x + pad;
        L.rowMaxH = std::max(L.rowMaxH, h);
    }

    L.lastW = w;
    L.lastH = h;
    return clicked;
}

bool Checkbox(const char *label, bool *v) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    float pad = ctx.style.framePadding;
    float spacing = ctx.style.itemSpacing;
    float boxSize = 20.0f;

    float startX = ctx.currentWindowRect.x + pad;
    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;

    Rect r{x, y, boxSize, boxSize};

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    Color bg = (*v) ? ctx.style.buttonHover : ctx.style.button;
    ctx.commands.push_back({CmdType::Rect, r, "", bg});

    int tw = 0, th = 0;
    TTF_SizeUTF8(ctx.font, label, &tw, &th);
    Rect lr{x + boxSize + 5.0f, y, float(tw), float(th)};
    ctx.commands.push_back({CmdType::Text, lr, label, ctx.style.text});

    float widgetH = std::max(boxSize, float(th));
    if (L.sameLine) {
        L.cursorX = lr.x + lr.w + spacing;
        L.rowMaxH = std::max(L.rowMaxH, widgetH);
    } else {
        L.cursorY += widgetH + spacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, widgetH);
    }
    L.lastW = boxSize + 5.0f + lr.w;
    L.lastH = widgetH;
    if (L.sameLineCount == 0) L.sameLine = false;

    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        *v = !(*v);
        ctx.activeItem = 0;
        ctx.focusedItem = id;
        return true;
    }
    // keyboard toggle when focused
    if (ctx.focusedItem == id && (ctx.io.keyEnter || ctx.io.keySpace)) {
        *v = !(*v);
        ctx.focusedItem = id;
        return true;
    }

    return false;
}

bool SliderFloat(const char *label, float *v, float v_min, float v_max) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    float pad = ctx.style.framePadding;
    float spacing = ctx.style.itemSpacing;
    float totalW = ctx.currentWindowRect.w - pad * 2.0f;

    float startX = ctx.currentWindowRect.x + pad;
    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;

    // label above
    int lw = 0, lh = 0;
    TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    Rect lr{x, y, float(lw), float(lh)};
    ctx.commands.push_back({CmdType::Text, lr, label, ctx.style.text});

    y += lh + 5.0f;

    float trackW =
        (ctx.nextItem.hasWidth ? std::min(ctx.nextItem.width, totalW)
                               : (L.columns > 1 ? (totalW - spacing * (L.columns - 1)) / L.columns
                                                : totalW));
    if (ctx.nextItem.hasXOffset) x = startX + ctx.nextItem.xoff;
    ctx.nextItem.clear();

    const float trackH = 20.0f;
    const float handleW = 12.0f;
    Rect track{x, y, trackW, trackH};

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);
    bool hovered = HitTest(track, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    if (ctx.activeItem == id && ctx.io.mouseDown) {
        float rel = (ctx.io.mouseX - (track.x + handleW * 0.5f)) / (track.w - handleW);
        rel = std::clamp(rel, 0.0f, 1.0f);
        *v = v_min + rel * (v_max - v_min);
    }
    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        ctx.activeItem = 0;
        ctx.focusedItem = id;
    }

    ctx.commands.push_back({CmdType::Rect, track, "", ctx.style.sliderTrack});
    float t = (*v - v_min) / (v_max - v_min);
    Rect handle{track.x + t * (track.w - handleW), track.y, handleW, track.h};
    ctx.commands.push_back({CmdType::Rect, handle, "", ctx.style.sliderHandle});

    // keyboard nudge when focused
    if (ctx.focusedItem == id) {
        float step = (v_max - v_min) / 100.0f;
        if (ctx.io.keyLeft) *v = std::clamp((*v) - step, v_min, v_max);
        if (ctx.io.keyRight) *v = std::clamp((*v) + step, v_min, v_max);
    }

    float usedH = trackH + lh + 5.0f;
    if (L.columns > 1) {
        L.rowMaxH = std::max(L.rowMaxH, usedH);
        NextColumn();
    } else if (L.sameLineCount > 0) {
        L.rowMaxH = std::max(L.rowMaxH, usedH);
        L.sameLineIndex++;
        if (L.sameLineIndex >= L.sameLineCount) {
            L.sameLineCount = 0;
            L.sameLineIndex = 0;
            L.cursorY += L.rowMaxH + spacing;
            L.cursorX = ctx.currentWindowRect.x + pad;
            L.rowMaxH = 0.0f;
            L.sameLine = false;
        } else {
            L.sameLine = true;
        }
    } else if (L.sameLine) {
        L.rowMaxH = std::max(L.rowMaxH, usedH);
        L.cursorX = x + trackW + spacing;
    } else {
        L.cursorY += usedH + spacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, usedH);
    }
    L.lastW = trackW;
    L.lastH = usedH;

    return true;  // value may or may not have changed; caller can compare if needed
}

void ProgressBar(float fraction, float width) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    float pad = ctx.style.framePadding;
    float spacing = ctx.style.itemSpacing;
    float startX = ctx.currentWindowRect.x + pad;
    float y = L.cursorY;

    float barW = (width > 0.0f) ? width : (ctx.currentWindowRect.w - pad * 2.0f);
    float barH = 10.0f;

    Rect barR{startX, y, barW, barH};
    Rect fillR{startX, y, barW * std::clamp(fraction, 0.0f, 1.0f), barH};

    ctx.commands.push_back({CmdType::Rect, barR, "", ctx.style.sliderTrack});
    ctx.commands.push_back({CmdType::Rect, fillR, "", ctx.style.button});

    L.cursorY += barH + spacing;
    L.lastW = barW;
    L.lastH = barH;
    L.rowMaxH = std::max(L.rowMaxH, barH);
    if (L.sameLineCount == 0) L.sameLine = false;
}

bool InputInt(const char *label, int *v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", *v);
    if (!InputText(label, buf, sizeof(buf))) return false;
    int tmp = std::atoi(buf);
    if (tmp != *v) {
        *v = tmp;
        return true;
    }
    return false;
}

bool InputFloat(const char *label, float *v, float v_min, float v_max, const char *format) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, *v);
    if (!InputText(label, buf, sizeof(buf))) return false;
    float tmp = std::strtof(buf, nullptr);
    tmp = std::clamp(tmp, v_min, v_max);
    if (tmp != *v) {
        *v = tmp;
        return true;
    }
    return false;
}


// -----------------------------
// New Widgets
// -----------------------------

bool RadioButton(const char *label, int *v, int v_value) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;

    // measure label
    int lw=0, lh=0; TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    float box = 18.0f;

    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;
    Rect r{x, y, box + 6.0f + float(lw), std::max(box, float(lh))};

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    // indicator (reuse button colors)
    Rect ind{r.x, r.y + (r.h - box) * 0.5f, box, box};
    Color bg = (*v == v_value) ? ctx.style.buttonHover : ctx.style.button;
    if (hovered) bg = (*v == v_value) ? ctx.style.buttonHover : ctx.style.buttonHover;
    ctx.commands.push_back({CmdType::Rect, ind, "", bg});

    // inner "dot"
    if (*v == v_value) {
        Rect dot{ind.x + 4, ind.y + 4, ind.w - 8, ind.h - 8};
        ctx.commands.push_back({CmdType::Rect, dot, "", ctx.style.sliderHandle});
    }

    // label
    ctx.commands.push_back({CmdType::Text,
        {ind.x + ind.w + 6.0f, r.y + (r.h - lh) * 0.5f, float(lw), float(lh)}, label, ctx.style.text});

    bool changed = false;
    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        if (hovered) { *v = v_value; changed = true; }
        ctx.activeItem = 0;
        ctx.focusedItem = id;
    }
    if (ctx.focusedItem == id && (ctx.io.keyEnter || ctx.io.keySpace)) {
        *v = v_value; changed = true;
    }

    // layout advance
    if (L.sameLine) {
        L.cursorX = r.x + r.w + spacing;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    } else {
        L.cursorY += r.h + spacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    }
    L.lastW = r.w; L.lastH = r.h;
    if (L.sameLineCount == 0) L.sameLine = false;

    return changed;
}

bool Selectable(const char *label, bool *selected, bool full_width) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;
    const float totalW = ctx.currentWindowRect.w - pad * 2.0f;

    int tw=0, th=0; TTF_SizeUTF8(ctx.font, label, &tw, &th);
    float h = std::max(float(th) + pad, ctx.style.menuItemHeight);
    float w = full_width ? (L.sameLine ? (startX + totalW - L.cursorX) : totalW)
                         : float(tw) + pad * 2.0f;

    float x = L.sameLine ? L.cursorX : startX;
    float y = L.cursorY;
    Rect r{x, y, w, h};

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    // bg
    bool sel = selected ? *selected : false;
    Color bg = hovered ? ctx.style.menuItemHoverBg : (sel ? ctx.style.menuItemBg : Color{0,0,0,0});
    if (bg.a > 0.0f) ctx.commands.push_back({CmdType::Rect, r, "", bg});

    // text
    ctx.commands.push_back({CmdType::Text,
        {r.x + pad, r.y + (r.h - th) * 0.5f, float(tw), float(th)}, label, ctx.style.text});

    bool clicked = false;
    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        clicked = hovered;
        ctx.activeItem = 0;
        ctx.focusedItem = id;
    }
    if (!clicked && ctx.focusedItem == id && (ctx.io.keyEnter || ctx.io.keySpace)) {
        clicked = true;
    }
    if (clicked && selected) *selected = !*selected;

    // layout
    if (L.sameLine) {
        L.cursorX = r.x + r.w + spacing;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    } else {
        L.cursorY += r.h + spacing;
        L.cursorX = startX;
        L.rowMaxH = std::max(L.rowMaxH, r.h);
    }
    L.lastW = r.w; L.lastH = r.h;
    if (L.sameLineCount == 0) L.sameLine = false;

    return clicked;
}

bool ListBox(const char *label, int *current_index, const std::vector<std::string> &items, int height_in_items) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;
    const float totalW = ctx.currentWindowRect.w - pad * 2.0f;
    const float itemH = ctx.style.menuItemHeight;

    // label
    int lw=0, lh=0; TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    ctx.commands.push_back({CmdType::Text, {L.cursorX, L.cursorY, float(lw), float(lh)}, label, ctx.style.text});
    L.cursorY += lh + 4.0f;

    // frame
    float w = (L.columns > 1) ? ( (totalW - ctx.style.itemSpacing * (L.columns - 1)) / L.columns )
                              : totalW;
    float h = std::max(1, height_in_items) * itemH + 2.0f; // small top/bot gap
    Rect frame{startX, L.cursorY, w, h};
    ctx.commands.push_back({CmdType::Rect, frame, "", {0.12f,0.12f,0.12f,1.0f}});
    ctx.commands.push_back({CmdType::Rect, {frame.x-1, frame.y-1, frame.w+2, frame.h+2}, "", {0,0,0,0.35f}});

    // id & scroll
    size_t id = GenerateID(label);
    float &scroll = ctx.listScrollY[id];

    // interactions
    bool hovered = HitTest(frame, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered && ctx.io.mouseWheelY != 0.0f)
        scroll -= ctx.io.mouseWheelY * (itemH * 2.0f);
    scroll = std::max(0.0f, scroll);

    // keyboard navigation when hovered/focused
    if (hovered || ctx.focusedItem == id) {
        if (ctx.io.keyUp)    *current_index = std::max(0, *current_index - 1);
        if (ctx.io.keyDown)  *current_index = std::min(int(items.size()) - 1, *current_index + 1);
        // keep selection visible
        float selY = (*current_index) * itemH;
        if (selY < scroll) scroll = selY;
        if (selY + itemH > scroll + (frame.h - 2)) scroll = selY + itemH - (frame.h - 2);
    }

    // clip + draw items
    PushClipRect(frame.x, frame.y, frame.w, frame.h);
    float y = frame.y - fmod(scroll, itemH);
    int first = int(scroll / itemH);
    int visible = int(std::ceil(frame.h / itemH)) + 2;

    bool changed = false;
    for (int i = 0; i < visible; ++i) {
        int idx = first + i;
        if (idx < 0 || idx >= (int)items.size()) continue;
        Rect row{frame.x, y + i * itemH, frame.w, itemH};
        bool rowHover = HitTest(row, ctx.io.mouseX, ctx.io.mouseY);
        if (rowHover) ctx.hotItem = id;

        Color bg = (idx == *current_index) ? ctx.style.menuItemBg
                                           : (rowHover ? ctx.style.menuItemHoverBg : Color{0,0,0,0});
        if (bg.a > 0.0f) ctx.commands.push_back({CmdType::Rect, row, "", bg});

        int tw=0, th=0; TTF_SizeUTF8(ctx.font, items[idx].c_str(), &tw, &th);
        ctx.commands.push_back({CmdType::Text,
            {row.x + pad, row.y + (row.h - th) * 0.5f, float(tw), float(th)},
            items[idx], ctx.style.text});

        if (rowHover && ctx.io.mouseReleased) {
            *current_index = idx;
            changed = true;
        }
    }
    PopClipRect();

    // layout advance
    L.cursorY += h + spacing;
    L.lastW = w; L.lastH = h; L.rowMaxH = std::max(L.rowMaxH, h);
    if (L.sameLineCount == 0) L.sameLine = false;

    // register for focus (TAB)
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    return changed;
}

bool DragFloat(const char *label, float *v, float speed, float v_min, float v_max, const char *format) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;
    const float totalW = ctx.currentWindowRect.w - pad * 2.0f;

    // label (left)
    int lw=0, lh=0; TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    Rect lr{L.cursorX, L.cursorY, float(lw), float(lh)};
    ctx.commands.push_back({CmdType::Text, lr, label, ctx.style.text});

    // value box
    float w = (L.columns > 1) ? ((totalW - ctx.style.itemSpacing * (L.columns - 1)) / L.columns) : totalW;
    float boxW = std::min(w, 140.0f);
    float x = startX + lw + spacing;
    float h = float(ctx.fontSize) + pad;
    Rect r{x, L.cursorY, boxW, h};

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) ctx.activeItem = id;

    // bg
    Color bg = (ctx.activeItem == id) ? Color{0.25f,0.25f,0.25f,1} : (hovered ? Color{0.20f,0.20f,0.20f,1} : Color{0.15f,0.15f,0.15f,1});
    ctx.commands.push_back({CmdType::Rect, r, "", bg});
    ctx.commands.push_back({CmdType::Rect, {r.x-1,r.y-1,r.w+2,r.h+2}, "", {0,0,0,0.35f}});

    // drag logic
    static float dragAnchor = 0.0f;
    static float startValue = 0.0f;
    bool changed = false;

    if (ctx.activeItem == id && ctx.io.mouseClicked) {
        dragAnchor = ctx.io.mouseX;
        startValue = *v;
    }
    if (ctx.activeItem == id && ctx.io.mouseDown) {
        float dx = ctx.io.mouseX - dragAnchor;
        float newV = startValue + dx * speed;
        newV = std::clamp(newV, v_min, v_max);
        if (newV != *v) { *v = newV; changed = true; }
    }
    if (ctx.activeItem == id && ctx.io.mouseReleased) {
        ctx.activeItem = 0;
        ctx.focusedItem = id;
    }

    // keyboard nudge when focused
    if (ctx.focusedItem == id) {
        float step = std::max( (v_max - v_min) / 200.0f, speed );
        if (ctx.io.keyLeft)  { *v = std::clamp((*v) - step, v_min, v_max); changed = true; }
        if (ctx.io.keyRight) { *v = std::clamp((*v) + step, v_min, v_max); changed = true; }
    }

    // value text
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, *v);
    int vw=0, vh=0; TTF_SizeUTF8(ctx.font, buf, &vw, &vh);
    ctx.commands.push_back({CmdType::Text, {r.x + (r.w - vw) * 0.5f, r.y + (r.h - vh) * 0.5f, float(vw), float(vh)}, buf, ctx.style.text});

    // layout
    L.cursorY += h + spacing;
    L.lastW = w; L.lastH = h; L.rowMaxH = std::max(L.rowMaxH, h);
    if (L.sameLineCount == 0) L.sameLine = false;

    return changed;
}

bool Combo(const char *label, int *current_index, const std::vector<std::string> &items, int max_visible_items) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float startX = ctx.currentWindowRect.x + pad;
    const float totalW = ctx.currentWindowRect.w - pad * 2.0f;
    const float rowH = ctx.style.menuItemHeight;

    // label
    int lw=0, lh=0; TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    ctx.commands.push_back({CmdType::Text, {L.cursorX, L.cursorY, float(lw), float(lh)}, label, ctx.style.text});

    // field
    float fieldW = std::min( totalW, 220.0f );
    float x = startX + lw + spacing;
    float h = float(ctx.fontSize) + pad;
    Rect field{x, L.cursorY, fieldW, h};

    // field bg
    ctx.commands.push_back({CmdType::Rect, field, "", {0.15f,0.15f,0.15f,1}});
    ctx.commands.push_back({CmdType::Rect, {field.x-1, field.y-1, field.w+2, field.h+2}, "", {0,0,0,0.35f}});

    // text
    std::string display = (*current_index >= 0 && *current_index < (int)items.size()) ? items[*current_index] : "";
    int tw=0, th=0; TTF_SizeUTF8(ctx.font, display.c_str(), &tw, &th);
    ctx.commands.push_back({CmdType::Text, {field.x + pad, field.y + (field.h - th) * 0.5f, float(tw), float(th)}, display, ctx.style.text});

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    bool hovered = HitTest(field, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;
    if (hovered && ctx.io.mouseReleased) {
        // toggle
        ctx.combo.openId = (ctx.combo.openId == id) ? 0 : id;
        if (ctx.combo.openId == id) {
            ctx.combo.scrollY = 0.0f;
            // place overlay just under the field
            int vis = std::max(1, std::min(max_visible_items, (int)items.size()));
            ctx.combo.rect = { field.x, field.y + field.h, field.w, vis * rowH + 2.0f };
        }
    }

    // layout advance for the field
    L.cursorY += h + spacing;
    L.lastW = fieldW; L.lastH = h; L.rowMaxH = std::max(L.rowMaxH, h);
    if (L.sameLineCount == 0) L.sameLine = false;

    bool changed = false;

    // draw overlay if open
    if (ctx.combo.openId == id) {
        Rect &overlay = ctx.combo.rect;

        // backdrop
        ctx.overlayCommands.push_back({CmdType::Rect, overlay, "", ctx.style.menuItemBg});
        ctx.overlayCommands.push_back({CmdType::Rect, {overlay.x-1, overlay.y-1, overlay.w+2, overlay.h+2}, "", {0,0,0,0.35f}});

        // mouse wheel over overlay
        bool overOverlay = HitTest(overlay, ctx.io.mouseX, ctx.io.mouseY);
        if (overOverlay) ctx.overlayHovering = true;
        if (overOverlay && ctx.io.mouseWheelY != 0.0f)
            ctx.combo.scrollY -= ctx.io.mouseWheelY * (rowH * 2.0f);
        ctx.combo.scrollY = std::max(0.0f, ctx.combo.scrollY);

        // clip & draw rows in overlay
        ctx.overlayCommands.push_back({CmdType::PushClip, overlay, "", {}});
        int first = int(ctx.combo.scrollY / rowH);
        float y0 = overlay.y - fmod(ctx.combo.scrollY, rowH);
        int visible = int(std::ceil(overlay.h / rowH)) + 2;

        for (int i = 0; i < visible; ++i) {
            int idx = first + i;
            if (idx < 0 || idx >= (int)items.size()) continue;
            Rect row{overlay.x, y0 + i * rowH, overlay.w, rowH};
            bool hvr = HitTest(row, ctx.io.mouseX, ctx.io.mouseY);
            if (hvr) ctx.overlayHovering = true;

            Color bg = (idx == *current_index) ? ctx.style.menuItemBg
                                               : (hvr ? ctx.style.menuItemHoverBg : Color{0,0,0,0});
            if (bg.a > 0.0f) ctx.overlayCommands.push_back({CmdType::Rect, row, "", bg});

            int rw=0, rh=0; TTF_SizeUTF8(ctx.font, items[idx].c_str(), &rw, &rh);
            ctx.overlayCommands.push_back({CmdType::Text,
                {row.x + pad, row.y + (row.h - rh) * 0.5f, float(rw), float(rh)}, items[idx], ctx.style.text});

            if (hvr && ctx.io.mouseReleased) {
                *current_index = idx;
                changed = true;
                ctx.combo.openId = 0; // close on pick
            }
        }
        ctx.overlayCommands.push_back({CmdType::PopClip, {}, "", {}});

        // click-away close
        if (ctx.io.mouseReleased && !HitTest(overlay, ctx.io.mouseX, ctx.io.mouseY) && !HitTest(field, ctx.io.mouseX, ctx.io.mouseY)) {
            ctx.combo.openId = 0;
        }
    } else {
        // clicking anywhere else (including menus) is handled by their own logic; no global close needed
    }

    return changed;
}


static void TooltipRequestCommon(const char *txt, bool allowOverlay) {
    auto &ctx = GetContext();
    if (!txt || !*txt) return;

    // Suppress global tooltips while overlay/menus are hovered
    if (!allowOverlay && ctx.overlayHovering) return;

    size_t id = ctx.hotItem;
    if (id == 0) return;

    auto &tp = ctx.tooltip;
    ctx.tooltip.want = true;
    ctx.tooltip.allowOverlay = allowOverlay;

    // Starting a new tooltip or changing text/id: reset delay timer
    Uint32 now = SDL_GetTicks();
    if (tp.id != id || tp.text != txt) {
        tp.id = id;
        tp.text = txt;
        tp.lastChangeMs = now;
        // keep current alpha for smooth cross-fade; do not force to 0
    }

    // Track latest mouse position for placement
    float pad = ctx.style.framePadding;
    tp.x = ctx.io.mouseX + 12.0f;
    tp.y = ctx.io.mouseY + 12.0f;

    // Clamp into current window bounds if possible
    const Rect &wr = ctx.currentWindowRect;
    int tw = 0, th = 0;
    if (ctx.font) TTF_SizeUTF8(ctx.font, txt, &tw, &th);
    float w = float(tw) + pad * 2.0f;
    float h = float(th) + pad * 2.0f;
    tp.x = std::min(tp.x, wr.x + wr.w - w);
    tp.y = std::min(tp.y, wr.y + wr.h - h);
}

void Tooltip(const char *txt) { TooltipRequestCommon(txt, /*allowOverlay=*/false); }
void TooltipOverlay(const char *txt) { TooltipRequestCommon(txt, /*allowOverlay=*/true); }

bool InputText(const char *label, char *buf, size_t buf_size) {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];

    size_t id = GenerateID(label);
    if (std::find(ctx.focusOrder.begin(), ctx.focusOrder.end(), id) == ctx.focusOrder.end())
        ctx.focusOrder.push_back(id);

    const float pad = ctx.style.framePadding;
    const float spacing = ctx.style.itemSpacing;
    const float winX = ctx.currentWindowRect.x;
    const float winW = ctx.currentWindowRect.w;

    // 1) Label
    int lw = 0, lh = 0;
    TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    Rect labelR{L.cursorX, L.cursorY, float(lw), float(lh)};
    ctx.commands.push_back({CmdType::Text, labelR, label, ctx.style.text});

    // 2) Field rect
    float fieldX = L.cursorX + lw + spacing;
    float fieldW = winW - (fieldX - winX) - pad;
    if (ctx.nextItem.hasWidth) fieldW = std::min(fieldW, ctx.nextItem.width);
    if (ctx.nextItem.hasXOffset) fieldX = winX + pad + ctx.nextItem.xoff;
    ctx.nextItem.clear();

    float fieldH = float(ctx.fontSize) + pad;
    Rect fieldR{fieldX, L.cursorY, fieldW, fieldH};

    // layout advance
    L.cursorY += fieldH + spacing;
    L.cursorX = winX + pad;
    L.lastW = fieldW;
    L.lastH = fieldH;
    L.rowMaxH = std::max(L.rowMaxH, fieldH);
    if (L.sameLineCount == 0) L.sameLine = false;

    // 3) Interaction
    bool hovered = HitTest(fieldR, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.hotItem = id;

    if (hovered && ctx.io.mouseClicked && ctx.activeItem == 0) {
        ctx.activeItem = id;
        ctx.focusedItem = id;
        ctx.textCursor[id] = int(strlen(buf));  // end
    }
    if (ctx.activeItem == id && ctx.io.mouseReleased && !hovered) {
        ctx.activeItem = 0;
    }

    // 4) Background & border
    Color idleBg{0.15f, 0.15f, 0.15f, 1.0f};
    Color hoverBg{0.20f, 0.20f, 0.20f, 1.0f};
    Color activeBg{0.25f, 0.25f, 0.25f, 1.0f};
    Color bg = (ctx.activeItem == id) ? activeBg : (hovered ? hoverBg : idleBg);
    ctx.commands.push_back({CmdType::Rect, fieldR, "", bg});
    Color borderCol{0.40f, 0.40f, 0.40f, 1.0f};
    ctx.commands.push_back({CmdType::Rect,
                            {fieldR.x - 1, fieldR.y - 1, fieldR.w + 2, fieldR.h + 2},
                            "",
                            borderCol});

    // Clip the text within the field
    PushClipRect(fieldR.x, fieldR.y, fieldR.w, fieldR.h);

    // 5) Input handling (UTF-8 aware)
    bool dirty = false;
    if (ctx.activeItem == id) {
        int &cpos = ctx.textCursor[id];
        float &scr = ctx.textScroll[id];
        int len = int(strlen(buf));

        // If we just moved focus here via Tab, ensure active editing
        if (ctx.focusedItem == id && ctx.prevFocusedItem != id && ctx.activeItem != id) {
            ctx.activeItem = id;
            ctx.textCursor[id] = int(strlen(buf));
        }

        if (ctx.io.keyLeft) cpos = utf8_prev_cp_start(buf, cpos);
        if (ctx.io.keyRight) cpos = utf8_next_cp_end(buf, len, cpos);
        if (ctx.io.keyHome) cpos = 0;
        if (ctx.io.keyEnd) cpos = len;

        // paste
        if (ctx.io.keyCtrlV) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                size_t clipLen = strlen(clip);
                if (size_t(len) + clipLen < buf_size) {
                    memmove(buf + cpos + clipLen, buf + cpos, len - cpos + 1);
                    memcpy(buf + cpos, clip, clipLen);
                    cpos += int(clipLen);
                    dirty = true;
                }
                SDL_free(clip);
            }
        }
        // typed chars
        for (char c : ctx.io.inputChars) {
            len = int(strlen(buf));
            if (size_t(len) + 1 < buf_size) {
                memmove(buf + cpos + 1, buf + cpos, len - cpos + 1);
                buf[cpos++] = c;
                dirty = true;
            }
        }
        // backspace (delete previous codepoint)
        if (ctx.io.backspace && cpos > 0) {
            int prev = utf8_prev_cp_start(buf, cpos);
            memmove(buf + prev, buf + cpos, len - cpos + 1);
            cpos = prev;
            dirty = true;
        }
        // enter = unfocus
        if (ctx.io.keyEnter) {
            ctx.activeItem = 0;
        }

        // scroll so caret is visible
        int preW = 0, preH = 0;
        std::string pre(buf, buf + cpos);
        TTF_SizeUTF8(ctx.font, pre.c_str(), &preW, &preH);
        if (preW - scr > fieldW - 8)
            scr = float(preW) - (fieldW - 8);
        else if (preW - scr < 0)
            scr = float(preW);
        scr = std::max(0.0f, scr);
    }

    // 6) Blinking caret
    if (ctx.activeItem == id) {
        Uint32 t = SDL_GetTicks() / 500;
        if ((t & 1) == 0) {
            int preW = 0, preH = 0;
            int cpos = ctx.textCursor[id];
            std::string pre(buf, buf + cpos);
            TTF_SizeUTF8(ctx.font, pre.c_str(), &preW, &preH);
            float cx = fieldR.x + 4.0f - ctx.textScroll[id] + float(preW);
            Rect cursorR{cx, fieldR.y + 2.0f, 2.0f, fieldR.h - 4.0f};
            ctx.commands.push_back({CmdType::Rect, cursorR, "", ctx.style.text});
        }
    }

    // 7) Draw text
    int textW = 0, textH = 0;
    TTF_SizeUTF8(ctx.font, buf, &textW, &textH);
    Rect txtR{
        fieldR.x + 4.0f - ctx.textScroll[id],
        fieldR.y + (fieldR.h - textH) * 0.5f,
        float(textW),
        float(textH)};
    ctx.commands.push_back({CmdType::Text, txtR, buf, {1, 1, 1, 1}});

    PopClipRect();
    return dirty;
}

// -----------------------------
// Persistence
// -----------------------------
void SaveLayout(const char *filename) {
    auto &ctx = GetContext();
    std::ofstream out(filename);
    if (!out.is_open()) {
        SDL_Log("timgui: could not open layout file for writing: %s", filename);
        return;
    }
    for (auto &kv : ctx.windowPositions) {
        const std::string &name = kv.first;
        const Rect &r = kv.second;
        float scroll = ctx.windowScrollY[name];
        out << name << " " << r.x << " " << r.y << " " << r.w << " " << r.h << " " << scroll << "\n";
    }
}

void LoadLayout(const char *filename) {
    auto &ctx = GetContext();
    std::ifstream in(filename);
    if (!in.is_open()) return;
    std::string name;
    float x, y, w, h, scroll;
    while (in >> name >> x >> y >> w >> h >> scroll) {
        ctx.windowPositions[name] = {x, y, w, h};
        ctx.windowScrollY[name] = scroll;
    }
}

// -----------------------------
// Menu Bar & Menus
// -----------------------------
bool BeginMenuBar() {
    auto &ctx = GetContext();
    if (!ctx.insideWindow) return false;
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    // building a new bar this frame; reset build stack
    g_menu.menuStack.clear();

    float barY = ctx.currentWindowRect.y + 20.0f;
    Rect barR{ctx.currentWindowRect.x, barY, ctx.currentWindowRect.w, ctx.style.menuBarHeight};
    ctx.overlayCommands.push_back({CmdType::Rect, barR, "", ctx.style.menuBarBg});

    L.cursorX = barR.x + ctx.style.framePadding;
    L.cursorY = barY + (barR.h - ctx.fontSize) * 0.5f;
    L.lastW = L.lastH = 0;
    L.sameLine = false;
    return true;
}

void EndMenuBar() {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    L.cursorY = ctx.currentWindowRect.y + 20.0f + ctx.style.menuBarHeight + ctx.style.itemSpacing;
    L.cursorX = ctx.currentWindowRect.x + ctx.style.framePadding;
    L.sameLine = false;
}

bool BeginMenu(const char *label) {
    auto &ctx = GetContext();
    auto &L = ctx.layouts[ctx.currentWindowTitle];
    size_t id = GenerateID(label);

    int tw = 0, th = 0;
    TTF_SizeUTF8(ctx.font, label, &tw, &th);
    Rect btn{L.cursorX, L.cursorY, float(tw + ctx.style.itemSpacing), ctx.style.menuItemHeight};

    bool hovered = HitTest(btn, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.overlayHovering = true;

    bool clicked = hovered && ctx.io.mouseReleased;
    if (clicked) {
        if (g_menu.activeMenu == id && g_menu.isOpen) {
            g_menu.isOpen = false;
            CloseAllSubMenus();
        } else {
            g_menu.activeMenu = id;
            g_menu.isOpen = true;
            CloseAllSubMenus();  // switching top-level: close all subs from previous tree
        }
    }

    // classic menubar: while open, hover switches active menu
    if (g_menu.isOpen && g_menu.activeMenu != id && hovered && (ctx.io.mouseDown || ctx.io.mouseReleased)) {
        g_menu.activeMenu = id;
        CloseAllSubMenus();
    }

    bool openNow = (g_menu.isOpen && g_menu.activeMenu == id);
    if (openNow) {
        g_menu.originX[id] = btn.x;
        g_menu.itemY[id] = btn.y + btn.h;
        g_menu.dropRect[id] = {btn.x, btn.y + btn.h, 0.0f, 0.0f};
        if (g_menu.menuStack.empty() || g_menu.menuStack.back() != id)
            g_menu.menuStack.push_back(id);
    }

    Color bg = openNow ? ctx.style.menuItemHoverBg : ctx.style.menuItemBg;
    ctx.overlayCommands.push_back({CmdType::Rect, btn, "", bg});
    ctx.overlayCommands.push_back({CmdType::Text,
                                   {btn.x + 4, btn.y + (btn.h - th) * 0.5f, float(tw), float(th)},
                                   label,
                                   ctx.style.text});

    L.cursorX += btn.w + ctx.style.itemSpacing;
    L.sameLine = true;

    return openNow;
}

void EndMenu() {
    auto &ctx = GetContext();

    if (ctx.io.mouseReleased && g_menu.isOpen) {
        Rect barRect{
            ctx.currentWindowRect.x,
            ctx.currentWindowRect.y + 20.0f,
            ctx.currentWindowRect.w,
            ctx.style.menuBarHeight};
        bool insideAny = HitTest(barRect, ctx.io.mouseX, ctx.io.mouseY);

        if (!insideAny) {
            if (HitTest(g_menu.dropRect[g_menu.activeMenu], ctx.io.mouseX, ctx.io.mouseY))
                insideAny = true;
            for (auto &p : g_menu.subOpen) {
                if (p.second && HitTest(g_menu.dropRect[p.first], ctx.io.mouseX, ctx.io.mouseY)) {
                    insideAny = true;
                    break;
                }
            }
        }
        if (!insideAny) {
            g_menu.isOpen = false;
            g_menu.menuStack.clear();
            CloseAllSubMenus();  // <— ensure no stale backgrounds
        }
    }

    if (!g_menu.menuStack.empty())
        g_menu.menuStack.pop_back();
}

// Internal: render a menu item into the current menu (top or sub)
// Returns true if clicked. Can opt out of auto-closing.
static bool MenuItemEx(const char *label,
                       bool enabled,
                       const char *accel,
                       const char *tip,
                       bool close_on_click,
                       bool *out_hovered,
                       Rect *out_rect) {
    auto &ctx = GetContext();
    // target menu = top of the build stack if present, otherwise active top-level
    size_t mid = g_menu.menuStack.empty() ? g_menu.activeMenu : g_menu.menuStack.back();

    // measure
    int lw = 0, lh = 0;
    TTF_SizeUTF8(ctx.font, label, &lw, &lh);
    int aw = 0, ah = 0;
    if (accel) TTF_SizeUTF8(ctx.font, accel, &aw, &ah);

    float pad = ctx.style.framePadding;
    float neededW = pad + lw + (accel ? (ctx.style.itemSpacing + aw) : 0) + pad;
    if (g_menu.dropRect[mid].w < neededW) g_menu.dropRect[mid].w = neededW;

    float x0 = g_menu.originX[mid];
    float y0 = g_menu.itemY[mid];
    Rect r{x0, y0, g_menu.dropRect[mid].w, ctx.style.menuItemHeight + 8.0f};
    g_menu.dropRect[mid].h += r.h;

    bool hovered = HitTest(r, ctx.io.mouseX, ctx.io.mouseY);
    if (hovered) ctx.overlayHovering = true;  // suppress global tooltips

    // draw
    Color bg = enabled ? (hovered ? ctx.style.menuItemHoverBg : ctx.style.menuItemBg)
                       : Color{0.15f, 0.15f, 0.15f, 1.0f};
    ctx.overlayCommands.push_back({CmdType::Rect, r, "", bg});
    ctx.overlayCommands.push_back({CmdType::Text,
                                   {r.x + pad, r.y + (r.h - lh) * 0.5f, float(lw), float(lh)},
                                   label,
                                   enabled ? ctx.style.text : Color{0.5f, 0.5f, 0.5f, 1.0f}});

    if (accel) {
        float ax = r.x + r.w - pad - aw;
        ctx.overlayCommands.push_back({CmdType::Text,
                                       {ax, r.y + (r.h - ah) * 0.5f, float(aw), float(ah)},
                                       accel,
                                       ctx.style.text});
    }

    if (tip && hovered) TooltipOverlay(tip);

    bool clicked = enabled && hovered && ctx.io.mouseReleased;

    // auto-close only if requested (submenu header will pass false)
    if (clicked && close_on_click) {
        g_menu.isOpen = false;
        g_menu.menuStack.clear();
        // (optional) g_menu.subOpen.clear(); // uncomment if you want hard close
    }

    g_menu.itemY[mid] += r.h;

    if (out_hovered) *out_hovered = hovered;
    if (out_rect) *out_rect = r;
    return clicked;
}

bool MenuItem(const char *label, bool enabled, const char *accel, const char *tip) {
    return MenuItemEx(label, enabled, accel, tip, /*close_on_click=*/true, nullptr, nullptr);
}

void MenuSeparator() {
    auto &ctx = GetContext();
    size_t mid = g_menu.menuStack.empty() ? g_menu.activeMenu : g_menu.menuStack.back();
    float x0 = g_menu.originX[mid];
    float y0 = g_menu.itemY[mid];
    float w = g_menu.dropRect[mid].w;
    float h = ctx.style.menuItemHeight;

    Rect r{x0, y0 + h * 0.5f, w, 1.0f};
    ctx.overlayCommands.push_back({CmdType::Rect, r, "", ctx.style.menuItemBg});
    g_menu.itemY[mid] += h;
    g_menu.dropRect[mid].h += h;
}

bool BeginSubMenu(const char *label) {
    auto &ctx = GetContext();

    bool hoveredParent = false;
    Rect parentR{};
    (void)MenuItemEx(label, /*enabled=*/true, /*accel=*/nullptr, /*tip=*/nullptr,
                     /*close_on_click=*/false, &hoveredParent, &parentR);

    size_t parent = g_menu.menuStack.empty() ? g_menu.activeMenu : g_menu.menuStack.back();
    size_t sid = GenerateID(label);

    // If we already had a drop rect from a previous frame, detect hover over the child body
    bool overChild = HitTest(g_menu.dropRect[sid], ctx.io.mouseX, ctx.io.mouseY);

    // Stay open while hovering the parent row OR the child panel itself
    bool open = hoveredParent || (g_menu.subOpen[sid] && (overChild || hoveredParent));

    if (open) {
        g_menu.subOpen[sid] = true;
        g_menu.parentMenu[sid] = parent;
        g_menu.parentItemRect[sid] = parentR;

        // child opens at the right edge of parent row
        g_menu.originX[sid] = parentR.x + parentR.w;
        g_menu.itemY[sid] = parentR.y;
        // Rebuild dropRect fresh this frame
        float baseW = g_menu.dropRect[parent].w;
        g_menu.dropRect[sid] = {g_menu.originX[sid], parentR.y, baseW, 0.0f};

        if (g_menu.menuStack.empty() || g_menu.menuStack.back() != sid)
            g_menu.menuStack.push_back(sid);

        // hovering parent or child should suppress global tooltips
        if (overChild || hoveredParent) ctx.overlayHovering = true;
        return true;
    }
    return false;
}

void EndSubMenu() {
    auto &ctx = GetContext();

    if (!g_menu.menuStack.empty())
        g_menu.menuStack.pop_back();

    // Close subs that are neither hovered in parent row nor over their child rect.
    for (auto &p : g_menu.subOpen) {
        if (!p.second) continue;
        size_t sid = p.first;

        // If this submenu no longer belongs under the active top-level, nuke it
        if (!IsUnderActive(sid)) {
            p.second = false;
            continue;
        }

        Rect childR = g_menu.dropRect[sid];
        Rect parentR = g_menu.parentItemRect[sid];

        bool overChild = HitTest(childR, ctx.io.mouseX, ctx.io.mouseY);
        bool overParent = HitTest(parentR, ctx.io.mouseX, ctx.io.mouseY);

        if (!(overChild || overParent)) {
            p.second = false;  // immediate close when leaving both (prevents stale bg)
        } else {
            ctx.overlayHovering = true;  // keep global tooltips suppressed
        }
    }
}

}  // namespace timgui
