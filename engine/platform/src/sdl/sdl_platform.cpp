// engine/platform/src/sdl/sdl_platform.cpp — the SDL2 desktop backend (Linux/Windows/macOS).
// It implements the SAME C seam as the null backend, so the entire engine + example run
// UNCHANGED on a real window: it owns the software framebuffer (render tier = software),
// uploads it to a streaming SDL texture each present, maps the keyboard to the canonical
// phx button bits, and uses a real monotonic clock. The GL backend will later replace the
// texture upload with GPU draws; until then this makes `make platformer` a game you can see.
//
// Compiled ONLY when PHX_HAVE_SDL is defined (and linked INSTEAD OF null_platform.cpp); the
// guard makes the translation unit empty otherwise, so it is harmless to list in a build that
// has no SDL2. See docs/02-platform-layer.md.
#if defined(PHX_HAVE_SDL)

#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/platform/desktop.h"

#include <SDL.h>
#if defined(PHX_HAVE_GL)
#include <SDL_opengl.h>      // glReadPixels for the verification readback (GL render tier)
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>          // _chdir
#else
#include <unistd.h>          // chdir
#endif

// The real audio device (defined at the bottom), also offered through the seam's audio().
extern "C" int  phx_sdl_audio_start(int rate, phx_audio_fill fill, void* user);
extern "C" void phx_sdl_audio_stop(void);

