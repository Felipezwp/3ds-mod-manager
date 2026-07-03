# Changelog

All notable changes to the 3DS Mod Manager. Versions before this repo was
created (v3.0) are reconstructed from session notes — early entries are
approximate.

## v3.3.2 — 2026-07-03

- Swipe to scroll: drag anywhere on the list and it follows your finger a
  row at a time (clamped at the ends); a release without movement is a tap
  (select / activate as before). Pure input math — zero I/O per frame.

## v3.3.1 — 2026-07-03

- Fix crash at launch: v3.3's ndspInit data-aborted because the exheader
  lacked the DSP memory-region mapping (adding the dsp::DSP service alone
  isn't enough). Added IORegisterMapping 1ff00000-1ff7ffff + VRAM mapping
  to the RSF, matching FBI's template.

## v3.3 — 2026-07-03

- **Touchscreen support**: tap a row to highlight it, tap it again to
  open/activate; tap the `<` corner of the header to go back; theme picker
  fully tappable (live preview on tap)
- **UI sounds** via ndsp: synthesized move/confirm/back/error blips (no
  audio assets; needs a dumped dspfirm.cdc, silently disabled otherwise)
- L / R cycle themes from any screen
- SD free space shown in the top-screen footer

## v3.2.2 — 2026-07-03

- Fix false "Name lookup failed" warning on every warm boot: the SD cache
  made real SMDH probes rare, so the zero-hit heuristic only ever saw the
  always-failing probes (ejected cart, uninstalled titles). Cached names
  now count as resolved.

## v3.2.1 — 2026-07-02

- Fix X-launch exiting to HOME instead of starting the game — and, worse,
  wedging NS so the *next* app launched black-screened (reboot clears it).
  After `APT_DoApplicationJump` the app must keep pumping `aptMainLoop()`
  and let the system terminate it; exiting on our own cancelled the pending
  jump mid-handshake.

## v3.2 — 2026-07-02

- Launch the highlighted game straight from the app: press X on the game
  list. Detects whether the title lives on SD, game card, or NAND, fades
  out, and APT-jumps into the game — activate a mod and go.

## v3.1 — 2026-07-02

- **Much faster launch.** Names and icons now persist in an SD cache
  (`3ds/3dsmods/.cache/`), so the up-to-three SMDH archive probes per title
  happen once ever instead of every boot; unresolvable system titles are
  never re-probed (game carts still are, in case one was inserted). The
  game-card slot is skipped entirely when empty, and `luma/titles` is
  listed once per boot instead of once per game.
- Fade-to-black animation when quitting with START
- Battery indicator in the top header (green/yellow/red, pulses while
  charging), polled via ptm:u every ~2 s

## v3.0.1 — 2026-07-02

- Games with zero mods (and nothing active or tidy-able) are dropped from
  the list after the boot scan — kills leftover-folder ghosts like
  CTRXplorer for good, regardless of which folder scan registered them
- Mod lists are cached from the boot scan and kept in sync by every
  action, so opening a game's mod menu is now zero SD reads (previously
  each entry re-read every mod's name markers — slow for Smash's 11 mods).
  External SD changes made mid-session appear after a relaunch.

## v3.0 — 2026-07-02 — "Release 1" (first tagged release)

UI motion overhaul:
- Selection highlight glides between rows (eased, with shadow)
- Staggered row slide-in on every screen change
- Status toasts slide in, auto-dismiss after ~4 s, slide away
- Aurora-style layered glow blobs in the background
- Drop shadows under cards; rounded card list rows (28 px) replace zebra
  stripes; eased scrollbar thumb; accent hairline over the hint bar
- Game rows show mod-count subtext next to the ON badge

## v2.7 — 2026-07-02

- Empty leftover `luma/titles` folders are no longer listed as games
- System titles without an SMDH get readable category labels
  ("System applet BC02") instead of raw Title IDs; YouTube + CTRXplorer
  added to the offline name table
- Backing out of a game's mod menu costs zero SD reads (stats derived from
  the in-memory list; previously every backout re-scanned every game)
- SaltySD pill no longer overlaps the Title ID

## v2.6 — 2026-07-02

- New HOME menu icon, banner, and jingle (all generated programmatically)
- Faster back-and-forth: per-game stat refresh instead of full rescan

## v2.5 — 2026-07-02

- Game icons: each title's 48x48 SMDH icon shown in the game list and
  detail cards (SMDH icons are stored GPU-tiled; copied straight into a
  64x64 RGB565 texture)
- Clock in the top header
- SMDH name lookup fixed: FS rejects IPC read buffers on the app stack
  (0xE0C046F9) — buffer moved to static memory and the full 0x36C0 SMDH is
  read, matching FBI's behavior byte-for-byte
- Boot-time diagnostics log at `sdmc:/3ds/3dsmods/namelookup.log`

## v2.4 — 2026-07-02

- **Fixed Smash mods** (the big one). Two independent bugs:
  1. Mods store data as `<mod>/romfs/...` but SaltySD reads the data
     folders directly from `saltysd/smash/` — activation now unwraps the
     `romfs/` layer and disable/tidy re-wraps it.
  2. The SaltySD loader that ships on GitHub (v1.2 USA) targets a
     different code revision than this cart (every hook shifted +0x24) and
     data-aborted at boot; diagnosed via Luma crash dumps + IPS record
     comparison, fixed by installing the loader from the old working
     ModMoon-era mod pack.
- Loader self-heal now prefers a pristine `code.ips` kept at the repo root
- First FTP-based deploy workflow (ftpd + curl)

## v2.3 — 2026-07-01

- SaltySD support for Super Smash Bros.: active Smash mod lives in
  `saltysd/smash`, loader kept alive in `luma/titles/<TID>/`
- 6 color themes with SELECT picker, persisted to `settings.txt`

## v2.2 — 2026-07-01

- Game names resolved from installed titles' SMDH metadata
  (gamename.txt > SMDH > offline table > raw ID)
- Animated UI: dusk gradient, drifting particles, pulsing selection,
  colored badges (ON / ACTIVE / LOOSE)

## v2.1 — 2026-07-01

- Full citro2d GPU UI (Tokyo Night dark theme, system-font button glyphs,
  pills, badges, scrollbar, toasts) — replaced the text console UI
- Name-preserving swaps: active mods return to the repo under their own
  name, never deleted
- New Title ID 0x5BD37 (old 0xFF3FF collided with another installed app)

## v2.0 — 2026-06-08

- Central mod repository layout: `sdmc:/3ds/3dsmods/<TitleID>/<mod>/`
- Tidy (Y) migrates legacy locations on-console: loose `<TID>_<mod>` and
  `Disabled<TID>` folders in luma/titles, ModMoon slots

## v1.0 — 2026-06-07

- First working version: console (text) UI, LayeredFS folder swapping in
  `luma/titles`, built with libctru
