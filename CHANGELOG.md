# Changelog

Doom CORE for [Game & Watch Retro-Go SD](https://github.com/sylverb/game-and-watch-retro-go-sd).

This file follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Release tags must
match a section heading exactly (for example `v1.0.0`).

When you cut a release:

1. Move items from `[Unreleased]` into a new `## [vX.Y.Z] - YYYY-MM-DD` section.
2. Commit the changelog update.
3. Push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`

CI reads the matching section and uses it as the GitHub Release notes.

## [Unreleased]

### Added

- Integrate gnw-doom engine + G&W platform layer into this single-project tree
  (`/cores/doom.bin`, WHDs under `/roms/doom/`)

### Changed

- Adapt `src/gnw/abi_stubs.c` to the current firmware ABI (direct audio/LCD/input
  slots; `*_ctl` folding was reverted upstream)
