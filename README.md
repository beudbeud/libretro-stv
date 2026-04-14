# Mednafen STV — libretro core

A libretro core for **Sega ST-V (Sega Titan Video)** arcade hardware, wrapping [Mednafen](https://mednafen.github.io/) 1.32.1's Saturn/ST-V emulation module. 

---

## Supported games

The core supports all **64 ST-V titles** in the Mednafen 1.32.1 database, including games that require the optional protection/encryption chips:

### 315-5881 encryption chip
| Game | Status |
|------|-----|
| Astra SuperStars | `Boot, Imperfect |
| Final Fight Revenge | `Boot, Imperfect |
| Steep Slope Sliders | `Unknown |
| Tecmo World Cup '98 | `No Boot` |
| Touryuu Densetsu Elan Doree | `Unknown` |

### 315-5838 compression + encryption chip
| Game |
|------|
| Decathlete (V1.000) |
| Decathlete (V1.001) |

The 315-5838 combines Decathlete-specific 16-bit decryption (`decipher()`) with a 12-level Huffman decompressor. The game uploads tree and dictionary tables at startup via the chip's register interface.

> **Status**: Decathlete graphics are currently not decompressed correctly.

### Batman Forever

> **Status**: No sound, because this game use a extra Qsound card

---

## BIOS files

| File | Region | SHA-256 |
|------|--------|---------|
| `epr-20091.ic8` | Japan | `ac778ec04aaa4df296d30743536da3de31281f8ae5c94d7be433dcc84e25d85b` |
| `epr-17952a.ic8` | North America | `bac5a52794cf424271f073df228e0b0eb042dede6a3b829eb49abf155e7e0137` |
| `epr-17954a.ic8` | Europe | `3e6f91506031badc4ebdf7fe5b4f33180222a369b575522861688d3b27322a68` |

---

## Input mapping

The core follows the same button mapping as **Beetle Saturn** (`beetle-saturn-libretro`):

| RetroArch button | Saturn button | ST-V function |
|-----------------|---------------|---------------|
| B | **A** | SW1 |
| A | **B** | SW2 |
| R1 | **C** | SW3 |
| Y | **X** | SW4 |
| X | **Y** | SW5 |
| L1 | **Z** | SW6 |
| L2 | **L** (Left Shoulder) | — |
| R2 | **R** (Right Shoulder) | — |
| Start | Start | Start |
| **Select** | — | **Insert Coin** |
| **L3** (P1) | — | **Test Button** |
| **R3** (P1) | — | **Service Button** |
| **R3** (P2) | — | **Pause Button** |

Coin insertion uses `MDFN_QSimpleCommand(MDFN_MSC_INSERT_COIN)` with edge detection (one press = one coin).

---

## Core options

| Option | Default | Description |
|--------|---------|-------------|
| `mednafen_stv_region` | `jp` | BIOS region (Japan / NA / Europe / Auto) |
| `mednafen_stv_h_overscan` | `enabled` | Show horizontal overscan pixels |
| `mednafen_stv_h_blend` | `disabled` | Horizontal blend filter (anti-dithering) |
| `mednafen_stv_correct_aspect` | `enabled` | Correct pixel aspect ratio |
| `mednafen_stv_slstart` | `8` | First displayed NTSC scanline (0–239) |
| `mednafen_stv_slend` | `231` | Last displayed NTSC scanline (0–239) |
| `mednafen_stv_sound_quality` | `4` | SCSP resampler quality (0=lowest, 4=highest) |
| `mednafen_stv_bios_sanity` | `enabled` | Verify BIOS SHA-256 checksums at load |
| `mednafen_stv_autortc` | `enabled` | Auto-set Real Time Clock from host |

---

## Build

### CMake (local / x86_64 for testing)

```bash
git clone <this-repo>
cd mednafen
cmake -B build_lr
cmake --build build_lr -j$(nproc)
# Output: build_lr/libretro/mednafen_stv_libretro.so
```

### Cross-compile 

```bash
make -f libretro/Makefile.libretro TARGET_CROSS=aarch64-buildroot-linux-gnu-
```

## Credits

- **Mednafen Team** — SS/ST-V emulation core
- **Andreas Naive, Olivier Galibert, David Haywood** — 315-5881 cipher research (MAME)
- **David Haywood, Samuel Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard** — 315-5838 decompressor (MAME)
- **Beetle Saturn authors** — libretro wrapper reference
- **Beetle PCE Fast authors** — geometry / input reference
