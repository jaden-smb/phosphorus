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
All files in declared asset folders are required sources. Unsupported formats, discovery,
conversion, identifier and assembly failures return nonzero with resource diagnostics.
Failed bakes invalidate the tier bundle/lock and generated data; successful bundles are
published only after assembly validates identifiers and texture references.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from project_config import load_project


class BakeError(Exception):
    pass


def main():
    ap = argparse.ArgumentParser(description="Bake a Phosphorus game project's assets into one .phxp bundle.")
    ap.add_argument("project", help="the project folder (holding phxproject.json)")
    ap.add_argument("--tools", required=True, help="folder with the built converters and phxpack/phxnew")
    ap.add_argument("--tier", type=int, default=2, choices=(0, 1, 2))
    args = ap.parse_args()
    proj = os.path.abspath(args.project)
    tool = lambda t: os.path.join(os.path.abspath(args.tools), t)
    build = os.path.join(proj, "build")
    inter = os.path.join(build, "intermediate", f"t{args.tier}")
    gen = os.path.join(build, "gen")
    bundle = None
    pending = None
    sources = {}
    receipt = os.path.join(build, f".bake-output-t{args.tier}")

    def fail(path, kind, why):
        raise BakeError(f"{path} [{kind}]: {why}")

    def run(cmd, path, kind):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        except OSError as e:
            fail(path, kind, f"cannot run {cmd[0]}: {e}")
        if r.returncode:
            for baked, author in sources.items():
                r.stderr = r.stderr.replace(baked, author)
            fail(path, kind, f"{os.path.basename(cmd[0])} exited {r.returncode}: "
                 + (r.stderr.strip() or r.stdout.strip() or "no diagnostic"))
        if r.stdout:
            print(r.stdout, end="")
        if r.stderr:
            print(r.stderr, end="", file=sys.stderr)

    def remove_outputs():
        for base in (bundle, pending):
            if base:
                for suffix in ("", ".lock", ".manifest.txt"):
                    try:
                        os.unlink(base + suffix)
                    except FileNotFoundError:
                        pass

    try:
        # Remember only our own output name, so even broken metadata invalidates the
        # previous bake without deleting unrelated bundles in the project.
        if os.path.isfile(receipt):
            with open(receipt, encoding="utf-8") as f:
                previous = f.read().strip()
            suffix = ".phxp" if args.tier == 2 else f".t{args.tier}.phxp"
            if previous != os.path.basename(previous) or not previous.endswith(suffix):
                fail(receipt, "Bake output record", "invalid bundle filename")
            bundle = os.path.join(build, previous)
            pending = bundle + ".pending"
            remove_outputs()
        try:
            meta = load_project(proj, tool("phxnew"))
        except (OSError, ValueError, subprocess.CalledProcessError) as e:
            fail(os.path.join(proj, "phxproject.json"), "Project", getattr(e, "stderr", "") or str(e))
        if meta["build"] != "generic":
            fail(os.path.join(proj, "phxproject.json"), "Project", "custom-build project; use its configured launches")
        bundle = os.path.join(build, meta["id"] + (".phxp" if args.tier == 2 else f".t{args.tier}.phxp"))
        pending = bundle + ".pending"
        # Invalidate the previous success BEFORE discovery or conversion can fail.
        remove_outputs()
        os.makedirs(build, exist_ok=True)
        with open(receipt, "w", encoding="utf-8") as f:
            f.write(os.path.basename(bundle))
        for directory in (inter, gen):
            if os.path.isdir(directory):
                shutil.rmtree(directory)
            os.makedirs(directory, exist_ok=True)

        files = []
        for folder in meta["assets"]:
            root = os.path.join(proj, folder)
            if not os.path.isdir(root):
                fail(root, "Asset directory", "missing or not a directory")
            def walk_error(e):
                fail(e.filename, "Asset directory", str(e))
            for dirpath, dirnames, names in os.walk(root, onerror=walk_error):
                dirnames.sort()
                for d in dirnames:
                    if os.path.islink(os.path.join(dirpath, d)):
                        fail(os.path.join(dirpath, d), "Asset directory", "symlink directories are not traversed; declare the real directory")
                files.extend(os.path.join(dirpath, n) for n in sorted(names))

        inputs, identifiers = [], {}
        tier = ["--target", str(args.tier)]
        # JSON is inspected only to select a converter. All schema/source validation is
        # performed by the shared C++ builders used by the converters and Studio.
        for p in files:
            stem, ext = os.path.splitext(os.path.basename(p))
            ext = ext.lower()
            kind, converter, output_ext = {
                ".sprdef": ("Sprite", "phxsprite", ".phxspr"),
                ".font": ("Font", "phxsprite", ".phxspr"),
                ".fnt": ("Font", "phxsprite", ".phxspr"),
                ".tmj": ("Tilemap", "phxtile", ".phxtmap"),
                ".wav": ("Sound", "phxsnd", ".phxsnd"),
                ".sfx": ("Sound", "phxsnd", ".phxsnd"),
                ".song": ("Sound", "phxsnd", ".phxsnd"),
                ".dlg": ("Dialogue", None, None),
                ".png": ("Texture", None, None),
                ".ppm": ("Texture", None, None),
                ".tmcsv": ("Tilemap", None, None),
            }.get(ext, (None, None, None))
            if ext == ".json":
                try:
                    with open(p, encoding="utf-8") as f:
                        doc = json.load(f)
                except (OSError, ValueError) as e:
                    fail(p, "JSON asset (Sprite/Blob)", str(e))
                if isinstance(doc, dict) and any(k in doc for k in ("struct", "fields", "records")):
                    kind, converter, output_ext = "Blob", "phxbin", ".phxbin"
                elif isinstance(doc, dict) and any(k in doc for k in ("animations", "image")):
                    kind, converter, output_ext = "Sprite", "phxsprite", ".phxspr"
                else:
                    fail(p, "JSON asset", "unsupported schema; expected a sprite or phxbin table")
            if not kind:
                fail(p, ext or "Asset", "unsupported source format")
            key = (kind, stem)
            if key in identifiers:
                fail(p, kind, f"duplicate asset identifier '{stem}'; also defined by {identifiers[key]}")
            identifiers[key] = p
            if converter:
                # Type subdirectories prevent Font/Sprite intermediate-name collisions.
                directory = os.path.join(inter, kind)
                os.makedirs(directory, exist_ok=True)
                out = os.path.join(directory, stem + output_ext)
                cmd = [tool(converter), "--out", out, "--name", stem] + tier + [p]
                if kind == "Blob":
                    cmd += ["--header", os.path.join(gen, stem + ".gen.h")]
                run(cmd, p, kind)
                if not os.path.isfile(out) or not os.path.getsize(out):
                    fail(p, kind, f"converter produced no output: {out}")
                if kind == "Blob":
                    header = os.path.join(gen, stem + ".gen.h")
                    if not os.path.isfile(header) or not os.path.getsize(header):
                        fail(p, kind, f"converter produced no header: {header}")
                sources[out] = f"{p} [{kind}]"
                inputs.append(out)
            else:
                sources[p] = f"{p} [{kind}]"
                inputs.append(p)
        if not inputs:
            fail(proj, "Project assets", "nothing to bake in " + ", ".join(meta["assets"]))
        # Sheets are baked by their own validators too. The assembler coalesces identical
        # Texture payloads carried by sprites/fonts; conflicting definitions are errors.
        run([tool("phxpack"), "--out", pending, "--full", "--validate-references"] + tier + inputs,
            proj, "Project bundle")
        if not os.path.isfile(pending) or not os.path.isfile(pending + ".lock"):
            fail(pending, "Project bundle", "assembler produced no bundle/lock")
        os.replace(pending + ".lock", bundle + ".lock")
        os.replace(pending, bundle)
        print(f"bake_project: {len(inputs)} inputs -> {os.path.relpath(bundle, proj)}")
        return 0
    except (BakeError, OSError) as e:
        diagnostic = f"{e.filename or proj} [Bake I/O]: {e}" if isinstance(e, OSError) else str(e)
        print(f"bake_project: {diagnostic}", file=sys.stderr)
        try:
            remove_outputs()
            for directory in (inter, gen):
                if bundle and os.path.isdir(directory):
                    shutil.rmtree(directory)
        except OSError as cleanup:
            print(f"bake_project: cleanup failed: {cleanup}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
