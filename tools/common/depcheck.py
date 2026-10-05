#!/usr/bin/env python3
"""depcheck.py — enforce Phosphorus's acyclic module dependency law (docs/00 §3).

Parses `#include "phx/<module>/..."` edges across engine/ and fails the build if any
module depends on a module that is *above* it in the allowed layering, or if any cycle
exists. A dependency violation is treated like a compile error.

Usage:  depcheck.py <engine_dir>
"""
import os
import re
import sys

# Allowed downward layering. A module may depend only on modules in LOWER layers
# (or its own layer for leaf utilities). Lower index = lower layer.
LAYERS = [
    ["core"],                           # L0: closed foundation (types/math/fixed/log/time/config)
    ["memory", "platform"],             # L1: build on core only (allocators; the C seam)
    ["render", "input", "audio", "resource", "ecs"],   # L2: services
    ["scene", "physics", "anim", "ui"], # L3: gameplay systems
    ["runtime"],                        # L4: composition root (App/loop) — depends on all below
]
LAYER_OF = {m: i for i, layer in enumerate(LAYERS) for m in layer}

INCLUDE_RE = re.compile(r'#\s*include\s*[<"]phx/([a-z_]+)/')


def module_of(path, engine_dir):
    rel = os.path.relpath(path, engine_dir)
    return rel.split(os.sep)[0]


def main():
    if len(sys.argv) != 2:
        print("usage: depcheck.py <engine_dir>", file=sys.stderr)
        return 2
    engine = sys.argv[1]
    edges = set()          # (from_module, to_module)
    violations = []

    for root, _dirs, files in os.walk(engine):
        for f in files:
            if not f.endswith((".h", ".hpp", ".c", ".cpp")):
                continue
            p = os.path.join(root, f)
            src_mod = module_of(p, engine)
            if src_mod not in LAYER_OF:
                continue
            with open(p, encoding="utf-8", errors="ignore") as fh:
                for line in fh:
                    m = INCLUDE_RE.search(line)
                    if not m:
                        continue
                    dst_mod = m.group(1)
                    if dst_mod not in LAYER_OF or dst_mod == src_mod:
                        continue
                    edges.add((src_mod, dst_mod))
                    # the law: you may only include strictly-lower layers
                    if LAYER_OF[dst_mod] >= LAYER_OF[src_mod]:
                        violations.append(
                            f"  {src_mod} (L{LAYER_OF[src_mod]}) -> "
                            f"{dst_mod} (L{LAYER_OF[dst_mod]})  in {os.path.relpath(p, engine)}"
                        )

    # cycle detection (defensive — layering should already prevent cycles)
    cycles = find_cycles(edges)

    if violations or cycles:
        print("DEPENDENCY LAW VIOLATION (docs/00 §3):", file=sys.stderr)
        for v in sorted(set(violations)):
            print(v, file=sys.stderr)
        for c in cycles:
            print("  cycle: " + " -> ".join(c), file=sys.stderr)
        return 1

    print(f"depcheck: OK ({len(edges)} edges, acyclic, layering respected)")
    return 0


def find_cycles(edges):
    graph = {}
    for a, b in edges:
        graph.setdefault(a, set()).add(b)
    cycles, color = [], {}

    def dfs(n, stack):
        color[n] = 1
        for m in graph.get(n, ()):
            if color.get(m, 0) == 1:
                cycles.append(stack[stack.index(m):] + [m])
            elif color.get(m, 0) == 0:
                dfs(m, stack + [m])
        color[n] = 2

    for n in list(graph):
        if color.get(n, 0) == 0:
            dfs(n, [n])
    return cycles


if __name__ == "__main__":
    sys.exit(main())
