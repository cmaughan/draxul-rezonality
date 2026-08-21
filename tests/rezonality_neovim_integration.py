#!/usr/bin/env python3
"""Exercise the bundled Rezonality package in a clean headless Neovim."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import subprocess
import sys
import tempfile
import time


def run(command: list[str], *, environment: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        text=True,
        capture_output=True,
        check=False,
        env=environment,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def diagnostic(path: pathlib.Path, line: int, message: str) -> dict[str, object]:
    return {
        "path": path.as_posix(),
        "stage": "compile",
        "severity": "error",
        "line": line,
        "column": 3,
        "message": message,
    }


def write_batch(
    path: pathlib.Path,
    project: pathlib.Path,
    entries: list[dict[str, object]],
) -> None:
    primary = entries[0]
    source_files = sorted({
        (project / "default.scenegraph").as_posix(),
        *(str(entry["path"]) for entry in entries),
    })
    document = {
        "schema_version": 3,
        "plugin_id": "dev.draxul.rezonality",
        "project_path": project.as_posix(),
        "attempted_generation": 4,
        "active_generation": 3,
        "active_source_file_count": len(source_files),
        "active_source_files_truncated": False,
        "active_source_files": source_files,
        "timestamp_unix_ms": time.time_ns() // 1_000_000,
        **primary,
        "diagnostic_count": len(entries),
        "diagnostics_truncated": False,
        "diagnostics": entries,
    }
    path.write_text(json.dumps(document), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nvim", required=True)
    parser.add_argument("--draxul", type=pathlib.Path, required=True)
    parser.add_argument("--installer", type=pathlib.Path, required=True)
    parser.add_argument("--script", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="rezonality-nvim-") as temporary:
        root = pathlib.Path(temporary)
        installed = root / "installed" / "rezonality.nvim"
        server_runtime = root / "server-runtime"
        server_runtime.mkdir()
        environment = os.environ.copy()
        if platform.system() == "Windows":
            cache_root = root / "local-app-data"
            environment["LOCALAPPDATA"] = str(cache_root)
            diagnostics = (
                cache_root / "draxul" / "cache" / "plugins"
                / "dev.draxul.rezonality" / "diagnostics"
            )
        elif platform.system() == "Darwin":
            home = root / "home"
            environment["HOME"] = str(home)
            diagnostics = (
                home / "Library" / "Caches" / "draxul" / "plugins"
                / "dev.draxul.rezonality" / "diagnostics"
            )
        else:
            cache_root = root / "cache"
            environment["XDG_CACHE_HOME"] = str(cache_root)
            diagnostics = (
                cache_root / "draxul" / "plugins"
                / "dev.draxul.rezonality" / "diagnostics"
            )
        project = root / "project"
        diagnostics.mkdir(parents=True)
        project.mkdir()
        first = project / "screen.frag"
        second = project / "copy.vert"
        (project / "default.scenegraph").write_text(
            "[[pass]]\nvertex = \"copy.vert\"\nfragment = \"screen.frag\"\n",
            encoding="utf-8",
        )
        first.write_text("line one\nline two\n", encoding="utf-8")
        second.write_text("one\ntwo\n", encoding="utf-8")

        run([
            sys.executable,
            str(args.installer),
            "--nvim",
            args.nvim,
            "--target",
            str(installed),
        ])
        run([
            sys.executable,
            str(args.installer),
            "--target",
            str(installed),
            "--check",
        ])
        if not (installed / "lua" / "rezonality" / "init.lua").is_file():
            raise RuntimeError("installer did not copy the Lua package")
        help_tags = installed / "doc" / "tags"
        if not help_tags.is_file() or ":RezApply" not in help_tags.read_text(
            encoding="utf-8"
        ):
            raise RuntimeError("installer did not generate Rezonality help tags")
        (server_runtime / "server.control.json").write_text(json.dumps({
            "state": "ready",
            "published_unix_ms": time.time_ns() // 1_000_000,
            "client_executable": str(args.draxul.resolve()),
        }), encoding="utf-8")

        stale = diagnostic(first, 1, "error from a pre-install pane")
        write_batch(diagnostics / "stale-pane.json", project, [stale])
        stale_document = json.loads(
            (diagnostics / "stale-pane.json").read_text(encoding="utf-8")
        )
        stale_document["timestamp_unix_ms"] = 1
        (diagnostics / "stale-pane.json").write_text(
            json.dumps(stale_document), encoding="utf-8"
        )

        shared = diagnostic(first, 2, "shared shader error")
        write_batch(
            diagnostics / "flight-left.json",
            project,
            [shared, diagnostic(second, 1, "left-only error")],
        )
        write_batch(
            diagnostics / "flight-right.json",
            project,
            [shared, diagnostic(second, 2, "right-only error")],
        )
        write_batch(
            diagnostics / "orphan-pane.json",
            project,
            [diagnostic(first, 1, "error from a closed pane")],
        )

        result = root / "result.json"
        environment.update({
            "REZONALITY_TEST_PACKAGE": str(installed),
            "REZONALITY_TEST_FIRST": str(first),
            "REZONALITY_TEST_SECOND": str(second),
            "REZONALITY_TEST_RESULT": str(result),
            "REZONALITY_TEST_DRAXUL": str(args.draxul.resolve()),
            "DRAXUL_SERVER_RUNTIME_DIR": str(server_runtime),
        })
        environment.pop("DRAXUL_EXECUTABLE", None)
        run([
            args.nvim,
            "--headless",
            "--clean",
            "-n",
            "-c",
            f"lua dofile([[{args.script.as_posix()}]])",
        ], environment=environment)
        observed = json.loads(result.read_text(encoding="utf-8"))
        expected = {
            "all_entries": 3,
            "first_inline": 1,
            "second_inline": 2,
            "quickfix": 3,
            "shared_sources": 2,
            "instances": 2,
            "active_files": 3,
            "selected_file": "default.scenegraph",
            "selected_file_has_both_instances": True,
            "apply_mapping_installed": True,
            "apply_saved": True,
            "apply_flash_started": True,
            "failed_instances": 2,
            "control_actions": [
                "focus:pane-left",
                "reload:pane-right",
                "reload:pane-left",
            ],
            "registry_available": True,
            "rez_commands": 10,
            "compatibility_commands": 10,
            "status_visible": True,
            "server_discovery": True,
        }
        if observed != expected:
            raise RuntimeError(f"unexpected Neovim result: {observed!r}")

        run([
            sys.executable,
            str(args.installer),
            "--target",
            str(installed),
            "--uninstall",
        ])
        if installed.exists():
            raise RuntimeError("uninstall left the package behind")

    print("Rezonality Neovim integration passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