namespace {

// Canonical phx button bit order — MUST match phx::Button in engine/input (Up=0 .. Select=11).
enum Btn : uint32_t {
    B_UP = 0, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_X, B_Y, B_L, B_R, B_START, B_SELECT
};

// Window is g_scale× the logical framebuffer for visibility. 3× unless a desktop tool picks
// another integer scale BEFORE init via phx_sdl_set_window_scale() (games never do).
int g_scale = 3;

struct SdlState {
    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;             // software-present path (no GL)
    SDL_Texture*  tex = nullptr;
#if defined(PHX_HAVE_GL)
    SDL_GLContext glctx = nullptr;           // GL-render-tier path
#endif
    phx_soft_fb   fb  { nullptr, 0, 0 };     // soft path: CPU framebuffer; GL path: logical size only (pixels=null)
    uint64_t      freq = 1;                   // performance-counter frequency
    uint64_t      base = 0;                   // counter at init (clock origin)
    int           quit = 0;
    SDL_GameController* pad = nullptr;        // first connected controller (hotplugged)
};
SdlState g;

// --- desktop extension state (phx/platform/desktop.h; tools only — games never touch it) ---
constexpr int kEvCap = 256;                   // bounded ring: overflow drops the OLDEST event
struct DesktopState {
    phx_desktop_event ring[kEvCap];
    int  head = 0, count = 0;
    int  quit_on_escape = 1;
    int  confirm_quit = 0;
    int  resizable = 0;
    int  off_x = 0, off_y = 0;                // letterbox offset of the fb inside the window
    char drop_path[1024] = { 0 };
    char* clip = nullptr;                     // last clipboard_get() result (SDL-owned copy)
    SDL_Cursor* cursors[PHX_CURSOR_COUNT] = { nullptr };
    int  cursor = -1;
};
DesktopState g_dt;

#if !defined(PHX_HAVE_GL)
// The native-resolution overlay (phx_desktop_overlay_begin): a CPU layer at window resolution and
// the streaming texture it is uploaded to; `live` = begun this frame, shown by the next present().
struct OverlayState {
    uint32_t*    px  = nullptr;
    int          w = 0, h = 0;
    SDL_Texture* tex = nullptr;
    bool         live = false;
};
OverlayState g_ov;

void overlay_free() {
    std::free(g_ov.px); g_ov.px = nullptr;
    if (g_ov.tex) SDL_DestroyTexture(g_ov.tex);
    g_ov.tex = nullptr; g_ov.w = g_ov.h = 0; g_ov.live = false;
}
#endif

void dt_push(const phx_desktop_event& e) {
    if (g_dt.count == kEvCap) { g_dt.head = (g_dt.head + 1) % kEvCap; --g_dt.count; }
    g_dt.ring[(g_dt.head + g_dt.count) % kEvCap] = e;
    ++g_dt.count;
}

uint16_t dt_mods(Uint16 m) {
    uint16_t o = 0;
    if (m & KMOD_SHIFT) o |= PHX_MOD_SHIFT;
    if (m & KMOD_CTRL)  o |= PHX_MOD_CTRL;
    if (m & KMOD_ALT)   o |= PHX_MOD_ALT;
    if (m & KMOD_GUI)   o |= PHX_MOD_GUI;
    return o;
}

// SDL keycode -> phx_key. Printable keys keep their (layout-aware) ASCII code, lower-cased.
int32_t dt_key(SDL_Keycode k) {
    if (k >= 32 && k < 127) return (k >= 'A' && k <= 'Z') ? int32_t(k - 'A' + 'a') : int32_t(k);
    switch (k) {
    case SDLK_ESCAPE:    return PHX_KEY_ESCAPE;
    case SDLK_RETURN: case SDLK_KP_ENTER: return PHX_KEY_ENTER;
    case SDLK_TAB:       return PHX_KEY_TAB;
    case SDLK_BACKSPACE: return PHX_KEY_BACKSPACE;
    case SDLK_DELETE:    return PHX_KEY_DELETE;
    case SDLK_INSERT:    return PHX_KEY_INSERT;
    case SDLK_LEFT:      return PHX_KEY_LEFT;
    case SDLK_RIGHT:     return PHX_KEY_RIGHT;
    case SDLK_UP:        return PHX_KEY_UP;
    case SDLK_DOWN:      return PHX_KEY_DOWN;
    case SDLK_HOME:      return PHX_KEY_HOME;
    case SDLK_END:       return PHX_KEY_END;
    case SDLK_PAGEUP:    return PHX_KEY_PAGE_UP;
    case SDLK_PAGEDOWN:  return PHX_KEY_PAGE_DOWN;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return PHX_KEY_SHIFT;
    case SDLK_LCTRL:  case SDLK_RCTRL:  return PHX_KEY_CTRL;
    case SDLK_LALT:   case SDLK_RALT:   return PHX_KEY_ALT;
    case SDLK_LGUI:   case SDLK_RGUI:   return PHX_KEY_GUI;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return PHX_KEY_F1 + int32_t(k - SDLK_F1);
    return PHX_KEY_NONE;
}

// Window pixels -> framebuffer pixels (undo the integer upscale and the letterbox offset).
void win_to_fb(int wx, int wy, int& fx, int& fy) {
    fx = (wx - g_dt.off_x) / g_scale;
    fy = (wy - g_dt.off_y) / g_scale;
    if (wx < g_dt.off_x) fx = -1;
    if (wy < g_dt.off_y) fy = -1;
}

int sdl_init(const phx_platform_desc* desc) {
    const int w = desc->width  > 0 ? desc->width  : 240;
    const int h = desc->height > 0 ? desc->height : 160;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[phx.sdl] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const char* title = desc->title ? desc->title : "Phosphorus Engine";

#if defined(PHX_HAVE_GL)
    // GL render tier: an OpenGL context + double buffering. The GL backend draws; we swap.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * g_scale, h * g_scale, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g.win) { std::fprintf(stderr, "[phx.sdl] CreateWindow(GL): %s\n", SDL_GetError()); return 1; }
    g.glctx = SDL_GL_CreateContext(g.win);
    if (!g.glctx) { std::fprintf(stderr, "[phx.sdl] GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(desc->vsync ? 1 : 0);
    g.fb.w = w; g.fb.h = h; g.fb.pixels = nullptr;   // logical size only; GL owns the pixels
    std::printf("[phx.sdl] init '%s' %dx%d GL (window %dx%d, vsync=%d)\n",
                title, w, h, w * g_scale, h * g_scale, desc->vsync);
#else
    // Software render tier: a streaming texture we upload the CPU framebuffer into each frame.
    g.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * g_scale, h * g_scale, SDL_WINDOW_SHOWN);
    if (!g.win) { std::fprintf(stderr, "[phx.sdl] CreateWindow: %s\n", SDL_GetError()); return 1; }

    Uint32 rflags = SDL_RENDERER_ACCELERATED | (desc->vsync ? Uint32(SDL_RENDERER_PRESENTVSYNC) : 0u);
    g.ren = SDL_CreateRenderer(g.win, -1, rflags);
    if (!g.ren) { std::fprintf(stderr, "[phx.sdl] CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(g.ren, w, h);    // crisp integer upscale of the framebuffer

    // phx Rgba is R|G<<8|B<<16|A<<24 -> bytes (MSB..LSB) A,B,G,R == SDL_PIXELFORMAT_ABGR8888.
    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g.tex) { std::fprintf(stderr, "[phx.sdl] CreateTexture: %s\n", SDL_GetError()); return 1; }

    g.fb.w = w; g.fb.h = h;
    g.fb.pixels = static_cast<uint32_t*>(std::calloc(size_t(w) * size_t(h), sizeof(uint32_t)));
    if (!g.fb.pixels) return 1;
    std::printf("[phx.sdl] init '%s' %dx%d SW (window %dx%d, vsync=%d)\n",
                title, w, h, w * g_scale, h * g_scale, desc->vsync);
#endif

    // Controllers are optional: a failure here (no evdev access, headless CI) must not take
    // the window down, so init the subsystem separately and just log the outcome. Already-
    // connected pads arrive as SDL_CONTROLLERDEVICEADDED events on the first pump.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        std::fprintf(stderr, "[phx.sdl] no controller support: %s\n", SDL_GetError());

    g.freq = SDL_GetPerformanceFrequency();
    g.base = SDL_GetPerformanceCounter();
    g.quit = 0;
    return 0;
}

#if !defined(PHX_HAVE_GL)
// Resizable (tool) mode: the framebuffer follows the window at the integer UI scale. The soft
// renderer re-locks g.fb every frame, so swapping the buffer here between frames is safe.
void refit_framebuffer(bool announce) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(g.win, &ww, &wh);
    int w = ww / g_scale, h = wh / g_scale;
    if (w < 64) w = 64;
    if (h < 48) h = 48;
    g_dt.off_x = (ww - w * g_scale) / 2; if (g_dt.off_x < 0) g_dt.off_x = 0;
    g_dt.off_y = (wh - h * g_scale) / 2; if (g_dt.off_y < 0) g_dt.off_y = 0;
    if (w == g.fb.w && h == g.fb.h) return;
    uint32_t* px = static_cast<uint32_t*>(std::calloc(size_t(w) * size_t(h), sizeof(uint32_t)));
    SDL_Texture* tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!px || !tex) { std::free(px); if (tex) SDL_DestroyTexture(tex); return; }   // keep the old one
    std::free(g.fb.pixels);
    SDL_DestroyTexture(g.tex);
    g.fb.pixels = px; g.fb.w = w; g.fb.h = h;
    g.tex = tex;
    SDL_RenderSetLogicalSize(g.ren, w, h);
    SDL_RenderSetIntegerScale(g.ren, SDL_TRUE);
    if (announce) {
        phx_desktop_event e{};
        e.kind = PHX_DEV_RESIZE; e.x = int16_t(w); e.y = int16_t(h);
        dt_push(e);
    }
}
#endif

void sdl_shutdown(void) {
    if (g.pad) { SDL_GameControllerClose(g.pad); g.pad = nullptr; }
    for (SDL_Cursor*& c : g_dt.cursors) if (c) { SDL_FreeCursor(c); c = nullptr; }
    if (g_dt.clip) { SDL_free(g_dt.clip); g_dt.clip = nullptr; }
    g_dt.count = 0; g_dt.head = 0; g_dt.cursor = -1;
#if defined(PHX_HAVE_GL)
    if (g.glctx) SDL_GL_DeleteContext(g.glctx);
    g.glctx = nullptr;
#else
    overlay_free();
    std::free(g.fb.pixels); g.fb.pixels = nullptr;
    if (g.tex) SDL_DestroyTexture(g.tex);
    if (g.ren) SDL_DestroyRenderer(g.ren);
    g.tex = nullptr; g.ren = nullptr;
#endif
    g.fb.w = g.fb.h = 0;
    if (g.win) SDL_DestroyWindow(g.win);
    g.win = nullptr;
    SDL_Quit();
}

uint64_t sdl_clock_ns(void) {
    const uint64_t now = SDL_GetPerformanceCounter() - g.base;
    // ns = now * 1e9 / freq, computed to avoid overflow on large counters
    return (now / g.freq) * 1000000000ull + ((now % g.freq) * 1000000000ull) / g.freq;
}
void sdl_sleep_ns(uint64_t ns) { SDL_Delay(Uint32(ns / 1000000ull)); }

int sdl_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        phx_desktop_event d{};
        if (e.type == SDL_QUIT) {
            if (g_dt.confirm_quit) { d.kind = PHX_DEV_QUIT; dt_push(d); }
            else g.quit = 1;
        } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            if (e.type == SDL_KEYDOWN && g_dt.quit_on_escape &&
                e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g.quit = 1;
            d.kind = e.type == SDL_KEYDOWN ? PHX_DEV_KEY_DOWN : PHX_DEV_KEY_UP;
            d.key = dt_key(e.key.keysym.sym);
            d.mods = dt_mods(e.key.keysym.mod);
            d.repeat = e.key.repeat ? 1 : 0;
            if (d.key != PHX_KEY_NONE) dt_push(d);
        } else if (e.type == SDL_TEXTINPUT) {
            d.kind = PHX_DEV_TEXT;
            std::memcpy(d.text, e.text.text, sizeof(d.text) - 1);   // both 32 bytes; d.text[31] stays 0
            d.mods = dt_mods(SDL_GetModState());
            dt_push(d);
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            const Uint8 b = e.button.button;
            if (b != SDL_BUTTON_LEFT && b != SDL_BUTTON_MIDDLE && b != SDL_BUTTON_RIGHT) continue;
            d.kind = e.type == SDL_MOUSEBUTTONDOWN ? PHX_DEV_MOUSE_DOWN : PHX_DEV_MOUSE_UP;
            d.button = b == SDL_BUTTON_LEFT ? PHX_MOUSE_LEFT : b == SDL_BUTTON_MIDDLE ? PHX_MOUSE_MIDDLE : PHX_MOUSE_RIGHT;
            d.clicks = e.button.clicks;
            int wx = 0, wy = 0, fx = 0, fy = 0;
            SDL_GetMouseState(&wx, &wy);
            win_to_fb(wx, wy, fx, fy);
            d.x = int16_t(fx); d.y = int16_t(fy);
            d.mods = dt_mods(SDL_GetModState());
            dt_push(d);
        } else if (e.type == SDL_MOUSEWHEEL) {
            d.kind = PHX_DEV_WHEEL;
            const int flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            d.wheel_x = int16_t(e.wheel.x * flip); d.wheel_y = int16_t(e.wheel.y * flip);
            int wx = 0, wy = 0, fx = 0, fy = 0;
            SDL_GetMouseState(&wx, &wy);
            win_to_fb(wx, wy, fx, fy);
            d.x = int16_t(fx); d.y = int16_t(fy);
            d.mods = dt_mods(SDL_GetModState());
            if (d.wheel_x || d.wheel_y) dt_push(d);
        } else if (e.type == SDL_DROPFILE) {
            if (e.drop.file) {
                std::snprintf(g_dt.drop_path, sizeof(g_dt.drop_path), "%s", e.drop.file);
                SDL_free(e.drop.file);
                d.kind = PHX_DEV_DROP_FILE;
                dt_push(d);
            }
        } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
#if !defined(PHX_HAVE_GL)
            if (g_dt.resizable) refit_framebuffer(true);
#endif
        } else if (e.type == SDL_CONTROLLERDEVICEADDED && !g.pad) {
            g.pad = SDL_GameControllerOpen(e.cdevice.which);
            if (g.pad) std::printf("[phx.sdl] controller: %s\n", SDL_GameControllerName(g.pad));
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && g.pad &&
                   e.cdevice.which == SDL_JoystickInstanceID(
                       SDL_GameControllerGetJoystick(g.pad))) {
            SDL_GameControllerClose(g.pad);
            g.pad = nullptr;                       // a still-connected pad re-adds on hotplug
            std::printf("[phx.sdl] controller disconnected\n");
        }
    }
    return g.quit ? 0 : 1;
}

