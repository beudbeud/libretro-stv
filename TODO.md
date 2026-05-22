# TODO

## Compatibility — 315-5881 encryption chip

All five cipher-protected games now boot. Two independent bugs were fixed:

1. **ROM layout** — `ROM_RELOAD_PLAIN` entries must use `STV_MAP_16BE` so that
   `rom_read()` returns the same byte order as MAME's `crypt_read_callback`.
2. **Enable-bit check** — our read handler incorrectly gated decryption behind
   `protenable & 0x00010000`. MAME has no such check: reading from offset 3
   (0x04FFFFFC) always returns decrypted data.

Fixed games: Astra SuperStars, Steep Slope Sliders, Tecmo World Cup '98,
Tecmo World Soccer '98, Touryuu Densetsu Elan Doree (untested).

---

## Compatibility — Batman Forever (Acclaim RAX soundboard)

Batman Forever boots but has no sound. The game requires **Acclaim's RAX soundboard**,
an external PCM/DSP audio expansion not part of the base ST-V hardware. No emulation
exists for it yet anywhere.

---

## Compatibility — Mahjong controller

Two games require the arcade mahjong tile controller (16-button layout mapped to the
ST-V I/O board). Currently both are `// Broken(needs special controller)` in `db.cpp`.

- **Virtual Mahjong**
- **Virtual Mahjong 2: My Fair Lady**

The controller mapping is known from MAME. Pro Mahjong Kiwame S uses a standard
3-button layout and is likely unaffected.

---

## Compatibility — Generic broken games (cause unknown)

These games boot-fail for undocumented reasons inherited from Mednafen's original
comments. They need to be tested in the core and diagnosed:

- Choro Q Hyper Racing 5
- Dancing Fever Gold
- Hashire Patrol Car
- Magical Zunou Power
- Microman Battle Charge
- Nerae Super Goal
- Sky Challenger
- Soreyuke Anpanman Crayon Kids
- Stress Busters
- Technical Bowling
- Yatterman Plus

Some of these may already boot — the `// Broken` tags were not re-verified for ST-V.

---

## VDP1 — Draw timing accuracy

The `HORRIBLEHACK_VDP1RWDRAWSLOWDOWN` is a global per-game slowdown that
approximates SH-2 bus contention during VDP1 command execution. Known gaps
documented in `vdp1.cpp`:

- Draw timing for small lines (2–15 px wide) is inaccurate — needs hardware measurement
- The 10–20% command overhead in `AdjustDrawTiming()` is a rough estimate
- Shrunken sprite lines with HSS disabled should cost 2× more cycles (currently not
  accounted for)
- `HORRIBLEHACK_VDP1INSTANT` for Astra SuperStars masks a real FB-swap vs
  command-list timing issue that should be fixed properly

---

## VDP2 — Known issues

- **31KHz monitor mode** not implemented (`vdp2_render.cpp:22`); some ST-V titles
  may switch to this mode for higher resolution output
- **Window coordinate wrap** (`vdp2_render.cpp:2905–2913`): coordinates ≥ 0x380
  are clamped via kludge; correct behavior not yet understood
- **Color modes 5–7** on NBG layers: clamped to 4 with `TODO: Test 5...7`; probably
  unused by ST-V titles but unverified

---

## VDP2 threading option

The VDP2 render thread (`mednafen_stv_vdp2_thread` core option) is exposed but not
validated across all games. Some race conditions may exist under multithreaded
rendering that are hidden in single-threaded mode.
