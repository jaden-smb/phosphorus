#!/usr/bin/env python3
"""Mutate project-check's fresh template to prove rename and source discovery end to end."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
from project_config import load_project

project = Path(sys.argv[1]).resolve()
tool = str(Path(sys.argv[2]).resolve())
meta = load_project(str(project), tool)
assert meta["id"] == "project_check" and meta["build"] == "generic"
# Moving gameplay out of src must affect actual compilation. A second declared root
# supplies a required symbol; unrelated src/tools and generated subtrees must be ignored.
main = project / "src/main.cpp"
target = project / "code/game/nested/main.cpp"
target.parent.mkdir(parents=True)
text = main.read_text().replace("using namespace phx;", "using namespace phx;\nextern void project_source_probe();")
text = text.replace("void on_start(App& app) override {", "void on_start(App& app) override { project_source_probe();")
target.write_text(text)
main.write_text('#error undeclared source root was compiled\n')
support = project / "support"
support.mkdir()
(support / "probe.cpp").write_text("void project_source_probe() {}\n")
for path in ("tools/unrelated.cpp", "code/game/build/generated.cpp", "code/game/dist/export.cpp"):
    p = project / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("#error unrelated source compiled\n")
# Simulate an older generated project: preserve its literal bundle path without an id.
meta.pop("id")
meta["name"] = "Renamed Display Title"
(support / "alias.cpp").symlink_to(support / "probe.cpp")
meta["source"] = ["code/game", "support", "code/game/nested"]
(project / "phxproject.json").write_text(json.dumps(meta))
assert load_project(str(project), tool)["id"] == "project_check"
files = shlex.split(subprocess.check_output([tool, "--project-sources", str(project)], text=True))
assert files == sorted([str(target), str(support / "probe.cpp")])
for example in Path("examples").glob("*/phxproject.json"):
    assert load_project(str(example.parent), tool)["build"] == "custom"
    result = subprocess.run([tool, "--project-check", str(example.parent)], capture_output=True)
    assert result.returncode != 0 and b"custom-build project" in result.stderr
print("project configuration: rename, nested roots, exclusions, custom modes OK")
