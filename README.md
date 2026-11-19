# XMI2MID07

A lightweight, cross-platform command-line utility for converting Standard MIDI files (`.mid`) to Miles Sound System Extended MIDI (`.xmi`) and back (`.xmi` to `.mid`).

Written in clean ANSI C with zero external dependencies. Compatible with **Windows XP (32-bit)** up to modern Windows, Linux, and macOS.

## Features

* **Bidirectional Conversion:** Automatically detects input format (`MThd` for MIDI, `FORM` for XMI).
* **Format 0 & Format 1 MIDI Support:** Merges multi-track MIDI files into a single XMI sequence.
* **Accurate Timing:** Uses John Miles' `MIDIFORM` DDA accumulator algorithm for 120 Hz quantization, with full support for Tempo events and legacy SMPTE time divisions.
* **Slim Mode (`-slim`):** Significantly reduces output `.xmi` file size by stripping Aftertouch (`0xA0`/`0xD0`), redundant text meta-events, and duplicate Control Changes while keeping **Pitch Bend** intact.
* **Custom Quantization (`-q:Hz`):** Allows overriding the default 120 Hz quantization rate for files with non-standard timing.
* **Clean XMI-to-MIDI Decoding:** Properly handles variable-length note durations and prevents overlapping note hangs on rapid re-triggers.

## Usage

Open Command Prompt (`cmd`) in the folder containing `xmi2mid.exe` and your music files (or drag and drop a file onto `xmi2mid.exe`).

### Convert MIDI to XMI

Basic conversion:
```cmd
xmi2mid myfile.mid myfile.xmi
```

Reduce file size using `-slim` (removes Aftertouch and duplicate CCs, keeps Pitch Bend):
```cmd
xmi2mid myfile.mid myfile.xmi -slim
```

Adjust quantization speed if the playback tempo in DOS is too fast or too slow (default is `120`):
```cmd
xmi2mid myfile.mid myfile.xmi -slim -q:480
```

If you omit the output filename, it will automatically create `myfile.xmi` in the same directory:
```cmd
xmi2mid myfile.mid
```

### Convert XMI to MIDI

```cmd
xmi2mid myfile.xmi
```
Or specify an explicit output filename:
```cmd
xmi2mid myfile.xmi output.mid
```

> **?? WARNING:** If no output filename is specified (or if the output filename matches an existing file), `xmi2mid` will **OVERWRITE** any existing `.mid` or `.xmi` file with the same name in that folder without asking!

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