#if !defined(PHX_HAVE_GL)
// Blend this frame's overlay (if a tool began one) over the framebuffer just copied. The texture
// is fb * scale texels drawn into the fb-sized logical rect, i.e. exactly one texel per window pixel.
void overlay_draw() {
    if (!g_ov.live || !g_ov.tex) return;
    if (g_ov.w != g.fb.w * g_scale || g_ov.h != g.fb.h * g_scale) return;   // resized since begin: skip a frame
    SDL_UpdateTexture(g_ov.tex, nullptr, g_ov.px, g_ov.w * int(sizeof(uint32_t)));
    const SDL_Rect dst{ 0, 0, g.fb.w, g.fb.h };
    SDL_RenderCopy(g.ren, g_ov.tex, nullptr, &dst);
}
#endif

void sdl_present(void) {
#if defined(PHX_HAVE_GL)
    SDL_GL_SwapWindow(g.win);            // the GL backend already drew into the back buffer
#else
    SDL_UpdateTexture(g.tex, nullptr, g.fb.pixels, g.fb.w * int(sizeof(uint32_t)));
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
    overlay_draw();
    SDL_RenderPresent(g.ren);
    g_ov.live = false;
#endif
}

phx_gfx*   sdl_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g); }   // gfx_soft_lock reads g.fb
phx_audio  g_audio_device{ 44100, phx_sdl_audio_start, phx_sdl_audio_stop };
phx_audio* sdl_audio(void) { return &g_audio_device; }

