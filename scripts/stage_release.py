#!/usr/bin/env python3
"""Stage Retro-Go SD release assets for the active project kind.

Reads PROJECT_KIND and PACKED_BIN from the root Makefile, copies the built
.bin into the correct SD layout, extracts release notes from CHANGELOG.md for
the requested tag, and writes release metadata for CI.

Usage:
  python3 scripts/stage_release.py --bin example.bin --tag v1.0.0 --out release
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = ROOT / "Makefile"
DEFAULT_CHANGELOG = ROOT / "CHANGELOG.md"

MAKE_VARS = ("PROJECT_KIND", "PACKED_BIN", "CORE_NAME", "DOCKER_IMAGE")

HEADING_RE = re.compile(
    r"^##\s*(?:\[(?P<bracket>[^\]]+)\]|(?P<plain>[^\s#]+))(?:\s*-\s*(?P<date>.+))?\s*$",
    re.MULTILINE,
)


def read_make_vars() -> dict[str, str]:
    cmd = ["make", "-f", str(MAKEFILE), "--no-print-directory"]
    for var in MAKE_VARS:
        cmd.append(f"print-{var}")

    try:
        out = subprocess.check_output(cmd, cwd=ROOT, text=True, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as exc:
        print(exc.output or exc, file=sys.stderr)
        raise SystemExit(f"failed to read Makefile variables from {MAKEFILE}") from exc

    values = out.splitlines()
    if len(values) != len(MAKE_VARS):
        raise SystemExit(
            f"expected {len(MAKE_VARS)} Makefile values, got {len(values)}:\n{out}"
        )
    return dict(zip(MAKE_VARS, values, strict=True))


def sd_subdir(project_kind: str) -> str:
    if project_kind == "core":
        return "cores"
    if project_kind == "homebrew":
        return "homebrews"
    raise SystemExit(f"unsupported PROJECT_KIND: {project_kind!r} (expected core or homebrew)")


def slug(tag: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "-", tag).strip("-") or "release"


def extract_changelog_section(changelog_path: Path, tag: str) -> str:
    if not changelog_path.is_file():
        raise SystemExit(f"CHANGELOG not found: {changelog_path}")

    text = changelog_path.read_text(encoding="utf-8")
    matches = list(HEADING_RE.finditer(text))
    for index, match in enumerate(matches):
        version = (match.group("bracket") or match.group("plain") or "").strip()
        if version != tag:
            continue
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        body = text[start:end].strip()
        if not body:
            raise SystemExit(f"CHANGELOG section for {tag!r} is empty")
        return body

    raise SystemExit(
        "no CHANGELOG section for tag "
        f"{tag!r}; add '## [{tag}] - YYYY-MM-DD' before pushing the tag"
    )


def build_release_notes(
    *,
    tag: str,
    changelog_body: str,
    project_kind: str,
    packed_name: str,
    sd_dir: str,
    core_name: str,
    docker_image: str,
    archive_name: str,
) -> str:
    sdk_version = (ROOT / "SDK_VERSION").read_text(encoding="utf-8").strip()
    install_path = f"/{sd_dir}/{packed_name}"

    lines = [
        f"# {tag}",
        "",
        changelog_body,
        "",
        "---",
        "",
        f"- Project kind: `{project_kind}`",
        f"- Packed binary: `{packed_name}`",
        f"- SD install path: `{install_path}`",
        f"- Release archive: `{archive_name}`",
        f"- Built with: `{docker_image}`",
        "",
        "```",
        sdk_version,
        "```",
    ]
    if project_kind == "core":
        lines.append(f"- Test ROMs: `/roms/{core_name}/`")
    else:
        stem = Path(packed_name).stem
        lines.append(f"- Optional cover: `/covers/homebrew/{stem}.img`")

    return "\n".join(lines) + "\n"


def stage_release(
    *,
    bin_path: Path,
    tag: str,
    out_dir: Path,
    changelog_path: Path,
    docker_image: str | None,
) -> None:
    cfg = read_make_vars()
    project_kind = cfg["PROJECT_KIND"]
    packed_name = cfg["PACKED_BIN"]
    core_name = cfg["CORE_NAME"]
    resolved_docker = docker_image or cfg.get("DOCKER_IMAGE") or "sylverb/retro-go-sd-builder:v1.5"

    if not bin_path.is_file():
        raise SystemExit(f"packed binary not found: {bin_path}")

    changelog_body = extract_changelog_section(changelog_path, tag)

    sd_dir = sd_subdir(project_kind)
    out_dir.mkdir(parents=True, exist_ok=True)
    sd_root = out_dir / sd_dir
    sd_root.mkdir(parents=True, exist_ok=True)

    sd_bin = sd_root / packed_name
    shutil.copy2(bin_path, sd_bin)

    stem = Path(packed_name).stem
    tagged_bin = out_dir / f"{stem}-{tag}{Path(packed_name).suffix}"
    flat_bin = out_dir / packed_name
    shutil.copy2(bin_path, tagged_bin)
    shutil.copy2(bin_path, flat_bin)

    archive_name = f"{stem}-{slug(tag)}.zip"
    archive_path = out_dir / archive_name
    if archive_path.exists():
        archive_path.unlink()
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(sd_bin, f"{sd_dir}/{packed_name}")

    notes_path = out_dir / "RELEASE_NOTES.md"
    notes_path.write_text(
        build_release_notes(
            tag=tag,
            changelog_body=changelog_body,
            project_kind=project_kind,
            packed_name=packed_name,
            sd_dir=sd_dir,
            core_name=core_name,
            docker_image=resolved_docker,
            archive_name=archive_name,
        ),
        encoding="utf-8",
    )

    release_files = [flat_bin, tagged_bin, archive_path]
    files_path = out_dir / "release-files.txt"
    files_path.write_text(
        "\n".join(p.name for p in release_files) + "\n",
        encoding="utf-8",
    )

    print(f"release_title={tag}")
    print(f"project_kind={project_kind}")
    print(f"packed_bin={packed_name}")
    print(f"sd_path=/{sd_dir}/{packed_name}")
    print(f"archive={archive_path}")
    print(f"notes={notes_path}")
    print(f"files={files_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin",
        dest="bin_path",
        type=Path,
        help="packed .bin to stage (default: PACKED_BIN from Makefile, relative to repo root)",
    )
    parser.add_argument("--tag", required=True, help="release tag (e.g. v1.0.0)")
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("release"),
        help="output directory (default: release/)",
    )
    parser.add_argument(
        "--changelog",
        type=Path,
        default=DEFAULT_CHANGELOG,
        help="changelog file (default: CHANGELOG.md)",
    )
    parser.add_argument(
        "--docker-image",
        help="builder image string for release notes (default: Makefile DOCKER_IMAGE)",
    )
    args = parser.parse_args()

    cfg = read_make_vars()
    bin_path = args.bin_path or (ROOT / cfg["PACKED_BIN"])
    if not bin_path.is_absolute():
        bin_path = ROOT / bin_path

    changelog_path = args.changelog
    if not changelog_path.is_absolute():
        changelog_path = ROOT / changelog_path

    stage_release(
        bin_path=bin_path,
        tag=args.tag,
        out_dir=(ROOT / args.out).resolve(),
        changelog_path=changelog_path,
        docker_image=args.docker_image,
    )


if __name__ == "__main__":
    main()
