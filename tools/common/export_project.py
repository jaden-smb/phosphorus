#!/usr/bin/env python3
"""tools/common/export_project.py — package a built Phosphorus GAME PROJECT for players.

`make game-export PROJECT=... EXPORT=pc|gba|psp` builds the game (PC: a release build plus its tier-2
bundle; GBA / PSP: the console build, which embeds its bundle), then runs this to assemble a folder
and a zip under <project>/dist/ that runs without the engine checkout:

    pc   <slug>-windows/ or <slug>-linux/
           <slug>(.exe)              the game (PHX_BUILD_RELEASE)
           build/<slug>.phxp         its assets (the game finds them from its own folder)
           *.dll                     Windows: every DLL it loads from the toolchain (SDL2, the
                                     C++ runtime), found by following the import tables
           README.txt
    gba  <slug>-gba/<slug>.gba + README.txt                       (flash cart / emulator)
    psp  <slug>-psp/PSP/GAME/<SLUG>/EBOOT.PBP + README.txt        (memory stick root / PPSSPP)

Prints the zip's path. Exit status is non-zero when the build output it packages is missing.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile


def die(msg):
    print(f"export: {msg}", file=sys.stderr)
    sys.exit(1)


def project_meta(proj):
    try:
        with open(os.path.join(proj, "phxproject.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        die(f"{proj}/phxproject.json: {e}")


# ---- Windows: the DLLs an executable needs, from the toolchain (not the system) -------------------
SYSTEM_DLL = re.compile(r"^(api-ms-|ext-ms-)|^(kernel32|user32|gdi32|shell32|advapi32|ole32|oleaut32|"
                        r"winmm|imm32|version|setupapi|cfgmgr32|msvcrt|ucrtbase|ws2_32|rpcrt4|comdlg32|"
                        r"dinput8|dxgi|d3d\d*|dsound|hid|opengl32|uxtheme|dwmapi|shlwapi|bcrypt|ntdll|"
                        r"combase|crypt32|secur32|userenv|powrprof|winspool|comctl32|mpr|netapi32|"
                        r"iphlpapi|dbghelp|psapi|sechost|msvcp_win|gdiplus|xinput\d.*)\.dll$", re.I)


def dll_imports(path, objdump):
    try:
        out = subprocess.run([objdump, "-p", path], capture_output=True, text=True, errors="replace").stdout
    except OSError:
        return []
    return re.findall(r"DLL Name:\s*(\S+)", out)


def toolchain_dirs():
    dirs = []
    for tool in ("g++", "gcc", "sdl2-config"):
        p = shutil.which(tool)
        if p:
            dirs.append(os.path.dirname(os.path.realpath(p)))
    try:
        prefix = subprocess.run(["sdl2-config", "--prefix"], capture_output=True, text=True).stdout.strip()
        if prefix:
            dirs.append(os.path.join(prefix, "bin"))
    except OSError:
        pass
    seen, out = set(), []
    for d in dirs:
        d = os.path.normcase(os.path.abspath(d))
        if d not in seen and os.path.isdir(d):
            seen.add(d)
            out.append(d)
    return out


def copy_dlls(exe, dest):
    objdump = shutil.which("objdump")
    if not objdump:
        print("export: warning: no objdump on PATH - copy the game's DLLs (SDL2.dll, the C++ runtime) by hand",
              file=sys.stderr)
        return []
    dirs = toolchain_dirs()
    todo, done, copied = [exe], set(), []
    while todo:
        for name in dll_imports(todo.pop(), objdump):
            key = name.lower()
            if key in done or SYSTEM_DLL.match(name):
                continue
            done.add(key)
            for d in dirs:
                src = os.path.join(d, name)
                if os.path.isfile(src):
                    shutil.copy2(src, os.path.join(dest, name))
                    copied.append(name)
                    todo.append(src)
                    break
    return copied


# ---- the README a player gets --------------------------------------------------------------------
def readme(title, target, slug):
    lines = [title, "=" * len(title), "", "Made with the Phosphorus engine.", ""]
    if target == "pc":
        exe = slug + (".exe" if os.name == "nt" else "")
        lines += ["HOW TO PLAY", f"  Run {exe}. Keep the build folder next to it: it holds the game's assets.", ""]
        if os.name != "nt":
            lines += ["  Needs SDL2 (e.g. `sudo apt install libsdl2-2.0-0`).", ""]
        lines += ["CONTROLS (keyboard / gamepad)",
                  "  Arrows or WASD   move              Z or Space   A (jump, confirm)",
                  "  X                B                  Enter        Start (pause)",
                  "  Esc              quit", ""]
    elif target == "gba":
        lines += ["HOW TO PLAY", f"  {slug}.gba is a Game Boy Advance ROM: copy it to a flash cart, or open it",
                  "  in an emulator (mGBA).", "",
                  "CONTROLS", "  D-pad move   A jump / confirm   Start pause", ""]
    else:
        lines += ["HOW TO PLAY", "  Copy the PSP folder to the root of the memory stick (it merges with the",
                  "  PSP/GAME folder there), or open PSP/GAME/*/EBOOT.PBP in PPSSPP.", "",
                  "CONTROLS", "  D-pad move   Cross jump / confirm   Start pause", ""]
    return "\n".join(lines)


def zip_folder(folder):
    zpath = folder + ".zip"
    base = os.path.dirname(folder)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(folder):
            for n in sorted(files):
                full = os.path.join(root, n)
                z.write(full, os.path.relpath(full, base))
    return zpath


def main():
    ap = argparse.ArgumentParser(description="Package a built Phosphorus game project for players.")
    ap.add_argument("project")
    ap.add_argument("--target", choices=("pc", "gba", "psp"), default="pc")
    ap.add_argument("--name", required=True, help="the project's slug (the build's file names)")
    args = ap.parse_args()

    proj = os.path.abspath(args.project)
    meta = project_meta(proj)
    title = (meta.get("name") or args.name).strip() or args.name
    build = os.path.join(proj, "build")
    plat = {"pc": "windows" if os.name == "nt" else "linux", "gba": "gba", "psp": "psp"}[args.target]
    out = os.path.join(proj, "dist", f"{args.name}-{plat}")
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    if args.target == "pc":
        exe = os.path.join(build, args.name + (".exe" if os.name == "nt" else ""))
        bundle = os.path.join(build, args.name + ".phxp")
        if not os.path.isfile(exe):
            die(f"no game at {exe} (the build failed?)")
        if not os.path.isfile(bundle):
            die(f"no bundle at {bundle} (bake the assets first)")
        shutil.copy2(exe, out)
        os.makedirs(os.path.join(out, "build"))
        shutil.copy2(bundle, os.path.join(out, "build"))
        if os.name == "nt":
            dlls = copy_dlls(exe, out)
            if dlls:
                print(f"export: + {', '.join(dlls)}")
    elif args.target == "gba":
        rom = os.path.join(build, args.name + ".gba")
        if not os.path.isfile(rom):
            die(f"no ROM at {rom} (the GBA build failed?)")
        shutil.copy2(rom, out)
    else:
        eboot = os.path.join(build, "psp", "EBOOT.PBP")
        if not os.path.isfile(eboot):
            die(f"no EBOOT at {eboot} (the PSP build failed?)")
        game_dir = os.path.join(out, "PSP", "GAME", re.sub(r"[^A-Z0-9_]", "", args.name.upper()) or "GAME")
        os.makedirs(game_dir)
        shutil.copy2(eboot, game_dir)

    with open(os.path.join(out, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(readme(title, args.target, args.name))
    zpath = zip_folder(out)
    size = os.path.getsize(zpath)
    print(f"export: {os.path.relpath(out, proj)}/ and {os.path.relpath(zpath, proj)} ({size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
