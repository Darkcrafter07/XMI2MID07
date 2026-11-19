//==============================================================================
// XMI2MID.h
// XMI2MID07 Utility by Darkcrafter07
// License: MIT. I do NOT take any responsibility for any potential damage.
//
// Architecture:
//   1. Sorting & Binary Encoding Helpers (VLQ, XMI Delay, Path Builders)
//   2. Shared Synthesizer Channel State Tracker (used by both slicers)
//   3. File Writers (Standard MIDI & IFF XMIDI containers)
//   4. XMI -> MIDI Pipeline (IFF Parser -> 60PPQN Decoder -> Single/Sliced Writer)
//   5. MIDI -> XMI Pipeline (MTrk Merger -> DDA 120Hz Quantizer -> Note Pairer -> Writer)
//   6. Directory Scanning & File I/O Helpers
//   7. File Merging Engine (-merge & -mergename for batch/explicit concatenation)
//   8. CLI Argument Parser & Main Entry Point
//
// Features:
//   - Bidirectional conversion between Standard MIDI (.mid) and XMIDI (.xmi)
//   - John Miles MIDIFORM DDA 120Hz quantization logic + legacy SMPTE fix
//   - Overlapping note detector during XMI -> MIDI decoding
//   - Slim mode (-slim): strips Aftertouch & duplicate CCs while keeping Pitch Bend
//   - Exact byte-size slicing (-slice <bytes>) preserving instruments, volume, pan,
//     tempo, Pitch Bend, and Pitch Bend Sensitivity (RPN 0,0) across slice boundaries
//   - Seamless file merging (-merge / -mergename) for both .mid and .xmi files
//==============================================================================

#ifndef XMI2MID_H
#define XMI2MID_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

//------------------------------------------------------------------------------
// Cross-platform compatibility macros (MSVC vs GCC/Clang on Linux/macOS)
//------------------------------------------------------------------------------
#ifndef _WIN32
    #define _byteswap_ulong __builtin_bswap32
    #define _byteswap_ushort __builtin_bswap16
    #define _stricmp strcasecmp
    #define fopen_s(fp, name, mode) ((*(fp) = fopen((name), (mode))) == NULL ? errno : 0)
    #define fread_s(buf, bsz, esz, cnt, fp) fread((buf), (esz), (cnt), (fp))
#endif

//------------------------------------------------------------------------------
// MIDI & XMIDI Timing and Format Constants
//------------------------------------------------------------------------------
#define DEFAULT_TEMPO       120UL       // Standard MIDI default tempo (120 BPM)
#define XMI_FREQ            120UL       // Miles Sound System XMIDI fixed tick rate (120 Hz)
#define DEFAULT_TIMEBASE    60UL        // XMIDI base resolution (60 ticks per quarter note @ 120 BPM)
#define DEFAULT_QN          500000UL    // Default quarter-note duration in microseconds (120 BPM)
#define DEFAULT_MID_TPQN    960         // Output Standard MIDI resolution (TPQN)

//------------------------------------------------------------------------------
// Buffer Sizes and Protocol Limits (Named constants prevent array index issues)
//------------------------------------------------------------------------------
#define HDR_SIZE            18          // Length of static MThd + MTrk header template
#define MAX_OFF_EVENTS      2048        // Maximum simultaneous active notes tracked during XMI->MID
#define MAX_CHANNELS        16          // Standard MIDI channel count (0..15)
#define MAX_NOTES           128         // Standard MIDI note/CC range (0..127)
#define MAX_TIMB_CNT        256         // Maximum unique patch@bank entries in XMI TIMB chunk
#define PATH_BUF_SIZE       512         // Safe file path buffer length
#define SUFFIX_BUF_SIZE     32          // Slice filename suffix buffer length ("_01", "_02", ...)
#define MIN_SLICE_BYTES     128         // Minimum allowed slice size in bytes
#define INITIAL_EVENT_CAP   16384       // Initial capacity for dynamic MIDI event array

//------------------------------------------------------------------------------
// Shared Data Structures
//------------------------------------------------------------------------------

