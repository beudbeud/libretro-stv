# Mednafen STV — libretro core

A libretro core for **Sega ST-V (Sega Titan Video)** arcade hardware, wrapping [Mednafen](https://mednafen.github.io/) 1.32.1's Saturn/ST-V emulation module.

---

## Supported games

The core supports all **70 ST-V titles** in the database (64 from Mednafen 1.32.1 + 6 additions), including games that require the optional protection/encryption chips. See **[COMPATIBILITY.md](COMPATIBILITY.md)** for the per-title status and special-hardware details (315-5881/5838, RAX audio, touchscreen, trackball/medal).

---

## BIOS files

Individual files in the RetroArch system directory, **or** a single `stvbios.zip` containing them (TORRENTZIPPED format supported):

| File | Region | SHA-256 |
|------|--------|---------|
| `epr-20091.ic8` | Japan | `ac778ec04aaa4df296d30743536da3de31281f8ae5c94d7be433dcc84e25d85b` |
| `epr-19854.ic8` | Asia / Taiwan | `a2a13f306c1ce85dc5751ab1210697f9f331f384bcd18662ff85f30c6c41b97b` |
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

### Trackball

Trackball games (Hashire Patrol Car, Sky Challenger, Nerae! Super Goal, Technical Bowling) accept three input methods at once — use whichever your controller exposes:

| Input | Action |
|-------|--------|
| Mouse / real trackball | Roll trackball (1:1, unaffected by sensitivity) |
| **Left or right analog stick** | Roll trackball (speed set by sensitivity option) |
| **D-pad** | Roll trackball (fallback, speed set by sensitivity option) |
| B / A / R1 | SW1 (A) / SW2 (B) / SW3 (C) |
| Start | Start |
| **Select** | **Insert Coin** |
| **L3** | **Test Button** |
| **R3** | **Service Button** |

Both analog stick indices are read, so the stick works regardless of which index your frontend maps the physical stick to. Roll speed for the analog/D-pad methods is set by `mednafen_stv_trackball_sensitivity` (see [Input options](#input)); mouse/real-trackball input is always 1:1.

---

## Core options

### System

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_region` | `auto` | BIOS region: Auto / Japan / North America / Europe / Asia |
| `mednafen_stv_cart` | `auto` | Expansion cart: Auto / None / Backup RAM / 4M RAM / 8M RAM |
| `mednafen_stv_skip_bios` | `disabled` | Skip BIOS boot animation. Restart required. |
| `mednafen_stv_autortc` | `enabled` | Auto-set Real Time Clock from host |

### Input

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_crosshair` | `enabled` | Show crosshair overlay for touchscreen games (Critter Crusher) |
| `mednafen_stv_crosshair_color` | `white` | Crosshair colour: White / Red / Green / Blue / Yellow / Cyan |
| `mednafen_stv_trackball_sensitivity` | `100` | Roll speed of the analog stick / D-pad for trackball games (25–400%). Higher rolls faster at full deflection. Does not affect mouse / real-trackball input. |

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

### Audio

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_volume` | `100` | Output volume (50–200%). `100` is unchanged; raise it for quiet titles. Above 100% may clip on loud scenes. Applied live, no restart. |

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
