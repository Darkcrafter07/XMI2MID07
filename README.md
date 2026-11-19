# XMI2MID07

A cross-platform, modular ANSI C command-line utility (`XMI2MID.h` / `XMI2MID.c`) for bidirectional conversion, exact byte-size slicing, and seamless merging between Standard MIDI (`.mid`) and Miles Sound System Extended MIDI (`.xmi`).

Built with zero external dependencies and compatible with **Windows XP (32-bit)** up to modern Windows, Linux, and macOS.

## Features

* **Bidirectional Conversion:** Automatically detects input format (`MThd` for MIDI, `FORM` for XMI) and converts in either direction.
* **Format 0 & Format 1 MIDI Support:** Merges multi-track MIDI files into a single XMI sequence.
* **Accurate Timing:** Uses John Miles' `MIDIFORM` DDA accumulator algorithm for 120 Hz quantization, with full support for Tempo meta-events and legacy SMPTE time divisions.
* **Custom Quantization (`-q:Hz`):** Overrides the default 120 Hz quantization rate for files with non-standard timing.
* **Slim Mode (`-slim`):** Reduces output `.xmi` size by stripping Aftertouch (`0xA0`/`0xD0`), redundant text meta-events, and duplicate Control Changes while preserving **Pitch Bend**.
* **Exact Byte-Size Slicing (`-slice <bytes>`):** Splits `.mid` or `.xmi` files into valid, self-contained chunks (`_01`, `_02`, ...) strictly under the specified byte limit, cleanly closing active notes and restoring instruments, volume, pan, tempo, Pitch Bend, and Pitch Bend Sensitivity (`RPN 0,0`) across boundaries.
* **Seamless File Merging (`-merge` & `-mergename`):** Concatenates multiple `.mid` or `.xmi` files back into a single continuous track with accurate timeline offsets.
* **Clean XMI-to-MIDI Decoding:** Handles variable-length note durations and prevents hanging/overlapping notes on rapid re-triggers.

## Usage

Open Command Prompt (`cmd`) or terminal in the folder containing `xmi2mid` and your music files.

### 1. Convert MIDI <-> XMI

Convert MIDI to XMI:
```cmd
xmi2mid myfile.mid myfile.xmi
```

Convert MIDI to XMI with `-slim` optimization:
```cmd
xmi2mid myfile.mid myfile.xmi -slim
```

Override quantization frequency (default is `120`):
```cmd
xmi2mid myfile.mid myfile.xmi -slim -q:480
```

Convert XMI back to MIDI (omitting the output filename automatically creates `myfile.mid`):
```cmd
xmi2mid myfile.xmi
```

### 2. Slice Files by Exact Byte Size (`-slice`)

Split a MIDI into `.xmi` chunks no larger than `65536` bytes each (`myfile_01.xmi`, `myfile_02.xmi`, ...):
```cmd
xmi2mid myfile.mid myfile.xmi -slim -slice 65536
```

Split an XMI into `.mid` chunks no larger than `32768` bytes each:
```cmd
xmi2mid myfile.xmi myfile.mid -slice 32768
```

### 3. Merge Multiple Files (`-merge` & `-mergename`)

Auto-detect and merge all sliced groups (`*_01.xmi`, `*_01.mid`) in the current directory:
```cmd
xmi2mid -merge
```

Auto-merge only files matching a specific root prefix (e.g., `e1l4_01.xmi`, `e1l4_02.xmi`, ... into `e1l4_merged.xmi`):
```cmd
xmi2mid -merge e1l4
```

Merge an explicit list of files in exact order (space-separated, no commas):
```cmd
xmi2mid -mergename intro.xmi loop.xmi outro.xmi full.xmi
```

> **?? WARNING:** When converting or merging with an explicit or auto-generated output filename, `xmi2mid` will **OVERWRITE** any existing file with the same name in that directory without prompting!

## Building from Source

### Windows XP 32-bit (MSVC 2017 `v140_xp`)
```cmd
mkdir build_xp
cd build_xp
cmake .. -G "Visual Studio 15 2017" -A Win32 -T v140_xp -DBUILD_FOR_WINXP=ON
cmake --build . --config Release
```

### Modern Windows / Linux / macOS
```bash
mkdir build
cd build
cmake .. -DBUILD_FOR_WINXP=OFF
cmake --build . --config Release
```

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
