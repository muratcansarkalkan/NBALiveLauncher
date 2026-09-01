#!/usr/bin/env python3
"""Build the NBA Live ASI plugin and Scoreboard Theme Editor together.

Place this file in the root of the ASI repository, beside its .sln or
.vcxproj. The editor is expected at:
    ScoreboardThemeEditor/ScoreboardThemeEditor.csproj

Examples:
    py build_all.py
    py build_all.py --clean
    py build_all.py --cpp NBALiveLauncher.sln --platform Win32
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent
EDITOR_PROJECT = ROOT / "ScoreboardThemeEditor" / "ScoreboardThemeEditor.csproj"
IGNORED_PARTS = {".git", ".vs", "bin", "obj", "packages"}


class BuildError(RuntimeError):
    pass


def executable(name: str) -> str | None:
    return shutil.which(name)


def find_msbuild() -> str:
    direct = executable("msbuild") or executable("MSBuild.exe")
    if direct:
        return direct

    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    candidates = []
    if program_files_x86:
        candidates.append(
            Path(program_files_x86) /
            "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        )
    candidates.append(
        Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    )

    for vswhere in candidates:
        if not vswhere.is_file():
            continue
        command = [
            str(vswhere), "-latest", "-products", "*",
            "-requires", "Microsoft.Component.MSBuild",
            "-find", r"MSBuild\**\Bin\MSBuild.exe",
        ]
        result = subprocess.run(
            command, capture_output=True, text=True, check=False
        )
        paths = [line.strip() for line in result.stdout.splitlines()
                 if line.strip()]
        if paths and Path(paths[0]).is_file():
            return paths[0]

    raise BuildError(
        "MSBuild was not found. Install Visual Studio 2022 with the "
        "Desktop development with C++ workload."
    )


def is_ignored(path: Path) -> bool:
    try:
        relative = path.relative_to(ROOT)
    except ValueError:
        return False
    return any(part.lower() in IGNORED_PARTS for part in relative.parts)


def discover_cpp_target(explicit: str | None) -> Path:
    if explicit:
        target = Path(explicit)
        if not target.is_absolute():
            target = ROOT / target
        if not target.is_file():
            raise BuildError(f"C++ build target does not exist: {target}")
        return target

    solutions = sorted(
        path for path in ROOT.glob("*.sln") if not is_ignored(path)
    )
    if len(solutions) == 1:
        return solutions[0]
    if len(solutions) > 1:
        names = "\n  ".join(path.name for path in solutions)
        raise BuildError(
            "More than one solution was found. Select one with --cpp:\n  " + names
        )

    projects = sorted(
        path for path in ROOT.glob("*.vcxproj") if not is_ignored(path)
    )
    if len(projects) == 1:
        return projects[0]
    if len(projects) > 1:
        names = "\n  ".join(path.name for path in projects)
        raise BuildError(
            "More than one C++ project was found. Select one with --cpp:\n  " + names
        )

    raise BuildError(
        "No .sln or .vcxproj was found beside build_all.py. Place the script "
        "in the ASI repository root or pass --cpp PATH."
    )


def read_build_pairs(target: Path) -> list[tuple[str, str]]:
    """Read valid Configuration|Platform pairs from a solution/project."""
    try:
        content = target.read_text(encoding="utf-8-sig", errors="ignore")
    except OSError as error:
        raise BuildError(f"Could not read build target {target}: {error}")

    pairs: list[tuple[str, str]] = []
    if target.suffix.lower() == ".sln":
        in_configurations = False
        for line in content.splitlines():
            if "GlobalSection(SolutionConfigurationPlatforms)" in line:
                in_configurations = True
                continue
            if in_configurations and "EndGlobalSection" in line:
                break
            if not in_configurations or "=" not in line:
                continue
            name = line.split("=", 1)[0].strip()
            if "|" not in name:
                continue
            configuration, platform = (part.strip()
                                       for part in name.split("|", 1))
            pair = (configuration, platform)
            if pair not in pairs:
                pairs.append(pair)
    else:
        for match in re.finditer(
            r'<ProjectConfiguration\s+Include="([^"|]+)\|([^"]+)"',
            content,
            flags=re.IGNORECASE,
        ):
            pair = (match.group(1).strip(), match.group(2).strip())
            if pair not in pairs:
                pairs.append(pair)
    return pairs


def resolve_build_pair(target: Path, requested_configuration: str,
                       requested_platform: str | None) -> tuple[str, str | None]:
    pairs = read_build_pairs(target)
    if not pairs:
        # Unusual hand-authored project: let MSBuild use the requested
        # configuration and its own default platform.
        return requested_configuration, requested_platform

    configuration_pairs = [
        pair for pair in pairs
        if pair[0].lower() == requested_configuration.lower()
    ]
    if not configuration_pairs:
        available = ", ".join(f"{c}|{p}" for c, p in pairs)
        raise BuildError(
            f"Configuration '{requested_configuration}' is unavailable. "
            f"Valid solution configurations: {available}"
        )

    if requested_platform:
        for configuration, platform in configuration_pairs:
            if platform.lower() == requested_platform.lower():
                return configuration, platform
        available = ", ".join(
            platform for _, platform in configuration_pairs
        )
        raise BuildError(
            f"Platform '{requested_platform}' is unavailable for "
            f"'{requested_configuration}'. Valid platforms: {available}"
        )

    # Prefer conventional 32-bit labels, but use the solution's spelling.
    for preferred in ("Win32", "x86", "Any CPU", "x64"):
        for configuration, platform in configuration_pairs:
            if platform.lower() == preferred.lower():
                return configuration, platform
    return configuration_pairs[0]


def run(command: list[str], label: str) -> None:
    print(f"\n{'=' * 72}\n{label}\n{'=' * 72}")
    print(" ".join(f'\"{part}\"' if " " in part else part for part in command))
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode:
        raise BuildError(f"{label} failed with exit code {result.returncode}.")


def recent_outputs(start_time: float) -> list[Path]:
    extensions = {".asi", ".dll", ".exe"}
    outputs = []
    for path in ROOT.rglob("*"):
        try:
            relative_parts = {part.lower()
                              for part in path.relative_to(ROOT).parts}
        except ValueError:
            relative_parts = set()
        if relative_parts & {".git", ".vs", "packages"} or not path.is_file():
            continue
        if path.suffix.lower() not in extensions:
            continue
        try:
            if path.stat().st_mtime >= start_time - 2.0:
                outputs.append(path)
        except OSError:
            pass
    return sorted(outputs, key=lambda path: path.stat().st_mtime, reverse=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the ASI plugin and WPF scoreboard editor."
    )
    parser.add_argument("--cpp", help="C++ .sln or .vcxproj path")
    parser.add_argument("--configuration", default="Debug")
    parser.add_argument(
        "--platform",
        help="Solution platform override (auto-detected when omitted)",
    )
    parser.add_argument("--clean", action="store_true",
                        help="Clean both projects before building")
    parser.add_argument("--no-pause", action="store_true",
                        help="Do not wait for Enter before closing")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    start_time = time.time()
    try:
        cpp_target = discover_cpp_target(args.cpp)
        if not EDITOR_PROJECT.is_file():
            raise BuildError(f"Editor project was not found: {EDITOR_PROJECT}")

        msbuild = find_msbuild()
        dotnet = executable("dotnet")
        if not dotnet:
            raise BuildError(
                ".NET SDK was not found. Install the .NET 8 SDK and ensure "
                "dotnet is available on PATH."
            )

        configuration, platform = resolve_build_pair(
            cpp_target, args.configuration, args.platform
        )

        print("NBA Live scoreboard build")
        print(f"Root:          {ROOT}")
        print(f"ASI target:    {cpp_target.name}")
        print(f"Editor target: {EDITOR_PROJECT.relative_to(ROOT)}")
        print(f"Configuration: {configuration}")
        print(f"Platform:      {platform or '<MSBuild default>'}")

        common_msbuild = [
            str(cpp_target), "/m", "/nologo",
            f"/p:Configuration={configuration}",
        ]
        if platform:
            common_msbuild.append(f"/p:Platform={platform}")
        if args.clean:
            run([msbuild, *common_msbuild, "/t:Clean"], "Clean ASI plugin")
            run([
                dotnet, "clean", str(EDITOR_PROJECT),
                "-c", configuration, "--nologo"
            ], "Clean Scoreboard Theme Editor")

        run([msbuild, *common_msbuild, "/t:Build"], "Build ASI plugin")
        run([
            dotnet, "build", str(EDITOR_PROJECT),
            "-c", configuration, "--nologo"
        ], "Build Scoreboard Theme Editor")

        print("\nBuild completed successfully.")
        outputs = recent_outputs(start_time)
        if outputs:
            print("\nNew build outputs:")
            for output in outputs[:12]:
                print(f"  {output.relative_to(ROOT)}")
        else:
            print("\nBuild succeeded; no new .asi/.dll/.exe files were "
                  "found under the repository root.")
        return 0
    except (BuildError, OSError) as error:
        print(f"\nBUILD FAILED: {error}", file=sys.stderr)
        return 1
    finally:
        if not args.no_pause and sys.stdin.isatty():
            try:
                input("\nPress Enter to close...")
            except EOFError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