// --- real audio device --------------------------------------------------------------------
// The platform owns the device but NOT the mixer (layering: platform must not depend on audio).
// The game registers a fill callback that drains its lock-free AudioCommandQueue and calls
// AudioMixer::mix(); SDL invokes it on the audio thread, so the mixer is touched single-threaded.
struct AudioState { SDL_AudioDeviceID dev; phx_audio_fill fill; void* user; };
AudioState g_audio{ 0, nullptr, nullptr };

void SDLCALL sdl_audio_trampoline(void* userdata, Uint8* stream, int len) {
    AudioState* a = static_cast<AudioState*>(userdata);
    const int frames = len / int(sizeof(int16_t) * 2);   // interleaved stereo S16
    if (a->fill) a->fill(a->user, reinterpret_cast<int16_t*>(stream), frames);
    else std::memset(stream, 0, size_t(len));
}

void sdl_poll_input(phx_input_raw* out) {
    std::memset(out, 0, sizeof(*out));
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t b = 0;
    auto set = [&](Btn bit, bool on) { if (on) b |= (1u << uint32_t(bit)); };
    set(B_UP,    k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]);
    set(B_DOWN,  k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]);
    set(B_LEFT,  k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]);
    set(B_RIGHT, k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]);
    set(B_A,     k[SDL_SCANCODE_Z]     || k[SDL_SCANCODE_SPACE]);
    set(B_B,     k[SDL_SCANCODE_X]);
    set(B_X,     k[SDL_SCANCODE_C]);
    set(B_Y,     k[SDL_SCANCODE_V]);
    set(B_L,     k[SDL_SCANCODE_Q]);
    set(B_R,     k[SDL_SCANCODE_E]);
    set(B_START, k[SDL_SCANCODE_RETURN]);
    set(B_SELECT,k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_TAB]);

    // Game controller: OR'd with the keyboard (both always live; no mode switch). Face
    // buttons map by POSITION (SDL's A = south), matching the console layouts the canonical
    // order mirrors; the left stick ALSO reaches the dpad via the input module's synthesis.
    if (g.pad) {
        SDL_GameController* p = g.pad;
        auto pb = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(p, btn) != 0; };
        set(B_UP,     pb(SDL_CONTROLLER_BUTTON_DPAD_UP));
        set(B_DOWN,   pb(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
        set(B_LEFT,   pb(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        set(B_RIGHT,  pb(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
        set(B_A,      pb(SDL_CONTROLLER_BUTTON_A));
        set(B_B,      pb(SDL_CONTROLLER_BUTTON_B));
        set(B_X,      pb(SDL_CONTROLLER_BUTTON_X));
        set(B_Y,      pb(SDL_CONTROLLER_BUTTON_Y));
        set(B_L,      pb(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        set(B_R,      pb(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
        set(B_START,  pb(SDL_CONTROLLER_BUTTON_START));
        set(B_SELECT, pb(SDL_CONTROLLER_BUTTON_BACK));
        out->axis[0] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
        out->axis[1] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
        out->axis[2] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTX);
        out->axis[3] = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_RIGHTY);
        out->connected_pads = 1;
    }
    out->buttons = b;

    int mx = 0, my = 0, fx = 0, fy = 0;
    Uint32 ms = SDL_GetMouseState(&mx, &my);
    // Mouse arrives in WINDOW pixels; the seam promises framebuffer coordinates, so undo
    // the integer upscale (tools like phxtmap hit-test tiles against these).
    win_to_fb(mx, my, fx, fy);
    out->pointer_x = int16_t(fx); out->pointer_y = int16_t(fy);
    out->pointer_down = (ms & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1 : 0;
}

// File I/O: load-once into a heap buffer; map() returns a stable pointer (seam contract).
struct SdlFile { void* data; size_t size; };

phx_file* sdl_open(const char* path, size_t* out_size) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) { if (out_size) *out_size = 0; return nullptr; }
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (n < 0) { std::fclose(fp); if (out_size) *out_size = 0; return nullptr; }
    SdlFile* h = static_cast<SdlFile*>(std::malloc(sizeof(SdlFile)));
    h->size = size_t(n);
    h->data = std::malloc(h->size ? h->size : 1);
    size_t got = std::fread(h->data, 1, h->size, fp);
    std::fclose(fp);
    if (got != h->size) { std::free(h->data); std::free(h); if (out_size) *out_size = 0; return nullptr; }
    if (out_size) *out_size = h->size;
    return reinterpret_cast<phx_file*>(h);
}
const void* sdl_map(phx_file* f) { return f ? reinterpret_cast<SdlFile*>(f)->data : nullptr; }
void        sdl_close(phx_file* f) {
    if (!f) return;
    SdlFile* h = reinterpret_cast<SdlFile*>(f);
    std::free(h->data); std::free(h);
}

// Persistence: a plain save file keyed by path (the desktop store).
int sdl_save(const char* key, const void* data, uint32_t size) {
    FILE* fp = std::fopen(key, "wb");
    if (!fp) return 1;
    size_t w = std::fwrite(data, 1, size, fp);
    std::fclose(fp);
    return (w == size) ? 0 : 1;
}
int sdl_load(const char* key, void* out, uint32_t cap, uint32_t* out_size) {
    FILE* fp = std::fopen(key, "rb");
    if (!fp) { if (out_size) *out_size = 0; return 1; }
    size_t got = std::fread(out, 1, cap, fp);
    std::fclose(fp);
    if (out_size) *out_size = uint32_t(got);
    return 0;
}

void sdl_log(phx_log_level level, const char* msg) {
    static const char* tag[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
    int l = int(level); if (l < 0 || l > 4) l = 2;
    std::printf("[phx.sdl][%s] %s\n", tag[l], msg);
}

const phx_platform g_sdl_platform = {
    sdl_init, sdl_shutdown,
    sdl_clock_ns, sdl_sleep_ns,
    sdl_pump_events, sdl_present,
    sdl_gfx, sdl_audio,
    sdl_poll_input,
    sdl_open, sdl_map, sdl_close,
    sdl_save, sdl_load,
    sdl_log,
};

} // namespace

extern "C" const phx_platform* phx_platform_get(void) { return &g_sdl_platform; }

// --- desktop-only extension: window scale ----------------------------------------------------
// Like phx_sdl_audio_start / phx_sdl_readback: an extra extern "C" symbol only this TU exports,
// declared by the desktop tool that uses it, absent on every console backend. Must be called
// before App::run() boots the platform (it sizes the window at init). Clamped to 1..6; the
// default stays 3×, so every game and verifier is unaffected. Used by phxstudio, whose 640×360
// canvas at 2× is a 1280×720 window (3× would not fit a 1080p screen).
extern "C" void phx_sdl_set_window_scale(int scale) {
    g_scale = scale < 1 ? 1 : (scale > 6 ? 6 : scale);
}

// --- desktop extension (phx/platform/desktop.h) ----------------------------------------------
// Tool-only: events beyond the 12 canonical buttons, a resizable framebuffer, clipboard, cursor.
// Games never call any of it, so their window/input behaviour is byte-for-byte unchanged.
extern "C" int phx_desktop_available(void) { return g.win ? 1 : 0; }

extern "C" int phx_desktop_poll(phx_desktop_event* out) {
    if (!out || g_dt.count == 0) return 0;
    *out = g_dt.ring[g_dt.head];
    g_dt.head = (g_dt.head + 1) % kEvCap;
    --g_dt.count;
    return 1;
}

extern "C" void phx_desktop_mouse(int* x, int* y, uint32_t* buttons) {
    int wx = 0, wy = 0, fx = -1, fy = -1;
    const Uint32 ms = SDL_GetMouseState(&wx, &wy);
    win_to_fb(wx, wy, fx, fy);
    if (fx >= g.fb.w || fy >= g.fb.h) { fx = -1; fy = -1; }
    if (x) *x = fx;
    if (y) *y = fy;
    if (buttons) {
        uint32_t b = 0;
        if (ms & SDL_BUTTON(SDL_BUTTON_LEFT))   b |= 1u << PHX_MOUSE_LEFT;
        if (ms & SDL_BUTTON(SDL_BUTTON_MIDDLE)) b |= 1u << PHX_MOUSE_MIDDLE;
        if (ms & SDL_BUTTON(SDL_BUTTON_RIGHT))  b |= 1u << PHX_MOUSE_RIGHT;
        *buttons = b;
    }
}
extern "C" uint16_t phx_desktop_mods(void) { return dt_mods(SDL_GetModState()); }

extern "C" void phx_desktop_set_quit_on_escape(int enable) { g_dt.quit_on_escape = enable ? 1 : 0; }
extern "C" void phx_desktop_set_confirm_quit(int enable)   { g_dt.confirm_quit = enable ? 1 : 0; }

extern "C" void phx_desktop_text_input(int enable) {
    if (enable) SDL_StartTextInput(); else SDL_StopTextInput();
}

extern "C" void phx_desktop_set_resizable(int enable) {
    g_dt.resizable = enable ? 1 : 0;
#if !defined(PHX_HAVE_GL)
    if (g.win) {
        SDL_SetWindowResizable(g.win, enable ? SDL_TRUE : SDL_FALSE);
        SDL_SetWindowMinimumSize(g.win, 320 * g_scale / 2, 180 * g_scale / 2);
        if (enable) refit_framebuffer(false);
    }
#endif
}

extern "C" void phx_desktop_set_scale(int scale) {
    phx_sdl_set_window_scale(scale);
#if !defined(PHX_HAVE_GL)
    if (g.win && g_dt.resizable) refit_framebuffer(true);
#endif
}
extern "C" int phx_desktop_scale(void) { return g_scale; }

extern "C" int phx_desktop_fb_size(int* w, int* h) {
    if (w) *w = g.fb.w;
    if (h) *h = g.fb.h;
    return g.fb.w > 0 ? 0 : 1;
}

extern "C" int phx_desktop_display_size(int* w, int* h) {
    SDL_Rect r{ 0, 0, 0, 0 };
    const int di = g.win ? SDL_GetWindowDisplayIndex(g.win) : 0;
    if (SDL_GetDisplayUsableBounds(di < 0 ? 0 : di, &r) != 0) return 0;
    if (w) *w = r.w;
    if (h) *h = r.h;
    return r.w > 0 ? 1 : 0;
}
extern "C" void phx_desktop_set_window_size(int w, int h) {
    if (!g.win || w <= 0 || h <= 0) return;
    SDL_SetWindowSize(g.win, w, h);
    SDL_SetWindowPosition(g.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#if !defined(PHX_HAVE_GL)
    if (g_dt.resizable) refit_framebuffer(true);
#endif
}

extern "C" int phx_desktop_overlay_begin(phx_overlay* out) {
#if defined(PHX_HAVE_GL)
    (void)out;
    return 0;                                                // the GL tier draws text with the GPU
#else
    if (!out || !g.win || !g.ren || g.fb.w <= 0) return 0;
    const int w = g.fb.w * g_scale, h = g.fb.h * g_scale;
    if (!g_ov.px || g_ov.w != w || g_ov.h != h) {            // first use, or a resize / scale change
        overlay_free();
        uint32_t* px = static_cast<uint32_t*>(std::calloc(size_t(w) * size_t(h), sizeof(uint32_t)));
        SDL_Texture* tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!px || !tex) { std::free(px); if (tex) SDL_DestroyTexture(tex); return 0; }
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        g_ov.px = px; g_ov.tex = tex; g_ov.w = w; g_ov.h = h;
    } else {
        std::memset(g_ov.px, 0, size_t(w) * size_t(h) * sizeof(uint32_t));
    }
    g_ov.live = true;
    out->pixels = g_ov.px; out->w = w; out->h = h;
    return 1;
#endif
}

extern "C" void phx_desktop_set_title(const char* utf8) {
    if (g.win && utf8) SDL_SetWindowTitle(g.win, utf8);
}

extern "C" void phx_desktop_set_cursor(int cursor) {
    if (!g.win || cursor < 0 || cursor >= PHX_CURSOR_COUNT || cursor == g_dt.cursor) return;
    static const SDL_SystemCursor kMap[PHX_CURSOR_COUNT] = {
        SDL_SYSTEM_CURSOR_ARROW, SDL_SYSTEM_CURSOR_IBEAM, SDL_SYSTEM_CURSOR_HAND,
        SDL_SYSTEM_CURSOR_CROSSHAIR, SDL_SYSTEM_CURSOR_SIZEWE, SDL_SYSTEM_CURSOR_SIZENS,
        SDL_SYSTEM_CURSOR_SIZEALL,
    };
    if (!g_dt.cursors[cursor]) g_dt.cursors[cursor] = SDL_CreateSystemCursor(kMap[cursor]);
    if (g_dt.cursors[cursor]) SDL_SetCursor(g_dt.cursors[cursor]);
    g_dt.cursor = cursor;
}

extern "C" const char* phx_desktop_clipboard_get(void) {
    if (g_dt.clip) { SDL_free(g_dt.clip); g_dt.clip = nullptr; }
    if (!SDL_HasClipboardText()) return "";
    g_dt.clip = SDL_GetClipboardText();
    return g_dt.clip ? g_dt.clip : "";
}
extern "C" void phx_desktop_clipboard_set(const char* utf8) { SDL_SetClipboardText(utf8 ? utf8 : ""); }

extern "C" const char* phx_desktop_drop_path(void) { return g_dt.drop_path; }

extern "C" int phx_desktop_use_exe_dir(const char* rel) {
    if (!rel || !*rel) return 0;
    struct stat st;
    if (stat(rel, &st) == 0) return 0;                       // found from here: leave it
    char* base = SDL_GetBasePath();                          // "<exe folder>/" (callable before SDL_Init)
    if (!base) return 0;
    char there[1024];
    std::snprintf(there, sizeof(there), "%s%s", base, rel);
    int changed = 0;
#if defined(_WIN32)
    if (stat(there, &st) == 0 && _chdir(base) == 0) changed = 1;
#else
    if (stat(there, &st) == 0 && chdir(base) == 0) changed = 1;
#endif
    SDL_free(base);
    return changed;
}

// Software-tier graphics contract: hand the render backend our CPU framebuffer.
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) {
    return reinterpret_cast<SdlState*>(gfx)->fb;
}

// --- verification readback ----------------------------------------------------------------
// Read the *actually presented* frame back as logical-resolution phx Rgba (R|G<<8|B<<16|A<<24),
// so a headless harness can pixel-diff the real window/GPU output against the software golden
// reference (the same way the PPU/GU backends are verified). Call right after the renderer's
// end_frame(), before present(). Returns 0 on success. The window is g_scale× the logical size,
// so we read the drawable and sample each logical pixel's block centre. Asking for lw x lh equal
// to the window size (framebuffer * phx_desktop_scale()) is the identity: every window pixel, which
// is how a tool captures its native-resolution overlay text.
extern "C" int phx_sdl_readback(uint32_t* out, int lw, int lh) {
    if (!out || lw <= 0 || lh <= 0) return 1;
#if defined(PHX_HAVE_GL)
    int ow = 0, oh = 0;
    SDL_GL_GetDrawableSize(g.win, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 1;
    uint32_t* tmp = static_cast<uint32_t*>(std::malloc(size_t(ow) * size_t(oh) * 4));
    if (!tmp) return 1;
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);  // bytes R,G,B,A == phx Rgba
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            int wx = x * ow / lw + ow / (2 * lw);
            int wy = y * oh / lh + oh / (2 * lh);
            int gy = oh - 1 - wy;                                // glReadPixels origin = bottom-left
            if (wx >= ow) wx = ow - 1;
            if (gy < 0) gy = 0;
            out[y * lw + x] = tmp[gy * ow + wx];
        }
    std::free(tmp);
    return 0;
#else
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(g.ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 1;
    // Compose the current soft framebuffer into the backbuffer exactly as present() does.
    SDL_UpdateTexture(g.tex, nullptr, g.fb.pixels, g.fb.w * int(sizeof(uint32_t)));
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
    overlay_draw();                                          // a tool's native-res layer (text) too
    uint32_t* tmp = static_cast<uint32_t*>(std::malloc(size_t(ow) * size_t(oh) * 4));
    if (!tmp) return 1;
    if (SDL_RenderReadPixels(g.ren, nullptr, SDL_PIXELFORMAT_ABGR8888, tmp, ow * 4) != 0) {
        std::free(tmp); return 1;
    }
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            int sx = x * ow / lw + ow / (2 * lw);
            int sy = y * oh / lh + oh / (2 * lh);                // SDL origin = top-left
            if (sx >= ow) sx = ow - 1;
            if (sy >= oh) sy = oh - 1;
            out[y * lw + x] = tmp[sy * ow + sx];
        }
    std::free(tmp);
    return 0;
#endif
}

// Open a stereo S16 device at `rate` and run `fill` on the audio thread (interleaved L,R).
// Returns 0 on success. The game's fill drains its command queue then mixes — see
// phx/audio/command_queue.h. Stop with phx_sdl_audio_stop().
extern "C" int phx_sdl_audio_start(int rate, phx_audio_fill fill, void* user) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 1;
    SDL_AudioSpec want; SDL_memset(&want, 0, sizeof(want));
    want.freq = rate > 0 ? rate : 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;                       // ~23ms latency at 44.1kHz
    want.callback = sdl_audio_trampoline;
    want.userdata = &g_audio;
    g_audio.fill = fill; g_audio.user = user;
    g_audio.dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (!g_audio.dev) return 1;
    SDL_PauseAudioDevice(g_audio.dev, 0);      // unpause -> the callback starts firing
    return 0;
}
extern "C" void phx_sdl_audio_stop(void) {
    if (g_audio.dev) { SDL_CloseAudioDevice(g_audio.dev); g_audio.dev = 0; }
}

#endif // PHX_HAVE_SDL