// Tracks pending Note-Off events when decoding XMIDI durations into MIDI
struct NOEVENTS
{
    unsigned int delta;                 // Remaining ticks until Note-Off triggers
    unsigned char off_status;           // Original Note-On status byte (0x90 | channel)
    unsigned char off_note;             // MIDI note pitch (0..127)
    unsigned char off_vel;              // Release velocity (default 0x7F)
};

// Represents a single flattened MIDI event across merged tracks for MID->XMI
struct RawMidiEvent
{
    unsigned long abs_tick;             // Absolute timestamp (first in MIDI ticks, later in 120Hz XMI ticks)
    unsigned char status;               // MIDI status byte (e.g., 0x90, 0xB0, 0xE0, 0xFF)
    unsigned char d1;                   // First data byte (note, CC number, meta type)
    unsigned char d2;                   // Second data byte (velocity, CC value, pitch MSB)
    unsigned char *meta_data;           // Pointer to Meta/SysEx payload in source buffer
    unsigned long meta_len;             // Length of Meta/SysEx payload in bytes
    unsigned long duration_ticks;       // Calculated note duration in XMI ticks (for 0x90 events)
    int skip;                           // Flag: 1 if event was consumed (e.g., paired Note-Off)
    unsigned long seq_order;            // Original file order tie-breaker for identical timestamps
};

// Shared per-channel synthesizer state used across slice boundaries
struct ChannelState
{
    unsigned char prog[MAX_CHANNELS];       // Active instrument patch (0xFF = unset)
    unsigned char bank[MAX_CHANNELS];       // Active Bank Select MSB (CC 0)
    unsigned char vol[MAX_CHANNELS];        // Active Channel Volume (CC 7, 0xFF = unset)
    unsigned char pan[MAX_CHANNELS];        // Active Pan position (CC 10, 0xFF = unset)
    unsigned char pitch_lsb[MAX_CHANNELS];  // Pitch Bend LSB (default 0x00)
    unsigned char pitch_msb[MAX_CHANNELS];  // Pitch Bend MSB (default 0x40 = center)
    unsigned char has_pitch[MAX_CHANNELS];  // Flag: 1 if Pitch Bend was ever used on this channel
    unsigned char rpn_msb[MAX_CHANNELS];    // Last RPN MSB (CC 101)
    unsigned char rpn_lsb[MAX_CHANNELS];    // Last RPN LSB (CC 100)
    unsigned char rpn_range[MAX_CHANNELS];  // Pitch Bend sensitivity in semitones (RPN 0,0 -> CC 6)
    unsigned char has_rpn[MAX_CHANNELS];    // Flag: 1 if custom Pitch Bend range was set
	unsigned char active_vel[MAX_CHANNELS][MAX_NOTES];    // To chop played notes
	unsigned long note_end_tick[MAX_CHANNELS][MAX_NOTES]; // To chop played notes
};

//------------------------------------------------------------------------------
// Public Conversion API
//------------------------------------------------------------------------------

void convert_xmi_to_mid
(
    unsigned char *midi_data,
    long fsize,
    const char *in_filename,
    const char *out_filename,
    unsigned int slice_max_bytes
);

void convert_mid_to_xmi
(
    unsigned char *midi_data,
    long fsize,
    const char *in_filename,
    const char *out_filename,
    int slim_mode,
    unsigned int quant_hz,
    unsigned int slice_max_bytes
);

//------------------------------------------------------------------------------
// Merge Mode Constants & API
//------------------------------------------------------------------------------
#define MAX_MERGE_FILES     256         // Maximum files in a single merge batch

// Merges an explicit list of MIDI/XMI files into a single output file
void merge_files_list
(
	const char **file_list,
	int file_count,
	const char *out_filename,
	int slim_mode,
	unsigned int quant_hz
);

// Scans current directory and merges files matching root prefix (or all groups if root == NULL)
void merge_directory_by_root
(
	const char *root_prefix,
	const char *out_filename,
	int slim_mode,
	unsigned int quant_hz
);

#endif // XMI2MID_H


