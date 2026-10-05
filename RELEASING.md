# Releasing Phosphorus

## Versioning policy

Phosphorus uses [semantic versioning](https://semver.org). Pre-1.0, the contract is:

- **MINOR** (`0.X.0`) — may change engine API, asset (`.phxp`) formats, or savegame layouts.
- **PATCH** (`0.x.Y`) — fixes and additions that break nothing existing.

The version lives in **one place**: `engine/core/include/phx/core/version.h`
(`PHX_VERSION_MAJOR/MINOR/PATCH`). Everything else derives from it — the composed
`PHX_VERSION_STRING`, `phx::version_string()`, CMake's `project(VERSION)`/CPack, the Makefile's
`PHX_VERSION` (dist archive names, `make version`), and the release workflow's tag check. Never
copy the number anywhere else.

## What a release ships

Pushing a `vX.Y.Z` tag runs `.github/workflows/release.yml`, which re-runs the gates and
attaches these to a GitHub release (build any of them locally with the same targets):

| Artifact | Built by | Contents |
|---|---|---|
| `phosphorus-X.Y.Z-tools-linux-x86_64.tar.gz` | `make dist` | the five asset-pipeline CLIs (`phxpack`, `phxsprite`, `phxtile`, `phxsnd`, `phxbin`) |
| `phosphorus-X.Y.Z-sdk-linux-x86_64.tar.gz` | `cpack` (CMake tree) | headers + static libs + tools + `find_package(phosphorus)` config |
| `phosphorus-X.Y.Z-tools-windows-x86_64.zip` | `make dist-win` | the same CLIs as static PE32+ `.exe` |
| `phosphorus-X.Y.Z-gba.zip` | `make dist-gba` | the shipping PPU ROMs: platformer + Emberwing (size-gated) |
| `phosphorus-X.Y.Z-psp.zip` | `make dist-psp` | `platformer/EBOOT.PBP` + `emberwing/EBOOT.PBP`, ready for `ms0:/PSP/GAME/` or PPSSPP |

The SDK installs anywhere via `cmake --install build/<dir> --prefix <where>`; downstream
projects then use `find_package(phosphorus CONFIG)` and link `phosphorus::phx_<module>`.

## Release checklist

1. **Start clean on `main`** — CI green, working tree clean.
2. **Bump the version** in `engine/core/include/phx/core/version.h` (the three ints only; the
   string composes itself).
3. **Update `CHANGELOG.md`** — retitle `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD` and start a
   fresh empty `[Unreleased]` section above it.
4. **Run the gates locally**:
   ```bash
   make check && make determinism && make sanitize && make release
   ```
5. **Commit and tag** (annotated tag, `v` prefix — the workflow triggers on it and rejects a
   tag that doesn't match the header):
   ```bash
   git commit -am "release: vX.Y.Z"
   git tag -a vX.Y.Z -m "Phosphorus X.Y.Z"
   git push origin main vX.Y.Z
   ```
6. **Watch the Release workflow** (Actions → Release). It re-verifies tag↔header, re-runs
   `make check` and the Wine-verified Windows build, enforces the GBA ROM budgets, builds all
   five artifacts, and publishes the GitHub release with generated notes.
7. **Spot-check the published artifacts**: the GBA zip in mGBA, the PSP zip in PPSSPP, a tool
   binary's basic run (`phxpack` with no args prints usage).

If a step fails after the tag is pushed: fix on `main`, delete the tag
(`git push origin :refs/tags/vX.Y.Z`, `git tag -d vX.Y.Z`), bump PATCH, and go again — never
reuse a version number that reached the public remote.
