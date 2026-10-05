#!/usr/bin/env python3
"""tools/common/bake_project.py — bake a Phosphorus GAME PROJECT's assets into one .phxp bundle.

Reads <project>/phxproject.json (its "assets" folders, default ["assets"]) and runs the SAME
converters `make check` covers on every author file it finds, then the assembler:

    .sprdef / sprite .json  -> phxsprite  (the sheet PNG rides along inside the sprite)
    .font / .fnt            -> phxsprite  (a font: its sheet PNG + the glyph table)
    .tmj                    -> phxtile
    .wav / .sfx / .song     -> phxsnd     (.sfx: a sound effect's parameters, .song: a tracker song)
    phxbin table .json      -> phxbin     (+ a generated header in build/gen/<name>.gen.h)
    .dlg                    -> baked directly by phxpack (conversations: no other files to follow)
    any other .png          -> baked directly by phxpack (tilesets, portraits, UI art)

Output: <project>/build/<slug>.phxp (tier 2, PC) or <slug>.t<N>.phxp for --tier 0/1. Asset names
are the file stems ("hero"_hash is assets/hero.sprdef). Used by `make game-assets PROJECT=...`.
Exit status is non-zero on the first converter failure (its message is printed as-is).
"""
import argparse
import json
import os
import re
import subprocess
import sys


def slug(name):
    s = re.sub(r"[^a-z0-9_-]", "", name.lower().replace(" ", "_"))
    return s


def main():
    ap = argparse.ArgumentParser(description="Bake a Phosphorus game project's assets into one .phxp bundle.")
    ap.add_argument("project", help="the project folder (holding phxproject.json)")
    ap.add_argument("--tools", required=True, help="folder with the built phxsprite/phxtile/phxsnd/phxbin/phxpack")
    ap.add_argument("--tier", type=int, default=2, choices=(0, 1, 2), help="0 GBA, 1 PSP, 2 PC (default)")
    args = ap.parse_args()

    proj = os.path.abspath(args.project)
    try:
        with open(os.path.join(proj, "phxproject.json"), encoding="utf-8") as f:
            meta = json.load(f)
    except (OSError, ValueError) as e:
        print(f"bake_project: {proj}/phxproject.json: {e}", file=sys.stderr)
        return 1
    name = slug(meta.get("name") or os.path.basename(proj)) or "game"
    folders = meta.get("assets") or ["assets"]

    files = []
    for folder in folders:
        root = os.path.join(proj, folder)
        for dirpath, dirnames, names in os.walk(root):
            dirnames.sort()
            for n in sorted(names):
                files.append(os.path.join(dirpath, n))

    build = os.path.join(proj, "build")
    inter = os.path.join(build, "intermediate", f"t{args.tier}")
    gen = os.path.join(build, "gen")
    os.makedirs(inter, exist_ok=True)
    os.makedirs(gen, exist_ok=True)
    tool = lambda t: os.path.join(os.path.abspath(args.tools), t)
    tier = ["--target", str(args.tier)]

    def run(cmd):
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print(f"bake_project: FAILED: {' '.join(cmd)}", file=sys.stderr)
            sys.exit(1)

    pack_inputs, sheets, pngs = [], set(), []
    for p in files:
        stem, ext = os.path.splitext(os.path.basename(p))
        ext = ext.lower()
        if ext == ".sprdef":
            with open(p, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r"\s*sheet\s+(\S+)", line)
                    if m:
                        sheets.add(os.path.normpath(os.path.join(os.path.dirname(p), m.group(1))))
            out = os.path.join(inter, stem + ".phxspr")
            run([tool("phxsprite"), "--out", out, "--name", stem] + tier + [p])
            pack_inputs.append(out)
        elif ext in (".font", ".fnt"):
            with open(p, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if ext == ".font":
                try:
                    img = json.loads(text).get("image", "")
                except ValueError:
                    img = ""
            else:
                m = re.search(r'page\s+id=0\s+file="([^"]+)"', text)
                img = m.group(1) if m else ""
            if img:
                sheets.add(os.path.normpath(os.path.join(os.path.dirname(p), img)))
            out = os.path.join(inter, stem + ".phxspr")
            run([tool("phxsprite"), "--out", out, "--name", stem] + tier + [p])
            pack_inputs.append(out)
        elif ext == ".json":
            try:
                with open(p, encoding="utf-8") as f:
                    doc = json.load(f)
            except ValueError as e:
                print(f"bake_project: skipping {p}: {e}", file=sys.stderr)
                continue
            if isinstance(doc, dict) and "animations" in doc and "image" in doc:
                sheets.add(os.path.normpath(os.path.join(os.path.dirname(p), doc["image"])))
                out = os.path.join(inter, stem + ".phxspr")
                run([tool("phxsprite"), "--out", out, "--name", stem] + tier + [p])
                pack_inputs.append(out)
            elif isinstance(doc, dict) and "fields" in doc and "records" in doc:
                out = os.path.join(inter, stem + ".phxbin")
                run([tool("phxbin"), "--out", out, "--name", stem, "--header",
                     os.path.join(gen, stem + ".gen.h")] + tier + [p])
                pack_inputs.append(out)
        elif ext == ".tmj":
            out = os.path.join(inter, stem + ".phxtmap")
            run([tool("phxtile"), "--out", out, "--name", stem] + tier + [p])
            pack_inputs.append(out)
        elif ext in (".wav", ".sfx", ".song"):
            out = os.path.join(inter, stem + ".phxsnd")
            run([tool("phxsnd"), "--out", out, "--name", stem] + tier + [p])
            pack_inputs.append(out)
        elif ext == ".dlg":
            pack_inputs.append(p)
        elif ext == ".png":
            pngs.append(p)

    # PNGs a sprite or font already carries as its sheet are not baked twice
    pack_inputs += [p for p in pngs if os.path.normpath(p) not in sheets]
    if not pack_inputs:
        print(f"bake_project: nothing to bake in {', '.join(folders)}", file=sys.stderr)
        return 1
    bundle = os.path.join(build, name + (".phxp" if args.tier == 2 else f".t{args.tier}.phxp"))
    run([tool("phxpack"), "--out", bundle] + tier + pack_inputs)
    print(f"bake_project: {len(pack_inputs)} inputs -> {os.path.relpath(bundle, proj)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
