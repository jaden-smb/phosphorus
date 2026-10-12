"""Project metadata is interpreted by ProjectDoc, shared with Studio and phxnew."""
import json
import subprocess


def load_project(project, config_tool):
    result = subprocess.run([config_tool, "--project-json", project],
                            check=True, capture_output=True, text=True)
    # Decode the normalized transport; do not apply another set of defaults here.
    return json.loads(result.stdout)
