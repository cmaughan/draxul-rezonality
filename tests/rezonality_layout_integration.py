#!/usr/bin/env python3
"""Apply a generated Rezonality layout to an isolated Draxul server."""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time


def run(
    command: list[str], *, input_text: str | None = None, timeout: int = 30
) -> str:
    completed = subprocess.run(
        command,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join(command)} failed ({completed.returncode}): "
            f"{completed.stderr}"
        )
    return completed.stdout


def wait_for_process_exit(process_id: int, timeout_seconds: float) -> bool:
    if os.name == "nt":
        synchronize = 0x00100000
        wait_object_0 = 0
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = [
            ctypes.c_ulong,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        kernel32.WaitForSingleObject.restype = ctypes.c_ulong
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.OpenProcess(synchronize, False, process_id)
        if not handle:
            return True
        try:
            timeout_ms = int(timeout_seconds * 1000)
            return kernel32.WaitForSingleObject(handle, timeout_ms) == wait_object_0
        finally:
            kernel32.CloseHandle(handle)

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            os.kill(process_id, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.05)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--draxul", required=True)
    parser.add_argument("--python", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--project", required=True)
    args = parser.parse_args()

    source_executable = pathlib.Path(args.draxul).resolve()
    executable = str(source_executable)
    generator = str(pathlib.Path(args.generator).resolve())
    project = pathlib.Path(args.project).resolve()
    # Darwin's Unix-domain socket path limit is only 103 bytes. Keep the
    # isolated runtime below /tmp instead of the much longer per-user temp root.
    temp_root = "/tmp" if sys.platform == "darwin" else None
    with tempfile.TemporaryDirectory(
        prefix="draxul-rezonality-layout-", dir=temp_root
    ) as temp:
        runtime = pathlib.Path(temp) / "runtime"
        runtime.mkdir()
        if os.name == "nt":
            # A user's long-lived Debug server legitimately keeps the sibling
            # draxul-server.exe helper open. Stage this isolated scenario's
            # launcher beside its private runtime so helper refresh cannot
            # collide with or require stopping that server.
            isolated_app = pathlib.Path(temp) / "app"
            isolated_app.mkdir()
            isolated_executable = isolated_app / source_executable.name
            shutil.copy2(source_executable, isolated_executable)
            executable = str(isolated_executable)
        route = ["--server-runtime-dir", str(runtime)]
        server_pid = 0
        server_process: subprocess.Popen[str] | None = None
        try:
            server_process = subprocess.Popen(
                [executable, "--server", *route],
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline:
                return_code = server_process.poll()
                if return_code is not None and return_code != 0:
                    stderr = server_process.stderr.read() if server_process.stderr else ""
                    raise RuntimeError(
                        f"isolated Draxul server exited "
                        f"({return_code}): {stderr}"
                    )
                metadata = list(runtime.glob("*.control.json"))
                if metadata:
                    try:
                        server_metadata = json.loads(metadata[0].read_text())
                        server_pid = int(server_metadata.get("server_pid", 0))
                        if server_metadata["state"] == "ready":
                            if not server_pid:
                                raise RuntimeError(
                                    "ready server metadata omitted server_pid"
                                )
                            break
                    except (OSError, KeyError, json.JSONDecodeError):
                        pass
                time.sleep(0.1)
            else:
                raise RuntimeError("isolated Draxul server did not become ready")

            layout = run(
                [args.python, generator, "--project", str(project)]
            )
            validation = json.loads(
                run(
                    [executable, "layout", "validate", "-", "--json", *route],
                    input_text=layout,
                )
            )
            if not validation.get("valid"):
                raise RuntimeError(f"layout validation failed: {validation}")
            applied = json.loads(
                run(
                    [executable, "layout", "apply", "-", "--json", *route],
                    input_text=layout,
                )
            )
            aliases = applied["aliases"]
            panes = json.loads(
                run(
                    [
                        executable,
                        "pane",
                        "list",
                        "--space",
                        applied["created_id"],
                        "--json",
                        *route,
                    ]
                )
            )
            if len(panes) != 2:
                raise RuntimeError(f"expected editor and view panes: {panes}")
            editor = next(pane for pane in panes if pane["id"] == aliases["editor"])
            view = next(pane for pane in panes if pane["id"] == aliases["view"])
            config = json.loads(view["client_plugin_config_json"])
            if not editor["terminal_id"]:
                raise RuntimeError("editor pane did not allocate a terminal")
            if (
                view["domain"] != "client_local"
                or view["client_plugin_id"] != "dev.draxul.rezonality"
                or view["terminal_id"]
                or pathlib.Path(config["project_path"]) != project
                or not config["diagnostics_id"]
            ):
                raise RuntimeError(f"invalid Rezonality pane descriptor: {view}")

            marker = "REZONALITY_AGENT_LAYOUT_OK"
            run(
                [
                    executable,
                    "pane",
                    "run",
                    aliases["editor"],
                    "--command",
                    f"echo {marker}",
                    "--json",
                    *route,
                ]
            )
            waited = json.loads(
                run(
                    [
                        executable,
                        "pane",
                        "wait-output",
                        aliases["editor"],
                        "--text",
                        marker,
                        "--timeout",
                        "15s",
                        "--json",
                        *route,
                    ],
                    timeout=20,
                )
            )
            if marker not in waited["text"]:
                raise RuntimeError("editor terminal did not run in generated layout")
        finally:
            try:
                run([executable, "--shutdown-server", "--yes", *route], timeout=15)
            except Exception:
                pass
            if server_process is not None:
                if server_process.poll() is None:
                    try:
                        server_process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server_process.terminate()
                        server_process.wait(timeout=10)
                        raise RuntimeError(
                            "isolated foreground Draxul server survived shutdown"
                        )
            if server_pid and not wait_for_process_exit(server_pid, 10.0):
                raise RuntimeError(
                    f"isolated Draxul server PID {server_pid} survived shutdown"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
