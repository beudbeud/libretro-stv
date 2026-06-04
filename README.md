# Mednafen STV — libretro core

A libretro core for **Sega ST-V (Sega Titan Video)** arcade hardware, wrapping [Mednafen](https://mednafen.github.io/) 1.32.1's Saturn/ST-V emulation module.

---

## Supported games

The core supports all **70 ST-V titles** in the database (64 from Mednafen 1.32.1 + 6 additions), including games that require the optional protection/encryption chips:

### 315-5881 encryption chip
| Game | Status |
|------|--------|
| Astra SuperStars | Working |
| Final Fight Revenge | Working |
| Steep Slope Sliders | Working |
| Tecmo World Cup '98 | Working |
| Tecmo World Soccer '98 | Working |
| Touryuu Densetsu Elan Doree | Working |

### 315-5838 decompression + encryption chip
| Game | Status |
|------|--------|
| Decathlete (V1.000) | Working |
| Decathlete (V1.001) | Working |

The 315-5838 combines Decathlete-specific 16-bit decryption (`decipher()`) with a 12-level Huffman decompressor. The game uploads tree and dictionary tables at startup via the chip's register interface. ROM bank selection matches MAME's `decathlt_prot_srcaddr_w` (SH-2 address bits [24:23] → 8 MB bank window).

### Touchscreen — Critter Crusher

Critter Crusher uses a resistive touchscreen panel wired to the JAMMA I/O connector. The hardware reports a touched position as a 6-bit X (0–62) and 6-bit Y (0–46) grid, with a hit-bit in the I/O register.

| Game | Status |
|------|--------|
| Critter Crusher | Working |

The frontend's pointer device (`RETRO_DEVICE_POINTER`) is used for input — this covers mouse on desktop, touchscreen on mobile/Android, and light-gun mappings. A crosshair overlay can be shown and coloured via core options (see [Input options](#input)).

---

### Acclaim RAX sound board — Batman Forever

Batman Forever uses an external audio expansion board (**Acclaim RAX**) plugged into the ST-V cartridge slot. The RAX contains an **ADSP-2181 DSP**, four 2 MB sample ROMs, and a 512 KB program ROM. The DSP runs a custom firmware that handles sound commands from the SH-2, mixes PCM voices, and streams audio via the SPORT0 serial port.

| Game | Status |
|------|--------|
| Batman Forever | Working (full audio) |

**Implementation notes:**

- Standalone ADSP-2181 CPU emulator adapted from MAME's `adsp2100.cpp` (Aaron Giles, BSD-3-Clause).
- BDMA transfers load firmware segments into DSP program memory; the synthesis engine arms itself via a PM[0x001C] hook patched each BDMA cycle.
- The BDMA IRQ is pulsed (assert + deassert in one step) to mimic MAME's `pulse_input_line`, ensuring a fresh rising edge for every transfer — matching the hardware's edge-triggered latch behaviour.
- Audio is sampled at 44 100 Hz (378 DSP cycles/sample) and mixed into the libretro audio ring buffer via the SPORT0-TX autobuffer mechanism.

---

## BIOS files

Individual files in the RetroArch system directory, **or** a single `stvbios.zip` containing them (TORRENTZIPPED format supported):

| File | Region | SHA-256 |
|------|--------|---------|
| `epr-20091.ic8` | Japan | `ac778ec04aaa4df296d30743536da3de31281f8ae5c94d7be433dcc84e25d85b` |
| `epr-19854.ic8` | Asia / Taiwan | — |
| `epr-17952a.ic8` | North America | `bac5a52794cf424271f073df228e0b0eb042dede6a3b829eb49abf155e7e0137` |
| `epr-17954a.ic8` | Europe | `3e6f91506031badc4ebdf7fe5b4f33180222a369b575522861688d3b27322a68` |

---

## Save states & rewind

Save states and RetroArch rewind are fully supported. The state size is computed automatically at first use (typically 5–7 MB depending on the game), so no manual size tuning is required.

---

## Input mapping

### Gamepad (3-button / 6-button games)

Action buttons are mapped ergonomically to face buttons:

| RetroArch button | 3-button games | 6-button games |
|-----------------|----------------|----------------|
| B | SW1 | SW1 |
| A | SW2 | SW2 |
| Y | SW3 | SW3 |
| X | — | SW4 |
| L1 | — | SW5 |
| R1 | — | SW6 |
| Start | Start | Start |
| **Select** | **Insert Coin** | **Insert Coin** |
| **L3** (P1) | **Test Button** | **Test Button** |
| **R3** (P1) | **Service Button** | **Service Button** |
| **R3** (P2) | **Pause Button** | **Pause Button** |

> **Batman Forever** (3-button with non-standard wiring): Jump → B, Punch → A, Kick → Y.

Coin insertion uses edge detection (one press = one coin).

Tate (vertical cabinet) games are detected automatically from the game database and reported to the frontend via `SET_ROTATION`. The `mednafen_stv_rotation` option overrides this for special setups (e.g. a physically-rotated screen).

### Touchscreen (Critter Crusher)

Critter Crusher is controlled entirely via `RETRO_DEVICE_POINTER`:

| Input | Action |
|-------|--------|
| Pointer move | Move cursor over target |
| Pointer press / click | Hit |
| **Select** | **Insert Coin** |
| **L3** | **Test Button** |
| **R3** | **Service Button** |

Works with mouse (desktop), touchscreen (Android / mobile), or any pointer device the frontend exposes. An optional crosshair overlay shows the current pointer position (see [Input options](#input)).

---

## Core options

### System

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_region` | `auto` | BIOS region: Auto / Japan / North America / Europe |
| `mednafen_stv_cart` | `auto` | Expansion cart: Auto / None / Backup RAM / 4M RAM / 8M RAM |
| `mednafen_stv_skip_bios` | `disabled` | Skip BIOS boot animation. Restart required. |
| `mednafen_stv_autortc` | `enabled` | Auto-set Real Time Clock from host |

### Input

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_crosshair` | `enabled` | Show crosshair overlay for touchscreen games (Critter Crusher) |
| `mednafen_stv_crosshair_color` | `white` | Crosshair colour: White / Red / Green / Blue / Yellow / Cyan |

### Video

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_correct_aspect` | `enabled` | Correct pixel aspect ratio |
| `mednafen_stv_h_overscan` | `enabled` | Show horizontal overscan pixels |
| `mednafen_stv_h_blend` | `disabled` | Horizontal blend filter (anti-dithering) |
| `mednafen_stv_mesh_transparency` | `disabled` | Replace VDP1 stipple (checkerboard) with 50% blend for smoke/shadow/fade effects. 16-bit framebuffer only. |
| `mednafen_stv_deinterlacer` | `blend` | 480i handling: Blend (smooth, recommended) / Off (renderer bob) / Weave / Bob / Bob with offset / Blend gamma-correct |
| `mednafen_stv_slstart` | `8` | First displayed NTSC scanline (0 / 2 / 4 / 8) |
| `mednafen_stv_slend` | `231` | Last displayed NTSC scanline (224 / 231 / 234 / 239) |
| `mednafen_stv_rotation` | `auto` | Display rotation. `auto` follows the game database (TATE games rotated 90°, yoko games unrotated). Force `0` / `90` / `180` / `270` for special setups (e.g. `0` on a physically-rotated screen). |

### Performance

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_frameskip` | `disabled` | Frameskip: disabled / auto / 1–5 |
| `mednafen_stv_cpu_cache` | `data_cb` | SH-2 cache emulation: Fast (recommended) / Full (accurate, slow). Restart required. |

---

## Build

### CMake (local / x86_64)

```bash
git clone <this-repo>
cd libretro-stv
cmake -B build_lr
cmake --build build_lr -j$(nproc)
# Output: build_lr/libretro/mednafen_stv_libretro.so
```

### Cross-compile (e.g. aarch64 / Raspberry Pi)

```bash
make -f libretro/Makefile.libretro TARGET_CROSS=aarch64-buildroot-linux-gnu-
```

### Android (arm64-v8a)

```bash
make -f libretro/Makefile.libretro platform=android_arm64
```

**Note on zstd linking:** cross-compile uses dynamic `libzstd.so`; native builds link static `libzstd.a` to avoid a runtime dependency.

---

## Credits

- **Mednafen Team** — SS/ST-V emulation core
- **Andreas Naive, Olivier Galibert, David Haywood** — 315-5881 cipher research (MAME)
- **David Haywood, Samuel Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard** — 315-5838 decompressor (MAME)
- **Aaron Giles** — ADSP-2181 CPU core (MAME `adsp2100.cpp`, BSD-3-Clause)
- **Beetle Saturn authors** — libretro wrapper reference
- **Beetle PCE Fast authors** — geometry / input reference
