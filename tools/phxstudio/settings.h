// tools/phxstudio/settings.h — Phosphorus Studio's own SETTINGS (File > Settings, Studio tab): the UI
// scale, whether to reopen the last session, and where the console SDKs and emulators live. The
// SDK / emulator paths become environment variables for every launch the Studio runs (DEVKITPRO,
// DEVKITARM, PSPDEV + its bin/ on PATH, MGBA, PPSSPP: what the engine's Makefile reads), so a GBA
// ROM or PSP EBOOT builds without editing the shell's profile. Stored as key=value lines in
// <config>/phxstudio/settings.txt. Host-only, headless (the editors suite covers it).
#ifndef PHX_TOOLS_PHXSTUDIO_SETTINGS_H
#define PHX_TOOLS_PHXSTUDIO_SETTINGS_H

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace phxstudio {

struct StudioSettings {
    int  scale = 0;                          // UI scale (window pixels per canvas pixel); 0 = automatic
    bool restore_session = true;             // reopen the last session's documents
    std::string devkitpro, devkitarm, pspdev, mgba, ppsspp;

    // The environment variables the paths map to (name, value), in the order they apply.
    std::vector<std::pair<std::string, std::string>> env() const {
        std::vector<std::pair<std::string, std::string>> e;
        if (!devkitpro.empty()) e.push_back({ "DEVKITPRO", devkitpro });
        if (!devkitarm.empty()) e.push_back({ "DEVKITARM", devkitarm });
        if (!pspdev.empty())    e.push_back({ "PSPDEV", pspdev });
        if (!mgba.empty())      e.push_back({ "MGBA", mgba });
        if (!ppsspp.empty())    e.push_back({ "PPSSPP", ppsspp });
        return e;
    }

    std::string to_text() const {
        std::string o = "# Phosphorus Studio settings (File > Settings)\n";
        o += "scale=" + std::to_string(scale) + "\n";
        o += std::string("restore_session=") + (restore_session ? "1" : "0") + "\n";
        for (const auto& kv : env()) o += kv.first + "=" + kv.second + "\n";
        return o;
    }
    static StudioSettings parse(const std::string& text) {
        StudioSettings s;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(p, e - p);
            p = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "scale") s.scale = std::max(0, std::min(8, std::atoi(v.c_str())));
            else if (k == "restore_session") s.restore_session = v != "0";
            else if (k == "DEVKITPRO") s.devkitpro = v;
            else if (k == "DEVKITARM") s.devkitarm = v;
            else if (k == "PSPDEV") s.pspdev = v;
            else if (k == "MGBA") s.mgba = v;
            else if (k == "PPSSPP") s.ppsspp = v;
        }
        return s;
    }

    // Put the paths into this process's environment (the launches' shells inherit it). PSPDEV's
    // bin/ goes on PATH once, so `psp-g++` resolves for the PSP launches' requirement check too.
    void apply_env() const {
        for (const auto& kv : env()) set_env(kv.first, kv.second);
        if (!pspdev.empty()) {
            const char* path = std::getenv("PATH");
            const std::string bin = pspdev + "/bin";
            const std::string cur = path ? path : "";
            if (cur.find(bin) == std::string::npos) {
#if defined(_WIN32)
                set_env("PATH", bin + ";" + cur);
#else
                set_env("PATH", bin + ":" + cur);
#endif
            }
        }
    }
    static void set_env(const std::string& k, const std::string& v) {
#if defined(_WIN32)
        _putenv_s(k.c_str(), v.c_str());
#else
        setenv(k.c_str(), v.c_str(), 1);
#endif
    }

    // <config>/phxstudio/settings.txt ("" when there is no home to keep it in).
    static std::string path() {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        const char* appdata = std::getenv("APPDATA");
        if (xdg && *xdg) return std::string(xdg) + "/phxstudio/settings.txt";
        if (home && *home) return std::string(home) + "/.config/phxstudio/settings.txt";
        if (appdata && *appdata) return std::string(appdata) + "/phxstudio/settings.txt";
        return "";
    }
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_SETTINGS_H
