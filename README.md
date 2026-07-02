# 3DS Mod Manager

A homebrew mod manager for the Nintendo 3DS that hot-swaps game mods on the
console itself — no PC required after setup. Built with libctru + citro2d.

![version](https://img.shields.io/badge/version-3.0-blue) (private project)

## What it does

Games on the 3DS load mods in two very different ways, and this app handles
both behind one UI:

- **Most games (Luma LayeredFS):** the active mod lives in
  `sdmc:/luma/titles/<TitleID>/` and file reads are redirected by Luma.
- **Super Smash Bros. 3DS (SaltySD):** Smash packs its data inside `dt`/`ls`
  archives, so LayeredFS can't replace individual files. A `code.ips` patch
  (SaltySD) redirects reads to `sdmc:/saltysd/smash/` instead. The manager
  keeps that loader alive (self-healing from a pristine copy) and un/re-wraps
  each mod's `romfs/` layout when swapping, since SaltySD expects the data
  folders at the top level.

Mods are stored centrally in `sdmc:/3ds/3dsmods/<TitleID>/<mod name>/` and
swapped by folder *moves* (rename), so switching is instant and nothing is
ever deleted.

## Features

- Game list with real icons + names read from each installed title's SMDH
  (works for SD, NAND, and game-card titles), with `gamename.txt` overrides
  and an offline fallback table
- One-button mod activate / disable (vanilla) per game
- **Tidy** rescues legacy layouts: loose `luma/titles/<TID>_<mod>` folders,
  `Disabled<TID>` folders, ModMoon slots, stray `saltysd/Slot_N` folders,
  and mods stranded in the SaltySD loader folder
- 6 color themes (SELECT to switch, persisted to SD)
- Fully animated citro2d UI at 60 fps: eased selection, screen transitions,
  auto-dismissing toasts, aurora background — all batched GPU quads
- Boot-time SMDH diagnostics log (`sdmc:/3ds/3dsmods/namelookup.log`)

## Controls

| Button | Action |
| ------ | ------ |
| D-pad  | Move |
| A      | Open game / activate mod |
| X      | Disable mods (vanilla) |
| Y      | Tidy legacy folders into the repo |
| B      | Back |
| SELECT | Theme picker |
| START  | Exit |

## Building

Requires devkitPro (devkitARM + libctru + citro2d) plus Steveice10's
`bannertool` and `makerom` on PATH for the CIA:

```
make        # 3dsx
make cia    # installable CIA
```

On this machine the devkitPro msys2 shell is required:

```
/c/devkitPro/msys2/usr/bin/bash -lc 'cd "<repo>" && make && make cia'
```

## SaltySD notes (hard-won)

- The SaltySD loader `code.ips` must match the exact game code revision.
  The official GitHub v1.2 USA build did **not** match this cart+update
  combo (hooks shifted +0x24) and crashed at boot; the loader bundled with
  older mod packs did match. If Smash data-aborts at boot with a register
  holding an ARM opcode (e.g. `0xE8BD8070`), suspect loader/game mismatch.
- The manager keeps a pristine loader at `3ds/3dsmods/<SmashTID>/code.ips`
  and restores `luma/titles/<SmashTID>/code.ips` from it whenever missing.
- Smash needs update 1.1.7 installed and Luma "Enable game patching" on.

## Credits

- [SaltySD](https://github.com/shinyquagsire23/SaltySD) by shinyquagsire23
- devkitPro / libctru / citro2d
- Built collaboratively with Claude Code
