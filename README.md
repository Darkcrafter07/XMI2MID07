# XMI2MID07

A cross-platform ANSI C utility (`XMI2MID.h` / `XMI2MID.c`) for bidirectional conversion and slicing between Standard MIDI (`.mid`) and Miles Sound System Extended MIDI (`.xmi`).

## Features

* **Bidirectional Conversion:** Converts between MIDI (Format 0/1) and XMI.
* **Accurate Timing:** Uses John Miles' `MIDIFORM` DDA accumulator algorithm with tempo and SMPTE support.
* **Slim Mode (`-slim`):** Optimizes output by removing redundant control messages.
* **Exact Byte-Size Slicing (`-slice <bytes>`):** Splits files into chunks while preserving active state, instruments, and controllers at boundaries.

## Usage

Run `xmi2mid` via the command line for conversion or slicing operations.

## Building from Source

Compile using CMake for Windows, Linux, or macOS.

## License

Licensed under the **MIT License**.
