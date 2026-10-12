#!/usr/bin/env python3
"""Real converter/assembler integration tests for generic project baking (G4)."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = Path(__file__).with_name("bake_project.py").resolve()
parser = argparse.ArgumentParser()
parser.add_argument("--tools", required=True)
parser.add_argument("--tool", action="append", default=[], help="CMake tool override: name=path")
args, remaining = parser.parse_known_args()
TOOLS = Path(args.tools).resolve()


class ProjectBake(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="phx-bake-test-")
        cls.root = Path(cls.tmp.name)
        if args.tool:
            global TOOLS
            TOOLS = cls.root / "tools"
            TOOLS.mkdir()
            for override in args.tool:
                name, path = override.split("=", 1)
                shutil.copy2(path, TOOLS / name)
        cls.template = cls.root / "template"
        result = subprocess.run([str(TOOLS / "phxnew"), str(cls.template), "Bake Test"], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.project = self.root / self._testMethodName
        shutil.copytree(self.template, self.project)
        self.assets = self.project / "assets"
        self.bundle = self.project / "build/bake_test.phxp"

    def bake(self, tier=2, tools=None):
        tools = TOOLS if tools is None else tools
        return subprocess.run([sys.executable, str(SCRIPT), str(self.project), "--tools", str(tools), "--tier", str(tier)],
                              capture_output=True, text=True)

    def bad(self, path, reason, kind=None):
        result = self.bake()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(str(path), result.stderr)
        self.assertIn(reason, result.stderr)
        if kind:
            self.assertIn(kind, result.stderr)
        self.assertNotIn("bake_project:", result.stdout)
        self.assertFalse(self.bundle.exists())
        self.assertFalse(Path(str(self.bundle) + ".lock").exists())
        self.assertFalse(Path(str(self.bundle) + ".pending").exists())
        self.assertFalse((self.project / "build/intermediate/t2").exists())
        self.assertFalse((self.project / "build/gen").exists())
        return result

    def good(self, tier=2):
        result = self.bake(tier)
        self.assertEqual(result.returncode, 0, result.stderr)
        bundle = self.project / "build" / ("bake_test.phxp" if tier == 2 else f"bake_test.t{tier}.phxp")
        data = bundle.read_bytes()
        magic, version, target, flags, count, toc, total, crc = struct.unpack_from("<IHBBIIII", data)
        self.assertEqual(magic, 0x50584850)
        self.assertEqual(version, 2)
        self.assertEqual(target, tier)
        self.assertEqual(total, len(data))
        self.assertEqual(crc, zlib.crc32(data[toc:]))
        keys = []
        for i in range(count):
            name, kind, _, offset, size, usize = struct.unpack_from("<IHHIII", data, toc + i * 20)
            self.assertLessEqual(offset + size, len(data))
            self.assertEqual(size, usize)
            keys.append((name, kind))
        self.assertEqual(keys, sorted(keys))
        self.assertEqual(len(keys), len(set(keys)))
        self.assertTrue(Path(str(bundle) + ".lock").is_file())
        self.assertTrue((self.project / "build/gen/flow.gen.h").is_file())
        return data

    def test_valid_template_all_tiers_deterministic(self):
        for tier in (2, 0, 1):
            self.assertEqual(self.good(tier), self.good(tier))

    def test_malformed_flow_removes_previous_success(self):
        self.good()
        self.good(0)
        path = self.assets / "flow.json"
        path.write_text('{"fields": [')
        self.bad(path, "Expecting", "JSON asset")
        self.assertTrue((self.project / "build/bake_test.t0.phxp").exists())

    def test_broken_metadata_removes_previous_success(self):
        self.good()
        path = self.project / "phxproject.json"
        path.write_text('{')
        self.bad(path, "Project")

    def test_missing_asset_directory(self):
        path = self.assets / "missing"
        meta = json.loads((self.project / "phxproject.json").read_text())
        meta["assets"].append("assets/missing")
        (self.project / "phxproject.json").write_text(json.dumps(meta))
        self.bad(path, "missing or not a directory", "Asset directory")

    def test_missing_sheet(self):
        path = self.assets / "hero.png"
        path.unlink()
        self.bad(self.assets / "hero.sprdef", "hero.png", "Sprite")

    def test_unreadable_source(self):
        # A dangling symlink reliably exercises read failure even when tests run as root.
        path = self.assets / "flow.json"
        path.unlink()
        path.symlink_to(self.assets / "missing.json")
        self.bad(path, "No such file", "JSON asset")

    def test_unreadable_directory(self):
        path = self.assets / "unreadable"
        path.mkdir()
        path.chmod(0)
        try:
            import os
            if os.geteuid() == 0:
                self.skipTest("root can read mode-000 directories")
            self.bad(path, "Permission denied", "Asset directory")
        finally:
            path.chmod(0o700)

    def test_symlink_directory_not_silently_skipped(self):
        path = self.assets / "alias"
        path.symlink_to(self.assets, target_is_directory=True)
        self.bad(path, "symlink directories", "Asset directory")

    def test_duplicate_stems(self):
        path = self.assets / "nested/flow.json"
        path.parent.mkdir()
        shutil.copyfile(self.assets / "flow.json", path)
        result = self.bad(path, "duplicate asset identifier 'flow'", "Blob")
        self.assertIn(str(self.assets / "flow.json"), result.stderr)

    def test_same_type_different_extensions_collide(self):
        path = self.assets / "hero.json"
        path.write_text(json.dumps({"image": "hero.png", "tile": 8, "animations": {}}))
        self.bad(path, "duplicate asset identifier 'hero'", "Sprite")

    def test_identical_texture_stems_still_collide(self):
        path = self.assets / "nested/hero.png"
        path.parent.mkdir()
        shutil.copyfile(self.assets / "hero.png", path)
        self.bad(path, "duplicate asset identifier 'hero'", "Texture")

    def test_overlapping_roots_collide(self):
        meta = json.loads((self.project / "phxproject.json").read_text())
        meta["assets"].append("assets")
        (self.project / "phxproject.json").write_text(json.dumps(meta))
        result = self.bake()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate asset identifier", result.stderr)

    def test_conflicting_embedded_texture_identifiers(self):
        nested = self.assets / "nested"
        nested.mkdir()
        shutil.copyfile(self.assets / "hero.png", nested / "tiles.png")
        path = nested / "other.sprdef"
        path.write_text("sheet tiles.png 8 8\nclip idle 0 1 1 1\n")
        # The embedded 'tiles' Texture differs from the project's standalone tiles.png.
        self.bad(self.assets / "tiles.png", "duplicate asset identifier", "Texture")

    def test_shared_sheet_and_cross_type_names(self):
        (self.assets / "other.sprdef").write_text("sheet hero.png 8 8\nclip idle 0 1 1 1\n")
        # Sprite, Font, Texture and Blob may all be called hero. Font and Sprite
        # formerly overwrote the same hero.phxspr intermediate.
        (self.assets / "hero.font").write_text(json.dumps({"image": "hero.png", "cell_w": 8, "cell_h": 8, "count": 1}))
        shutil.copyfile(self.assets / "flow.json", self.assets / "hero.json")
        self.assertEqual(self.good(), self.good())

    def test_sprite_json_format(self):
        (self.assets / "hero.sprdef").unlink()
        (self.assets / "hero.json").write_text(json.dumps({"image": "hero.png", "tile": 8,
                                                           "animations": {"idle": {"frames": [0], "fps": 1}}}))
        self.good()

    def test_bmfont_and_legacy_sources(self):
        (self.assets / "extra.fnt").write_text('common lineHeight=8 base=7 pages=1\npage id=0 file="hero.png"\n'
                                             'char id=65 x=0 y=0 width=4 height=7 xoffset=0 yoffset=0 xadvance=5 page=0\n')
        (self.assets / "art.ppm").write_bytes(b"P6\n1 1\n255\n\xff\x00\x00")
        (self.assets / "legacy.tmcsv").write_text("art 8 8\n1\n")
        self.good()

    def test_uppercase_extensions(self):
        (self.assets / "hero.sprdef").rename(self.assets / "hero.SPRDEF")
        (self.assets / "font.font").rename(self.assets / "font.FONT")
        (self.assets / "theme.song").rename(self.assets / "theme.SONG")
        self.good()

    def test_missing_texture_reference(self):
        path = self.assets / "tiles.png"
        path.unlink()
        self.bad(self.assets / "level.tmj", "missing Texture reference", "Tilemap")

    def test_invalid_sprite_transition(self):
        path = self.assets / "hero.sprdef"
        with path.open("a") as f:
            f.write("\ntrans idle nonexistent move\n")
        self.bad(path, "nonexistent", "Sprite")

    def test_invalid_font_reference(self):
        path = self.assets / "font.font"
        path.write_text('{"image":"missing.png"}')
        self.bad(path, "missing.png", "Font")

    def test_unsupported_format(self):
        path = self.assets / "portrait.jpg"
        path.write_bytes(b"image")
        self.bad(path, "unsupported source format", ".jpg")

    def test_unknown_json_schema(self):
        path = self.assets / "unknown.json"
        path.write_text('{"hello":1}')
        self.bad(path, "unsupported schema", "JSON asset")

    def test_invalid_table_schema(self):
        path = self.assets / "flow.json"
        path.write_text('{"struct":"Flow","fields":[{"name":"x","type":"bogus"}],"records":[]}')
        self.bad(path, "bad field", "Blob")

    def test_invalid_records(self):
        path = self.assets / "flow.json"
        doc = json.loads(path.read_text())
        doc["records"] = {}
        path.write_text(json.dumps(doc))
        self.bad(path, "records must be an array", "Blob")

    def test_invalid_record(self):
        path = self.assets / "flow.json"
        doc = json.loads(path.read_text())
        doc["records"] = [42]
        path.write_text(json.dumps(doc))
        self.bad(path, "record 0 must be an object", "Blob")

    def test_duplicate_field_identifiers(self):
        path = self.assets / "flow.json"
        doc = json.loads(path.read_text())
        doc["fields"].append(doc["fields"][0])
        path.write_text(json.dumps(doc))
        self.bad(path, "duplicate field identifier", "Blob")

    def test_malformed_converter_formats(self):
        for name, kind in (("level.tmj", "Tilemap"), ("font.font", "Font"), ("theme.song", "Sound"),
                           ("dialogue.dlg", "Dialogue")):
            with self.subTest(name=name):
                path = self.assets / name
                original = path.read_bytes()
                path.write_bytes(b"{")
                self.bad(path, "phx", kind)
                path.write_bytes(original)

    def test_corrupt_texture(self):
        path = self.assets / "tiles.png"
        path.write_bytes(b"not a PNG")
        self.bad(path, "PNG")

    def test_removed_source_removes_stale_header(self):
        shutil.copyfile(self.assets / "flow.json", self.assets / "extra.json")
        self.good()
        header = self.project / "build/gen/extra.gen.h"
        self.assertTrue(header.exists())
        (self.assets / "extra.json").unlink()
        self.good()
        self.assertFalse(header.exists())

    def test_asset_hash_collision(self):
        # These distinct strings have the same FNV-1a hash (0x5e4daa9d).
        for name in ("costarring", "liquid"):
            shutil.copyfile(self.assets / "flow.json", self.assets / (name + ".json"))
        result = self.bad(self.assets / "liquid.json", "duplicate asset identifier", "Blob")
        self.assertIn(str(self.assets / "costarring.json"), result.stderr)

    def test_invalid_field_value(self):
        path = self.assets / "flow.json"
        doc = json.loads(path.read_text())
        field = next(f for f in doc["fields"] if f["type"] == "u8")
        doc["records"][0][field["name"]] = "invalid number"
        path.write_text(json.dumps(doc))
        self.bad(path, "expects a number", "Blob")

    def test_empty_asset_directory(self):
        shutil.rmtree(self.assets)
        self.assets.mkdir()
        self.bad(self.project, "nothing to bake", "Project assets")

    def test_converter_success_without_output(self):
        fake = self.root / "no-output-tools"
        fake.mkdir()
        for name in ("phxnew", "phxsprite", "phxtile", "phxsnd", "phxpack"):
            (fake / name).symlink_to(TOOLS / name)
        converter = fake / "phxbin"
        converter.write_text("#!/bin/sh\nexit 0\n")
        converter.chmod(0o755)
        result = self.bake(tools=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.assets / "flow.json"), result.stderr)
        self.assertIn("[Blob]", result.stderr)
        self.assertIn("converter produced no output", result.stderr)
        self.assertFalse(self.bundle.exists())

    def test_corrupt_converter_output(self):
        fake = self.root / "corrupt-tools"
        fake.mkdir()
        for name in ("phxnew", "phxsprite", "phxtile", "phxsnd", "phxpack"):
            (fake / name).symlink_to(TOOLS / name)
        converter = fake / "phxbin"
        # Use a Python shim to preserve --out/--header argument handling.
        converter.write_text("#!" + sys.executable + "\nimport pathlib, sys\na=sys.argv\n"
                             "pathlib.Path(a[a.index('--out')+1]).write_text('broken')\n"
                             "pathlib.Path(a[a.index('--header')+1]).write_text('header')\n")
        converter.chmod(0o755)
        result = self.bake(tools=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.assets / "flow.json"), result.stderr)
        self.assertIn("[Blob]", result.stderr)
        self.assertIn("bad/unsupported bundle", result.stderr)
        self.assertFalse(self.bundle.exists())

    def test_corrupt_intermediate_checksum(self):
        fake = self.root / "bad-crc-tools"
        fake.mkdir()
        for name in ("phxnew", "phxsprite", "phxtile", "phxsnd", "phxpack"):
            (fake / name).symlink_to(TOOLS / name)
        converter = fake / "phxbin"
        converter.write_text("#!" + sys.executable + "\nimport pathlib, subprocess, sys\na=sys.argv[1:]\n"
                             "subprocess.check_call([" + repr(str(TOOLS / "phxbin")) + "]+a)\n"
                             "p=pathlib.Path(a[a.index('--out')+1]); b=bytearray(p.read_bytes()); b[-1]^=1; p.write_bytes(b)\n")
        converter.chmod(0o755)
        result = self.bake(tools=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.assets / "flow.json"), result.stderr)
        self.assertIn("bad/unsupported bundle", result.stderr)
        self.assertFalse(self.bundle.exists())

    def test_missing_generated_header(self):
        fake = self.root / "no-header-tools"
        fake.mkdir()
        for name in ("phxnew", "phxsprite", "phxtile", "phxsnd", "phxpack"):
            (fake / name).symlink_to(TOOLS / name)
        converter = fake / "phxbin"
        converter.write_text("#!" + sys.executable + "\nimport subprocess, sys\na=sys.argv[1:]\n"
                             "i=a.index('--header'); del a[i:i+2]\n"
                             "sys.exit(subprocess.call([" + repr(str(TOOLS / "phxbin")) + "]+a))\n")
        converter.chmod(0o755)
        result = self.bake(tools=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(self.assets / "flow.json"), result.stderr)
        self.assertIn("converter produced no header", result.stderr)
        self.assertFalse(self.bundle.exists())

    @unittest.skipUnless(Path("/dev/full").exists(), "requires /dev/full")
    def test_output_write_failures(self):
        # These writes open successfully but fail when their buffered bytes flush.
        for name, argv in (("phxpack", ["--out", "/dev/full", str(self.assets / "tiles.png")]),
                           ("phxbin", ["--out", str(self.project / "table.phxbin"), "--header", "/dev/full",
                                       str(self.assets / "flow.json")])):
            result = subprocess.run([str(TOOLS / name)] + argv, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot write", result.stderr)
        output = self.project / "lock-failure.phxp"
        Path(str(output) + ".lock").symlink_to("/dev/full")
        result = subprocess.run([str(TOOLS / "phxpack"), "--out", str(output), str(self.assets / "tiles.png")],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(output) + ".lock", result.stderr)
        self.assertNotIn("phxpack: wrote", result.stdout)

    def test_sparse_and_empty_tables_remain_valid(self):
        (self.assets / "sparse.json").write_text('{"struct":"Sparse","fields":[{"name":"x","type":"u8"}],"records":[{}]}')
        (self.assets / "empty.json").write_text('{"struct":"Empty","fields":[{"name":"x","type":"u8"}]}')
        self.good()

    def test_missing_converter(self):
        fake = self.root / "missing-tools"
        fake.mkdir()
        (fake / "phxnew").symlink_to(TOOLS / "phxnew")
        result = self.bake(tools=fake)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot run", result.stderr)
        self.assertIn("[", result.stderr)
        self.assertFalse(self.bundle.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + remaining)
