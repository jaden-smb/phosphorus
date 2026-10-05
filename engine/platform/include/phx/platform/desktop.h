/* phx/platform/desktop.h — the DESKTOP-ONLY extension of the platform seam, for tools.
 *
 * The game-facing seam (platform.h: phx_input_raw, 12 canonical buttons, one pointer button)
 * is deliberately console-shaped and stays untouched. Editors need more: a key-event stream
 * with modifiers, typed text, right/middle buttons, the wheel, a clipboard, a resizable
 * window, and the chance to veto a window close ("save your changes?"). Those live HERE, as
 * extra extern "C" symbols exported ONLY by the desktop-capable backends — the same precedent
 * as phx_sdl_audio_start / phx_sdl_readback (docs/gui-editor-feasibility.md, Phase 1):
 *
 *   - sdl   (engine/platform/src/sdl/)  : the real thing, over SDL2 events.
 *   - null  (engine/platform/src/null/) : a SCRIPTED event queue (phx_null_desktop_push), so
 *                                         tool logic is testable headlessly, like the pointer.
 *   - gba / psp                          : nothing. No console backend links a tool, so no
 *                                         game ROM changes by a single byte.
 *
 * No SDL type leaks through this header; key codes are Phosphorus's own (printable keys are
 * their lower-case ASCII code, so shortcuts are layout-aware: Ctrl+Z is the key LABELLED Z).
 * Gameplay code must never include this — it is for host tools (tools/) only. */
#ifndef PHX_PLATFORM_DESKTOP_H
#define PHX_PLATFORM_DESKTOP_H

#include "phx/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- keys: printable keys = lower-case ASCII (32..126); named keys from 256 ---- */
typedef enum phx_key {
    PHX_KEY_NONE = 0,
    PHX_KEY_ESCAPE = 256, PHX_KEY_ENTER, PHX_KEY_TAB, PHX_KEY_BACKSPACE, PHX_KEY_DELETE,
    PHX_KEY_INSERT, PHX_KEY_LEFT, PHX_KEY_RIGHT, PHX_KEY_UP, PHX_KEY_DOWN, PHX_KEY_HOME,
    PHX_KEY_END, PHX_KEY_PAGE_UP, PHX_KEY_PAGE_DOWN,
    PHX_KEY_F1, PHX_KEY_F2, PHX_KEY_F3, PHX_KEY_F4, PHX_KEY_F5, PHX_KEY_F6,
    PHX_KEY_F7, PHX_KEY_F8, PHX_KEY_F9, PHX_KEY_F10, PHX_KEY_F11, PHX_KEY_F12,
    PHX_KEY_SHIFT, PHX_KEY_CTRL, PHX_KEY_ALT, PHX_KEY_GUI     /* the modifier keys themselves */
} phx_key;

typedef enum phx_key_mod {
    PHX_MOD_SHIFT = 1, PHX_MOD_CTRL = 2, PHX_MOD_ALT = 4, PHX_MOD_GUI = 8  /* GUI = Cmd/Super */
} phx_key_mod;

typedef enum phx_mouse_button {
    PHX_MOUSE_LEFT = 1, PHX_MOUSE_MIDDLE = 2, PHX_MOUSE_RIGHT = 3
} phx_mouse_button;

typedef enum phx_desktop_ev_kind {
    PHX_DEV_NONE = 0,
    PHX_DEV_KEY_DOWN,     /* key, mods, repeat (1 = OS auto-repeat)                            */
    PHX_DEV_KEY_UP,       /* key, mods                                                         */
    PHX_DEV_TEXT,         /* text: UTF-8 typed text (only while text input is enabled)         */
    PHX_DEV_MOUSE_DOWN,   /* button, x, y (framebuffer coords), clicks (2 = double-click)      */
    PHX_DEV_MOUSE_UP,     /* button, x, y                                                      */
    PHX_DEV_WHEEL,        /* wheel_x, wheel_y (+y = away from the user / scroll up), x, y      */
    PHX_DEV_RESIZE,       /* x, y = the NEW framebuffer size (logical pixels)                  */
    PHX_DEV_DROP_FILE,    /* path: phx_desktop_drop_path() until the next drop                 */
    PHX_DEV_QUIT          /* the window close was vetoed (confirm-quit mode); the tool decides */
} phx_desktop_ev_kind;

typedef struct phx_desktop_event {
    uint8_t  kind;        /* phx_desktop_ev_kind */
    uint8_t  button;      /* phx_mouse_button */
    uint8_t  clicks;      /* consecutive clicks (mouse down) */
    uint8_t  repeat;      /* key auto-repeat */
    uint16_t mods;        /* phx_key_mod bitmask at the time of the event */
    int32_t  key;         /* phx_key (or lower-case ASCII) */
    int16_t  x, y;
    int16_t  wheel_x, wheel_y;
    char     text[32];    /* NUL-terminated UTF-8 (PHX_DEV_TEXT) */
} phx_desktop_event;

