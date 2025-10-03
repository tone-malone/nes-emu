// timgui.h - single-header immediate-mode GUI library for SDL2 + SDL2_ttf
#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace timgui {
// -----------------------------
// Basic types
// -----------------------------
struct Color {
    float r, g, b, a;
};
struct Rect {
    float x, y, w, h;
};

enum class CmdType {
    Rect,
    Text,
    PushClip,
    PopClip
};

struct DrawCmd {
    CmdType type;
    Rect rect;
    std::string text;  // for Text
    Color color;
};
using DrawList = std::vector<DrawCmd>;

// -----------------------------
// Input state (per frame)
// -----------------------------
struct IO {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool mouseDown = false;
    bool mouseClicked = false;
    bool mouseReleased = false;

    std::string inputChars;  // SDL_TEXTINPUT this frame
    bool backspace = false;

    // navigation / editing
    bool keyLeft = false;
    bool keyRight = false;
    bool keyHome = false;
    bool keyEnd = false;
    bool keyEnter = false;
    bool keyCtrlV = false;
    bool keyTab = false;
    bool keyShift = false;  // true if Shift is currently held when Tab pressed
    bool keySpace = false;
    float mouseWheelY = 0.0f;  // +1 up / -1 down (accumulated per frame)
    bool keyUp = false;
    bool keyDown = false;
    bool keyPageUp = false;
    bool keyPageDown = false;
};

// -----------------------------
// Layout per window
// -----------------------------
struct WindowLayout {
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    float lastW = 0.0f;
    float lastH = 0.0f;
    bool sameLine = false;

    // columns
    int columns = 0;
    int columnIndex = 0;
    float rowMaxH = 0.0f;  // tallest widget height in the current row

    // SameLine batching
    int sameLineCount = 0;         // how many items planned on this row
    int sameLineIndex = 0;         // which we're on
    float sameLineSpacing = 0.0f;  // spacing to use for the batch
};

// -----------------------------
// Styling
// -----------------------------
struct Style {
    Color windowBg;
    Color button;
    Color buttonHover;
    Color sliderTrack;
    Color sliderHandle;
    Color text;
    float framePadding;
    float itemSpacing;
    Color menuBarBg;
    Color menuItemBg;
    Color menuItemHoverBg;
    float menuBarHeight;
    float menuItemHeight;
};

enum class StyleColor {
    WindowBg,
    Button,
    ButtonHover,
    SliderTrack,
    SliderHandle,
    Text,
    MenuBarBg,
    MenuItemBg,
    MenuItemHoverBg
};

// single-use "next item" hints
struct NextItemData {
    bool hasWidth = false;
    float width = 0.0f;
    bool hasXOffset = false;
    float xoff = 0.0f;
    void clear() {
        hasWidth = false;
        hasXOffset = false;
        width = 0.0f;
        xoff = 0.0f;
    }
};

struct ClipRect {
    int x, y, w, h;
};

// -----------------------------
// Context
// -----------------------------
struct Context {
    IO io;
    DrawList commands;
    DrawList overlayCommands;  // menus & overlays
    DrawList tooltipCommands;

    Style style;
    Style baseStyle;  // snapshot of default theme to reset to
    std::vector<Style> styleStack;

    // windows
    std::string currentWindowTitle;
    Rect currentWindowRect;
    bool insideWindow = false;
    std::unordered_map<std::string, WindowLayout> layouts;
    std::unordered_map<std::string, Rect> windowPositions;
    std::unordered_map<std::string, float> windowScrollY;

    // SDL_ttf
    SDL_Renderer *renderer = nullptr;
    TTF_Font *font = nullptr;
    int fontSize = 0;

    // interaction
    size_t hotItem = 0;
    size_t activeItem = 0;
    size_t resizeItem = 0;
    size_t focusedItem = 0;
    std::vector<std::string> idStack;
    float lastMouseX = 0.0f;
    float lastMouseY = 0.0f;

    // --- focus order (keyboard traversal) ---
    std::vector<size_t> focusOrder;  // ids in draw order (per-frame)
    size_t prevFocusedItem = 0;      // to detect newly-focused

    // --- child panels (scrollable) ---
    struct ChildFrame {
        size_t id;
        Rect rect;
        float startCursorX, startCursorY;
        float contentStartY;
    };
    std::vector<ChildFrame> childStack;
    std::unordered_map<size_t, float> childScrollY;  // per-child vertical scroll

    // text edit state
    std::unordered_map<size_t, int> textCursor;    // byte index into buf
    std::unordered_map<size_t, float> textScroll;  // horizontal px scroll

    // per-next widget hints
    NextItemData nextItem;

    // clipping
    std::vector<ClipRect> clipStack;

        // simple list & combo state
    std::unordered_map<size_t, float> listScrollY; // per-listbox scroll

