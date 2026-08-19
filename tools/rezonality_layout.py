#!/usr/bin/env python3
"""Generate a Draxul terminal-plus-Rezonality declarative layout."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys


def diagnostics_id(project: pathlib.Path, index: int) -> str:
    stem = re.sub(r"[^a-z0-9._-]+", "-", project.name.lower()).strip("-")
    stem = (stem or "project")[:40]
    digest = hashlib.sha256(str(project).encode("utf-8")).hexdigest()[:8]
    suffix = "" if index == 1 else f"-{index}"
    return f"{stem}-{digest}{suffix}"


def make_layout(projects: list[pathlib.Path], name: str) -> dict[str, object]:
    panes: list[dict[str, object]] = [
        {
            "name": "Editor",
            "alias": "editor",
            "cwd": str(projects[0]),
        }
    ]
    previous = "editor"
    for index, project in enumerate(projects, start=1):
        alias = "view" if index == 1 else f"view{index}"
        panes.append(
            {
                "name": f"Rezonality {project.name}",
                "alias": alias,
                "split_from": previous,
                "direction": "right",
                "ratio": 0.55 if index == 1 else 0.5,
                "plugin_id": "dev.draxul.rezonality",
                "plugin_config": {
                    "project_path": str(project),
                    "auto_reload": True,
                    "diagnostics_id": diagnostics_id(project, index),
                },
            }
        )
        previous = alias
    return {
        "name": name,
        "alias": "rezonality_space",
        "root_directory": str(projects[0]),
        "tabs": [
            {
                "name": "Live Edit",
                "alias": "live_edit",
                "panes": panes,
            }
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Emit a Draxul layout with an editor terminal and Rezonality view."
    )
    parser.add_argument(
        "--project",
        action="append",
        required=True,
        help="Project directory; repeat for additional Rezonality views.",
    )
    parser.add_argument("--name", default="Rezonality Live Edit")
    args = parser.parse_args(argv)

    projects: list[pathlib.Path] = []
    for raw in args.project:
        project = pathlib.Path(raw).expanduser().resolve()
        if not project.is_dir():
            parser.error(f"project directory does not exist: {project}")
        if not (project / "project.toml").is_file():
            parser.error(f"project.toml is missing: {project}")
        projects.append(project)
    json.dump(make_layout(projects, args.name), sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
