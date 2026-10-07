#!/usr/bin/env python3
"""Open the SEASIDE 2-by-2 Rezonality nature scene in Draxul.

Run from a terminal pane inside the target Draxul Space:

    python3 plugins/rezonality/examples/seaside/launch.py

Outside Draxul, pass --space (see `draxul space list --json`). Every run
creates a new tab; the launcher prints the created tab and pane ids as JSON.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PLUGIN_ID = "dev.draxul.rezonality"
PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parents[3]

# (scenegraph stem, pane name) in grid order: top-left, top-right,
# bottom-left, bottom-right.
PANES = [
    ("dawn", "Dawn - Sea Stacks"),
    ("kelp", "Midday - Kelp Forest"),
    ("sunset", "Sunset - Broad Reach"),
    ("night", "Night - Lighthouse Point"),
]
MODELS = ["ocean_grid.obj", "sailboat.obj", "lighthouse.obj", "headland.obj", "kelp.obj", "fish.obj"]


def find_draxul(explicit):
    if explicit:
        return str(Path(explicit).resolve())
    from_env = os.environ.get("DRAXUL_EXECUTABLE")
    if from_env and Path(from_env).exists():
        return from_env
    candidates = [
        "build/draxul.app/Contents/MacOS/draxul",
        "build-release/draxul.app/Contents/MacOS/draxul",
        "build-ninja-release/draxul.exe",
        "build-ninja-debug/draxul.exe",
        "build/Release/draxul.exe",
        "build/Debug/draxul.exe",
    ]
    for candidate in candidates:
        path = REPO / candidate
        if path.exists():
            return str(path)
    on_path = shutil.which("draxul")
    if on_path:
        return on_path
    sys.exit("Could not find draxul. Build Draxul or pass --draxul <path>.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--draxul", help="Path to the draxul executable")
    parser.add_argument("--space", help="Target Space id (defaults to the current pane's Space)")
    parser.add_argument("--session", help="Draxul Session id")
    parser.add_argument("--server-runtime-dir", help="Non-default server runtime directory")
    parser.add_argument("--name", default="Seaside", help="Tab name")
    parser.add_argument("--paused", action="store_true", help="Start with animation paused")
    args = parser.parse_args()

    if any(not (PROJECT / model).exists() for model in MODELS):
        subprocess.run([sys.executable, str(PROJECT / "generate_models.py")], check=True)

    draxul = find_draxul(args.draxul)
    route = []
    if args.session:
        route += ["--session", args.session]
    if args.server_runtime_dir:
        route += ["--server-runtime-dir", args.server_runtime_dir]

    def call(*command):
        result = subprocess.run([draxul, *command, "--json", *route],
                                capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            sys.exit("draxul %s failed (%d): %s" % (" ".join(command), result.returncode,
                                                    result.stderr.strip()))
        return json.loads(result.stdout)

    space = args.space or os.environ.get("DRAXUL_SPACE_ID")
    if not space:
        spaces = call("space", "list")
        spaces = spaces if isinstance(spaces, list) else spaces.get("spaces", [])
        if len(spaces) != 1:
            sys.exit("Pass --space when the Session does not contain exactly one Space.")
        space = spaces[0].get("space_id") or spaces[0].get("id")

    def config(stem):
        return json.dumps({
            "project_path": PROJECT.as_posix(),
            "scenegraph": stem + ".scenegraph",
            "auto_reload": True,
            "paused": args.paused,
            "compile_debounce_ms": 120,
            "diagnostics_id": "seaside-" + stem,
        }, separators=(",", ":"))

    tab = call("tab", "create", "--space", space, "--name", args.name,
               "--plugin", PLUGIN_ID, "--plugin-config", config(PANES[0][0]))["created_id"]
    try:
        dawn = call("tab", "get", tab)["panes"][0]["pane_id"]

        def split(target, direction, stem):
            return call("pane", "split", target, "--direction", direction, "--ratio", "0.5",
                        "--plugin", PLUGIN_ID, "--plugin-config", config(stem))["created_id"]

        kelp = split(dawn, "right", PANES[1][0])
        sunset = split(dawn, "down", PANES[2][0])
        night = split(kelp, "down", PANES[3][0])
        ids = [dawn, kelp, sunset, night]
        for pane, (_, name) in zip(ids, PANES):
            call("pane", "rename", pane, "--name", name)
    except SystemExit:
        print("Launch stopped after creating tab %s; it was kept for inspection." % tab,
              file=sys.stderr)
        raise

    print(json.dumps({
        "ok": True,
        "space_id": space,
        "tab_id": tab,
        "panes": {stem: pane for (stem, _), pane in zip(PANES, ids)},
    }, indent=2))


if __name__ == "__main__":
    main()