    struct ComboState {
        size_t openId = 0;   // which combo is open (0 = none)
        Rect   rect{};       // overlay rect (for hit test)
        float  scrollY = 0;  // list scroll within overlay
    } combo;


    // --- tooltip state (delay + fade) ---
    struct TooltipState {
        bool want = false;          // someone requested a tooltip this frame
        bool allowOverlay = false;  // allowed even when overlay (menus) hovered
        size_t id = 0;              // item being tooltiped
        Uint32 lastChangeMs = 0;    // when id/text changed (for delay)
        Uint32 lastTickMs = 0;      // for fade step time delta
        float alpha = 0.0f;         // current opacity 0..1
        std::string text;           // what to render
        float x = 0.0f, y = 0.0f;   // screen position
    } tooltip;

    float tooltipDelayMs = 350.0f;  // delay before showing
    float tooltipFadeMs = 150.0f;   // fade in/out duration
    bool overlayHovering = false;   // true if menus/overlay are hovered this frame

    // text cache (for performance)
    struct TextKey {
        TTF_Font *f;
        Uint32 rgba;
        std::string s;
        bool operator==(const TextKey &o) const {
            return f == o.f && rgba == o.rgba && s == o.s;
        }
    };
    struct TextKeyHash {
        size_t operator()(TextKey const &k) const {
            return std::hash<void *>{}(k.f) ^ (size_t(k.rgba) * 1315423911u) ^ std::hash<std::string>{}(k.s);
        }
    };
    struct TextCacheEntry {
        SDL_Texture *tex = nullptr;
        int w = 0, h = 0;
        int age = 0;
    };
    std::unordered_map<TextKey, TextCacheEntry, TextKeyHash> textCache;
    int cacheAge = 0;
    int cacheBudget = 200;  // max cache entries
};

// -----------------------------
// Core API
// -----------------------------
void CreateContext();
void DestroyContext();
Context &GetContext();

bool Init(SDL_Renderer *renderer, const char *fontFile, int fontSize);
void NewFrame();
void EndFrame();

// SDL -> IO helper
void HandleSDLEvent(const SDL_Event &e);

// Layout helpers
void SameLine(float spacing = -1.0f);
void SameLineItemCount(int count, float spacing = -1.0f);
void SetNextItemWidth(float w);
void SetNextItemXOffset(float xoff);
void NewLine();
void Separator();
void CalcTextSize(const char *text, int &out_w, int &out_h);

// Scrollable child panels
bool BeginChild(const char *id, float w, float h, bool border = true);
void EndChild();

// Internal: keyboard navigation
void HandleKeyboardNav();  // called once per frame (we'll invoke from EndFrame)

// Columns
void Columns(int count);
void NextColumn();
void EndColumns();

// Clipping
void PushClipRect(float x, float y, float w, float h);
void PopClipRect();

// Rendering
void Render(const std::function<void(const DrawCmd &)> &renderer);
void RenderSDL();

// Windows
bool Begin(const char *title, bool *p_open, float x, float y, float w, float h);
void End();

// ID stack
void PushID(const char *str_id);
void PopID();

// Styling
void PushStyleColor(StyleColor which, Color col);
void PopStyleColor();
void PushStyleVar(float *var_ptr, float new_value);
void PopStyleVar();
void ResetStyle();

// Widgets
void Text(const char *txt);
void TextF(const char *fmt, ...);
// Wrapped text (wrap at word boundaries to available width or custom width)
void TextWrapped(const char *txt, float wrap_width = -1.0f);

bool Checkbox(const char *label, bool *v);
bool Button(const char *label);
bool SliderFloat(const char *label, float *v, float v_min, float v_max);
void ProgressBar(float fraction, float width = -1.0f);

bool RadioButton(const char *label, int *v, int v_value);
bool Selectable(const char *label, bool *selected = nullptr, bool full_width = true);
bool ListBox(const char *label, int *current_index, const std::vector<std::string> &items, int height_in_items = 6);
bool Combo(const char *label, int *current_index, const std::vector<std::string> &items, int max_visible_items = 6);
bool DragFloat(const char *label, float *v, float speed, float v_min, float v_max, const char *format = "%.3f");


// Inputs
bool InputInt(const char *label, int *v);
bool InputFloat(const char *label, float *v, float v_min, float v_max, const char *format = "%.3f");
bool InputText(const char *label, char *buf, size_t buf_size);
void Tooltip(const char *txt);
// Same as Tooltip() but permitted while hovering overlay/menus
void TooltipOverlay(const char *txt);


// Persistence
void SaveLayout(const char *filename);
void LoadLayout(const char *filename);

// Menus
bool BeginMenuBar();
void EndMenuBar();
bool BeginMenu(const char *label);
void EndMenu();
bool MenuItem(const char *label, bool enabled = true, const char *accel = nullptr, const char *tip = nullptr);
void MenuSeparator();
bool BeginSubMenu(const char *label);
void EndSubMenu();

}  // namespace timgui
