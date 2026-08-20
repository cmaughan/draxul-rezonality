#!/usr/bin/env python3
"""Install the bundled Rezonality Neovim package without editing init.lua."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import time
import uuid


PACKAGE_NAME = "rezonality.nvim"
PACKAGE_SOURCE = (
    pathlib.Path(__file__).resolve().parent.parent
    / "integrations"
    / "neovim"
    / PACKAGE_NAME
)


def neovim_data_directory(executable: str) -> pathlib.Path:
    command = [
        executable,
        "--headless",
        "--clean",
        "-n",
        "+lua io.write(vim.fn.stdpath('data'))",
        "+qa!",
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    if completed.returncode != 0 or not completed.stdout.strip():
        raise RuntimeError(
            f"could not query Neovim data directory: {completed.stderr.strip()}"
        )
    return pathlib.Path(completed.stdout.strip())


def package_target(data_directory: pathlib.Path) -> pathlib.Path:
    return (
        data_directory
        / "site"
        / "pack"
        / "rezonality"
        / "start"
        / PACKAGE_NAME
    )


def install(target: pathlib.Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    incoming = target.parent / f".{PACKAGE_NAME}.incoming-{uuid.uuid4().hex}"
    backup = target.parent / f".{PACKAGE_NAME}.backup-{uuid.uuid4().hex}"
    shutil.copytree(PACKAGE_SOURCE, incoming)
    marker = incoming / "lua" / "rezonality" / "installed_at"
    marker.write_text(str(time.time_ns() // 1_000_000), encoding="ascii")
    try:
        if target.exists():
            target.replace(backup)
        incoming.replace(target)
    except Exception:
        if backup.exists() and not target.exists():
            backup.replace(target)
        raise
    finally:
        shutil.rmtree(incoming, ignore_errors=True)
        shutil.rmtree(backup, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nvim", default="nvim", help="Neovim executable")
    parser.add_argument(
        "--target",
        type=pathlib.Path,
        help="Exact package directory; intended for tests and managed installs.",
    )
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true")
    action.add_argument("--uninstall", action="store_true")
    args = parser.parse_args(argv)

    target = args.target.expanduser().resolve() if args.target else package_target(
        neovim_data_directory(args.nvim)
    )
    required = target / "lua" / "rezonality" / "init.lua"
    marker = target / "lua" / "rezonality" / "installed_at"
    if args.check:
        if required.is_file() and marker.is_file():
            print(f"Rezonality Neovim package is installed at {target}")
            return 0
        print(f"Rezonality Neovim package is not installed at {target}")
        return 1
    if args.uninstall:
        if target.exists():
            shutil.rmtree(target)
            print(f"Removed Rezonality Neovim package from {target}")
        else:
            print(f"Rezonality Neovim package is not installed at {target}")
        return 0

    install(target)
    print(f"Installed Rezonality Neovim package at {target}")
    print("Restart Neovim, then run :RezonalityStatus or :RezonalityProblems")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