typedef enum phx_cursor {
    PHX_CURSOR_ARROW = 0, PHX_CURSOR_IBEAM, PHX_CURSOR_HAND, PHX_CURSOR_CROSSHAIR,
    PHX_CURSOR_SIZE_WE, PHX_CURSOR_SIZE_NS, PHX_CURSOR_SIZE_ALL, PHX_CURSOR_COUNT
} phx_cursor;

/* 1 when the linked backend implements this extension with a real window (sdl), 0 for the
 * scripted null queue. Tools may use it to skip window-only niceties in headless runs. */
int  phx_desktop_available(void);

/* Pop the oldest pending event into *out. Returns 1 if one was written, 0 when the queue is
 * empty. Events accumulate across pump_events() calls until drained (bounded ring: when it
 * overflows, the OLDEST events are dropped — a tool that stops draining loses history, never
 * memory). Drain once per frame after the App has pumped. */
int  phx_desktop_poll(phx_desktop_event* out);

/* Current pointer position (framebuffer coords, -1 when outside) and held buttons as a bitmask
 * of (1 << phx_mouse_button). Unlike phx_input_raw this includes right and middle. */
void phx_desktop_mouse(int* x, int* y, uint32_t* buttons);
uint16_t phx_desktop_mods(void);             /* modifiers held right now */

/* Esc quits the window by default (games rely on it). Tools that give Esc a meaning (close a
 * dialog, cancel a drag) turn that off. */
void phx_desktop_set_quit_on_escape(int enable);
/* With confirm-quit on, closing the window does NOT stop the app: a PHX_DEV_QUIT event is
 * queued instead and the tool asks "save changes?", then calls App::request_quit() itself. */
void phx_desktop_set_confirm_quit(int enable);

/* Deliver PHX_DEV_TEXT events (and show an IME where the OS has one). Off by default. */
void phx_desktop_text_input(int enable);

/* Resizable window: the framebuffer follows the window at the current integer UI scale, and a
 * PHX_DEV_RESIZE event reports the new logical size. The software renderer re-locks the
 * framebuffer every frame, so nothing else needs to know. */
void phx_desktop_set_resizable(int enable);
/* Integer UI scale (1..6): window pixels per framebuffer pixel. Before init it sizes the window;
 * after init (resizable mode) it re-derives the framebuffer from the current window size. */
void phx_desktop_set_scale(int scale);
int  phx_desktop_scale(void);
/* Current framebuffer (logical) size. Returns 0 on success. */
int  phx_desktop_fb_size(int* w, int* h);

/* Native-resolution overlay: an RGBA8 layer with ONE PIXEL PER WINDOW PIXEL over the framebuffer
 * area (framebuffer size x the integer UI scale), alpha-blended over the upscaled framebuffer at
 * present(). It lets a tool draw smooth, anti-aliased text (Phosphorus Studio's TrueType text) on top
 * of a canvas that is otherwise nearest-neighbour upscaled and alpha-tested. Pixels are straight
 * (non-premultiplied) alpha, R | G<<8 | B<<16 | A<<24 like phx::Rgba.
 *
 * phx_desktop_overlay_begin() (call once per frame, before drawing into it) clears the layer to
 * transparent, fills *out and returns 1; the layer is shown at the next present() and dropped after
 * it, so a frame that does not call it has no overlay. It returns 0 when the backend has no such
 * layer (the GL render tier, no window/framebuffer): the tool then draws its text with its bitmap
 * font instead. The null backend keeps the layer in memory (never presented) so tests can read it
 * back (phx_null_overlay_peek). Games never call it. */
typedef struct phx_overlay {
    uint32_t* pixels;   /* w*h, straight-alpha RGBA8, row-major, valid until the next begin/present */
    int32_t   w;        /* framebuffer width  * phx_desktop_scale() */
    int32_t   h;        /* framebuffer height * phx_desktop_scale() */
} phx_overlay;
int  phx_desktop_overlay_begin(phx_overlay* out);

/* The usable area of the display the window is on (window pixels; 0 on failure/headless), and
 * a new window size in window pixels (re-centred; the framebuffer follows in resizable mode). */
int  phx_desktop_display_size(int* w, int* h);
void phx_desktop_set_window_size(int w, int h);

void phx_desktop_set_title(const char* utf8);
void phx_desktop_set_cursor(int cursor);     /* phx_cursor */

/* Clipboard (UTF-8). get returns a pointer valid until the next get (empty string if none). */
const char* phx_desktop_clipboard_get(void);
void        phx_desktop_clipboard_set(const char* utf8);

/* The file path of the most recent PHX_DEV_DROP_FILE ("" if none). */
const char* phx_desktop_drop_path(void);

/* A shipped game's folder: when `rel` (a path relative to the running executable's folder) exists
 * there but not in the working directory, make the executable's folder the working directory, so a
 * game started by double-clicking it, or from anywhere else, still finds its build/<name>.phxp.
 * Returns 1 when it changed the working directory. The null backend does nothing (0). */
int phx_desktop_use_exe_dir(const char* rel);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PHX_PLATFORM_DESKTOP_H */
