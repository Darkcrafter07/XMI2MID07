//==============================================================================
// XMI2MID.c
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

#include "XMI2MID.h"

// Static Standard MIDI Format 0 header template (14 bytes MThd + 4 bytes "MTrk")
static unsigned char midiheader[HDR_SIZE] =
{
	'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 60, 'M', 'T', 'r', 'k'
};

static unsigned short timebase = DEFAULT_MID_TPQN;
static unsigned int qnlen = DEFAULT_QN;
static struct NOEVENTS off_events[MAX_OFF_EVENTS];

//==============================================================================
// 1. Sorting & Binary Encoding Helpers
//==============================================================================

// Comparator for qsort: orders pending Note-Offs by nearest expiration tick
static int comp_events(const void *pa, const void *pb)
{
	const struct NOEVENTS *a = (const struct NOEVENTS *)pa;
	const struct NOEVENTS *b = (const struct NOEVENTS *)pb;

	if (a->delta < b->delta)
	{
		return -1;
	}
	if (a->delta > b->delta)
	{
		return 1;
	}
	return 0;
}

// Comparator for qsort: orders merged MIDI events chronologically
static int comp_raw_events(const void *pa, const void *pb)
{
	const struct RawMidiEvent *ea = (const struct RawMidiEvent *)pa;
	const struct RawMidiEvent *eb = (const struct RawMidiEvent *)pb;

	if (ea->abs_tick < eb->abs_tick)
	{
		return -1;
	}
	if (ea->abs_tick > eb->abs_tick)
	{
		return 1;
	}
	if (ea->seq_order < eb->seq_order)
	{
		return -1;
	}
	if (ea->seq_order > eb->seq_order)
	{
		return 1;
	}
	return 0;
}

// Reads a standard MIDI Variable-Length Quantity (7 bits per byte)
static unsigned long read_vlq(unsigned char **ptr, unsigned char *end)
{
	unsigned long val = 0;
	unsigned char c;

	while (*ptr < end)
	{
		c = *(*ptr)++;
		val = (val << 7) | (c & 0x7F);
		if (!(c & 0x80))
		{
			break;
		}
	}
	return val;
}

// Writes an integer as a standard MIDI Variable-Length Quantity
static void write_vlq(unsigned char **ptr, unsigned long val)
{
	unsigned long buffer = val & 0x7F;

	// Pack 7-bit chunks in reverse byte order with MSB continuation flags
	while ((val >>= 7))
	{
		buffer <<= 8;
		buffer |= ((val & 0x7F) | 0x80);
	}

	// Emit packed bytes until MSB is 0
	while (1)
	{
		*(*ptr)++ = (unsigned char)(buffer & 0xFF);
		if (buffer & 0x80)
		{
			buffer >>= 8;
		}
		else
		{
			break;
		}
	}
}

// Calculates exact encoded VLQ byte size without writing to a buffer
static unsigned int vlq_byte_len(unsigned long val)
{
	unsigned int len = 1;

	while ((val >>= 7))
	{
		len++;
	}
	return len;
}

// Writes an XMIDI delta delay (sum of 0x7F bytes plus remainder < 0x7F)
static void write_xmi_delay(unsigned char **ptr, unsigned long delay)
{
	if (delay == 0)
	{
		return;
	}
	while (delay >= 0x7F)
	{
		*(*ptr)++ = 0x7F;
		delay -= 0x7F;
	}
	if (delay > 0)
	{
		*(*ptr)++ = (unsigned char)delay;
	}
}

// Calculates exact XMIDI delta delay byte size without writing to a buffer
static unsigned int xmi_delay_byte_len(unsigned long delay)
{
	unsigned int len = 0;

	if (delay == 0)
	{
		return 0;
	}
	while (delay >= 0x7F)
	{
		len++;
		delay -= 0x7F;
	}
	if (delay > 0)
	{
		len++;
	}
	return len;
}

// Replaces or appends the extension of 'in_path' into 'out_buf'
static void build_out_path(const char *in_path, const char *new_ext, char *out_buf, size_t buf_sz)
{
	const char *dot = strrchr(in_path, '.');
	const char *slash1 = strrchr(in_path, '\\');
	const char *slash2 = strrchr(in_path, '/');
	const char *last_slash = (slash1 > slash2) ? slash1 : slash2;
	size_t base_len;

	if (dot && (!last_slash || dot > last_slash))
	{
		base_len = (size_t)(dot - in_path);
	}
	else
	{
		base_len = strlen(in_path);
	}
	if (base_len + strlen(new_ext) + 1 > buf_sz)
	{
		base_len = buf_sz - strlen(new_ext) - 1;
	}

	memcpy(out_buf, in_path, base_len);
	strcpy(out_buf + base_len, new_ext);
}

// Inserts a 1-based slice index ("_01", "_02", ...) before the file extension
static void build_slice_path(const char *base_out_path, unsigned int slice_idx, char *slice_buf, size_t buf_sz)
{
	const char *dot = strrchr(base_out_path, '.');
	const char *slash1 = strrchr(base_out_path, '\\');
	const char *slash2 = strrchr(base_out_path, '/');
	const char *last_slash = (slash1 > slash2) ? slash1 : slash2;
	char suffix[SUFFIX_BUF_SIZE];
	size_t base_len;
	const char *ext = "";

	sprintf(suffix, "_%02u", slice_idx);

	if (dot && (!last_slash || dot > last_slash))
	{
		base_len = (size_t)(dot - base_out_path);
		ext = dot;
	}
	else
	{
		base_len = strlen(base_out_path);
	}

	if (base_len + strlen(suffix) + strlen(ext) + 1 > buf_sz)
	{
		base_len = buf_sz - strlen(suffix) - strlen(ext) - 1;
	}

	memcpy(slice_buf, base_out_path, base_len);
	strcpy(slice_buf + base_len, suffix);
	strcat(slice_buf + base_len, ext);
}

//==============================================================================
// 2. Shared Synthesizer Channel State Tracker (Used by both MID & XMI slicers)
//==============================================================================

// Resets all 16 MIDI channels to default power-on synthesizer values
static void ch_state_init(struct ChannelState *st)
{
	memset(st->prog, 0xFF, sizeof(st->prog));
	memset(st->bank, 0, sizeof(st->bank));
	memset(st->vol, 0xFF, sizeof(st->vol));
	memset(st->pan, 0xFF, sizeof(st->pan));
	memset(st->pitch_lsb, 0x00, sizeof(st->pitch_lsb));
	memset(st->pitch_msb, 0x40, sizeof(st->pitch_msb));
	memset(st->has_pitch, 0, sizeof(st->has_pitch));
	memset(st->rpn_msb, 0x7F, sizeof(st->rpn_msb));
	memset(st->rpn_lsb, 0x7F, sizeof(st->rpn_lsb));
	memset(st->rpn_range, 2, sizeof(st->rpn_range));
	memset(st->has_rpn, 0, sizeof(st->has_rpn));
	memset(st->active_vel, 0, sizeof(st->active_vel));
	memset(st->note_end_tick, 0, sizeof(st->note_end_tick));
}

// Updates channel state when a Control Change (0xB0) message is encountered
static void ch_state_track_cc(struct ChannelState *st, unsigned char ch, unsigned char cc, unsigned char val)
{
	if (cc == 0x00)
	{
		st->bank[ch] = val;
	}
	else if (cc == 0x07)
	{
		st->vol[ch] = val;
	}
	else if (cc == 0x0A)
	{
		st->pan[ch] = val;
	}
	else if (cc == 101)
	{
		st->rpn_msb[ch] = val;
	}
	else if (cc == 100)
	{
		st->rpn_lsb[ch] = val;
	}
	else if (cc == 6 && st->rpn_msb[ch] == 0 && st->rpn_lsb[ch] == 0)
	{
		// RPN 0,0 Data Entry sets Pitch Bend sensitivity in semitones
		st->rpn_range[ch] = val;
		st->has_rpn[ch] = 1;
	}
}

// Updates channel state when a Pitch Bend (0xE0) message is encountered
static void ch_state_track_pitch(struct ChannelState *st, unsigned char ch, unsigned char lsb, unsigned char msb)
{
	st->pitch_lsb[ch] = lsb;
	st->pitch_msb[ch] = msb;
	st->has_pitch[ch] = 1;
}

// Emits a channel event into either MIDI (with leading 0x00 delta) or XMI stream
static void emit_state_msg(unsigned char **cur, int is_midi, unsigned char b0, unsigned char b1, int has_b2, unsigned char b2)
{
	if (is_midi)
	{
		*(*cur)++ = 0x00;
	}
	*(*cur)++ = b0;
	*(*cur)++ = b1;
	if (has_b2)
	{
		*(*cur)++ = b2;
	}
}

// Restores active instruments, volume, pan, RPN range, pitch bend, and split notes at slice start
static void ch_state_emit_restore(const struct ChannelState *st, unsigned char **cur, int is_midi,
	unsigned short *timbres, size_t *timb_cnt, unsigned long slice_start_tick)
{
	int c;

	if (timb_cnt)
	{
		*timb_cnt = 0;
	}

	for (c = 0; c < MAX_CHANNELS; c++)
	{
		// 1. Restore Bank Select & Program Change
		if (st->prog[c] != 0xFF)
		{
			if (st->bank[c] != 0)
			{
				emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 0x00, 1, st->bank[c]);
			}
			emit_state_msg(cur, is_midi, (unsigned char)(0xC0 | c), st->prog[c], 0, 0);

			// Register timbre into XMI TIMB table if building an XMI slice
			if (timbres && timb_cnt)
			{
				unsigned short tb = (unsigned short)(st->prog[c] | (st->bank[c] << 8));
				size_t k;
				int found = 0;
				for (k = 0; k < *timb_cnt; k++)
				{
					if (timbres[k] == tb)
					{
						found = 1;
						break;
					}
				}
				if (!found && *timb_cnt < MAX_TIMB_CNT)
				{
					timbres[(*timb_cnt)++] = tb;
				}
			}
		}

		// 2. Restore Volume & Pan
		if (st->vol[c] != 0xFF)
		{
			emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 0x07, 1, st->vol[c]);
		}
		if (st->pan[c] != 0xFF)
		{
			emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 0x0A, 1, st->pan[c]);
		}

		// 3. Restore Pitch Bend Sensitivity (RPN 0,0) BEFORE Pitch Bend wheel value
		if (st->has_rpn[c])
		{
			emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 101, 1, 0);
			emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 100, 1, 0);
			emit_state_msg(cur, is_midi, (unsigned char)(0xB0 | c), 6, 1, st->rpn_range[c]);
		}

		// 4. Restore Pitch Bend wheel position (only if used on this channel)
		if (st->has_pitch[c])
		{
			emit_state_msg(cur, is_midi, (unsigned char)(0xE0 | c), st->pitch_lsb[c], 1, st->pitch_msb[c]);
		}

		// 5. Re-trigger sustained notes (pads/bass) split across the XMI slice boundary
		if (!is_midi)
		{
			int n;
			for (n = 0; n < MAX_NOTES; n++)
			{
				if (st->active_vel[c][n] > 0 && st->note_end_tick[c][n] > slice_start_tick)
				{
					unsigned long rem_dur = st->note_end_tick[c][n] - slice_start_tick;
					*(*cur)++ = (unsigned char)(0x90 | c);
					*(*cur)++ = (unsigned char)n;
					*(*cur)++ = st->active_vel[c][n];
					write_vlq(cur, rem_dur);
				}
			}
		}
	}
}

//==============================================================================
// 3. File Writers (Standard MIDI & IFF XMIDI)
//==============================================================================
// Writes a complete Standard MIDI (.mid) file with exact track byte length
static void write_mid_file(const char *path, unsigned char *trk_data, unsigned int trk_len)
{
	FILE *pFo = NULL;
	unsigned short *mh_timebase = (unsigned short *)(midiheader + 12);
	unsigned int bs_tlen = _byteswap_ulong(trk_len);

	if (fopen_s(&pFo, path, "wb"))
	{
		fprintf(stderr, "Error: Cannot open output file %s\n", path);
		exit(errno);
	}

	*mh_timebase = _byteswap_ushort(timebase);
	fwrite(midiheader, 1, HDR_SIZE, pFo);
	fwrite(&bs_tlen, 4, 1, pFo);
	fwrite(trk_data, 1, trk_len, pFo);
	fclose(pFo);

	printf("Saved MIDI -> %s (%u bytes)\n", path, trk_len + 22);
}

// Writes a complete IFF XMIDI (.xmi) file with FORM:XDIR, CAT :XMID, TIMB, and EVNT chunks
static void write_xmi_file(const char *path, unsigned char *evnt_buf, unsigned int lEVNT,
	unsigned short *timbres, size_t timb_count, unsigned int quant_hz)
{
	FILE *pFo = NULL;
	unsigned int evnt_pad = (lEVNT % 2 != 0) ? 1 : 0; // IFF requires even chunk lengths
	unsigned int lTIMB, lFORM_inner, lCAT, lFORM_outer, bs_val;
	unsigned short seqCount = 1;
	unsigned short local_timbres[MAX_TIMB_CNT];
	size_t local_cnt = timb_count;
	size_t i;

	if (local_cnt == 0)
	{
		*local_timbres = 0x0000; // Default to patch 0, bank 0
		local_cnt = 1;
	}
	else
	{
		if (local_cnt > MAX_TIMB_CNT)
		{
			local_cnt = MAX_TIMB_CNT;
		}
		memcpy(local_timbres, timbres, local_cnt * sizeof(unsigned short));
	}

	lTIMB = (unsigned int)(local_cnt * 2);
	lFORM_inner = 4 + 8 + lTIMB + 8 + lEVNT + evnt_pad;
	lCAT = 4 + 8 + lFORM_inner;
	lFORM_outer = 4 + 8 + 2;

	if (fopen_s(&pFo, path, "wb"))
	{
		fprintf(stderr, "Error: Cannot open output file %s\n", path);
		exit(errno);
	}

	// Write FORM XDIR + INFO header
	fwrite("FORM", 1, 4, pFo);
	bs_val = _byteswap_ulong(lFORM_outer);
	fwrite(&bs_val, 4, 1, pFo);
	fwrite("XDIRINFO", 1, 8, pFo);
	bs_val = _byteswap_ulong(2);
	fwrite(&bs_val, 4, 1, pFo);
	fwrite(&seqCount, 2, 1, pFo);

	// Write CAT XMID + FORM XMID container
	fwrite("CAT ", 1, 4, pFo);
	bs_val = _byteswap_ulong(lCAT);
	fwrite(&bs_val, 4, 1, pFo);
	fwrite("XMIDFORM", 1, 8, pFo);
	bs_val = _byteswap_ulong(lFORM_inner);
	fwrite(&bs_val, 4, 1, pFo);
	fwrite("XMID", 1, 4, pFo);

	// Write TIMB (Timbre list) chunk
	fwrite("TIMB", 1, 4, pFo);
	bs_val = _byteswap_ulong(lTIMB);
	fwrite(&bs_val, 4, 1, pFo);
	for (i = 0; i < local_cnt; i++)
	{
		unsigned char p = (unsigned char)(local_timbres[i] & 0xFF);
		unsigned char b = (unsigned char)((local_timbres[i] >> 8) & 0xFF);
		fwrite(&p, 1, 1, pFo);
		fwrite(&b, 1, 1, pFo);
	}

	// Write EVNT (Event stream) chunk
	fwrite("EVNT", 1, 4, pFo);
	bs_val = _byteswap_ulong(lEVNT);
	fwrite(&bs_val, 4, 1, pFo);
	fwrite(evnt_buf, 1, lEVNT, pFo);
	if (evnt_pad)
	{
		unsigned char zero = 0;
		fwrite(&zero, 1, 1, pFo);
	}
	fclose(pFo);
	printf("TIMB: %u entries, EVNT: %u bytes (Quant: %u Hz)\n", (unsigned int)local_cnt, lEVNT, quant_hz);
	printf("Saved XMI -> %s (%u bytes)\n", path, 8 + lFORM_outer + 8 + lCAT);
}

//==============================================================================
// 4. XMI -> MIDI Pipeline Helpers
//==============================================================================
// Parses XMIDI IFF chunks and returns a pointer to the start of EVNT payload
static unsigned char *xmi_locate_evnt_chunk(unsigned char *midi_data, unsigned int *out_evnt_len)
{
	unsigned char *cur = midi_data;
	unsigned int lTIMB;
	unsigned short seqCount;

	if (memcmp(cur, "FORM", 4)) fprintf(stderr, "Warning: Missing FORM header\n");
	cur += 8;

	if (memcmp(cur, "XDIR", 4)) fprintf(stderr, "Warning: Missing XDIR chunk\n");
	cur += 4;

	if (memcmp(cur, "INFO", 4)) fprintf(stderr, "Warning: Missing INFO chunk\n");
	cur += 8;

	seqCount = *(unsigned short *)cur;
	cur += 2;
	printf("XMIDI Sequences: %d\n", seqCount);

	if (memcmp(cur, "CAT ", 4)) fprintf(stderr, "Warning: Missing CAT chunk\n");
	cur += 8;

	if (memcmp(cur, "XMID", 4)) fprintf(stderr, "Warning: Missing XMID form type\n");
	cur += 4;

	if (memcmp(cur, "FORM", 4)) fprintf(stderr, "Warning: Missing inner FORM chunk\n");
	cur += 8;

	if (memcmp(cur, "XMID", 4)) fprintf(stderr, "Warning: Missing inner XMID form type\n");
	cur += 4;

	// Skip TIMB chunk if present
	if (!memcmp(cur, "TIMB", 4))
	{
		cur += 4;
		lTIMB = _byteswap_ulong(*(unsigned int *)cur);
		cur += 4 + lTIMB;
	}

	// Skip RBRN (branch table) chunk if present
	if (!memcmp(cur, "RBRN", 4))
	{
		unsigned short nBranch;
		cur += 8;
		nBranch = *(unsigned short *)cur;
		cur += 2 + (nBranch * 6);
	}

	if (memcmp(cur, "EVNT", 4))
	{
		fprintf(stderr, "Error: Not a valid XMIDI file (EVNT chunk missing)\n");
		exit(-1);
	}
	cur += 4;

	*out_evnt_len = _byteswap_ulong(*(unsigned int *)cur);
	cur += 4;
	printf("EVNT length: %u bytes\n", *out_evnt_len);

	return cur;
}

// Decodes raw XMIDI EVNT stream into an intermediate 60-PPQN MIDI stream
static unsigned int xmi_decode_to_60ppqn(unsigned char *evnt_start, unsigned int lEVNT, unsigned char *midi_decode)
{
	unsigned char *cur = evnt_start;
	unsigned char *dcur = midi_decode;
	int next_is_delta = 1;
	unsigned int oevents = 0;

	off_events->delta = 0xFFFFFFFFUL;
	off_events->off_status = 0;
	off_events->off_note = 0;
	off_events->off_vel = 0;

	while ((unsigned int)(cur - evnt_start) < lEVNT)
	{
		if (*cur < 0x80)
		{
			unsigned int delay = 0;
			unsigned int tdelay, i;

			while (*cur == 0x7F)
			{
				delay += *cur++;
			}
			delay += *cur++;

			while (oevents > 0 && delay > off_events->delta)
			{
				unsigned int no_delta = off_events->delta;
				unsigned int ntdelay = no_delta & 0x7F;

				while ((no_delta >>= 7))
				{
					ntdelay <<= 8;
					ntdelay |= (no_delta & 0x7F) | 0x80;
				}
				while (1)
				{
					*dcur++ = (unsigned char)(ntdelay & 0xFF);
					if (ntdelay & 0x80)
					{
						ntdelay >>= 8;
					}
					else
					{
						break;
					}
				}
				*dcur++ = off_events->off_status & 0x8F;
				*dcur++ = off_events->off_note;
				*dcur++ = 0x7F;

				delay -= off_events->delta;
				for (i = 1; i < oevents; i++)
				{
					off_events[i].delta -= off_events->delta;
				}
				off_events->delta = 0xFFFFFFFFUL;
				qsort(off_events, oevents, sizeof(struct NOEVENTS), comp_events);
				oevents--;
			}

			for (i = 0; i < oevents; i++)
			{
				off_events[i].delta -= delay;
			}

			tdelay = delay & 0x7F;
			while ((delay >>= 7))
			{
				tdelay <<= 8;
				tdelay |= (delay & 0x7F) | 0x80;
			}
			while (1)
			{
				*dcur++ = (unsigned char)(tdelay & 0xFF);
				if (tdelay & 0x80)
				{
					tdelay >>= 8;
				}
				else
				{
					break;
				}
			}
			next_is_delta = 0;
		}
		else
		{
			if (next_is_delta && *cur >= 0x80)
			{
				*dcur++ = 0;
			}
			next_is_delta = 1;

			if (*cur == 0xFF)
			{
				if (*(cur + 1) == 0x2F)
				{
					unsigned int i;
					for (i = 0; i < oevents; i++)
					{
						*dcur++ = off_events[i].off_status & 0x8F;
						*dcur++ = off_events[i].off_note;
						*dcur++ = 0x7F;
						*dcur++ = 0;
					}
					*dcur++ = *cur++;
					*dcur++ = *cur++;
					*dcur++ = 0;
					break;
				}
				*dcur++ = *cur++;
				*dcur++ = *cur++;
				{
					unsigned long mlen = read_vlq(&cur, evnt_start + lEVNT);
					write_vlq(&dcur, mlen);
					while (mlen--)
					{
						*dcur++ = *cur++;
					}
				}
			}
			else if (0x80 == (*cur & 0xF0) || 0xA0 == (*cur & 0xF0) ||
				0xB0 == (*cur & 0xF0) || 0xE0 == (*cur & 0xF0))
			{
				*dcur++ = *cur++;
				*dcur++ = *cur++;
				*dcur++ = *cur++;
			}
			else if (0x90 == (*cur & 0xF0))
			{
				unsigned char st_byte = *cur++;
				unsigned char note_byte = *cur++;
				unsigned char vel_byte = *cur++;
				unsigned int delta = 0;
				unsigned int k;

				while (*cur & 0x80)
				{
					delta = (delta << 7) | (*cur++ & 0x7F);
				}
				delta = (delta << 7) | (*cur++ & 0x7F);

				for (k = 0; k < oevents; k++)
				{
					if (off_events[k].off_status == st_byte && off_events[k].off_note == note_byte)
					{
						*dcur++ = (unsigned char)(st_byte & 0x8F);
						*dcur++ = note_byte;
						*dcur++ = 0x00;
						*dcur++ = 0x00;

						off_events[k].delta = 0xFFFFFFFFUL;
						qsort(off_events, oevents, sizeof(struct NOEVENTS), comp_events);
						oevents--;
						break;
					}
				}

				*dcur++ = st_byte;
				*dcur++ = note_byte;
				*dcur++ = vel_byte;

				if (oevents < MAX_OFF_EVENTS)
				{
					off_events[oevents].delta = delta;
					off_events[oevents].off_status = st_byte;
					off_events[oevents].off_note = note_byte;
					off_events[oevents].off_vel = 0x7F;
					oevents++;
					qsort(off_events, oevents, sizeof(struct NOEVENTS), comp_events);
				}
			}
			else if (0xC0 == (*cur & 0xF0) || 0xD0 == (*cur & 0xF0))
			{
				*dcur++ = *cur++;
				*dcur++ = *cur++;
			}
			else if (0xF0 == *cur || 0xF7 == *cur)
			{
				*dcur++ = *cur++;
				{
					unsigned long slen = read_vlq(&cur, evnt_start + lEVNT);
					write_vlq(&dcur, slen);
					while (slen--)
					{
						*dcur++ = *cur++;
					}
				}
			}
			else
			{
				cur++;
			}
		}
	}

	return (unsigned int)(dcur - midi_decode);
}

// Writes the decoded 60-PPQN MIDI stream into one or more rescaled 960-TPQN MIDI files
static void mid_rescale_and_write(unsigned char *midi_decode, unsigned int dlen,
	const char *target_path, unsigned int slice_max_bytes)
{
	unsigned char *dcur = midi_decode + dlen;
	unsigned char *midi_write = (unsigned char *)malloc((size_t)dlen * 2 + 65536);
	unsigned char *tcur = midi_write;
	unsigned char *pos = midi_decode;
	struct ChannelState ch_state;
	unsigned char active_notes[MAX_CHANNELS][MAX_NOTES];
	unsigned char tempo_b0 = 0x07, tempo_b1 = 0xA1, tempo_b2 = 0x20;
	unsigned int slice_idx = 1;
	int has_tempo = 0;

	if (!midi_write)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		exit(errno);
	}

	if (slice_max_bytes > 0 && slice_max_bytes < MIN_SLICE_BYTES)
	{
		fprintf(stderr, "Error: -slice size too small (minimum %d bytes)\n", MIN_SLICE_BYTES);
		free(midi_write);
		exit(-1);
	}

	ch_state_init(&ch_state);
	memset(active_notes, 0, sizeof(active_notes));

	while (pos < dcur)
	{
		unsigned char *delta_start_pos = pos;
		unsigned int delta = (unsigned int)read_vlq(&pos, dcur);
		double factor = (double)timebase * (double)DEFAULT_QN / ((double)qnlen * (double)DEFAULT_TIMEBASE);
		unsigned char *tcur_checkpoint = tcur;
		unsigned int saved_qnlen = qnlen;
		struct ChannelState saved_ch_state = ch_state;
		unsigned char saved_note_ch = 0xFF;
		unsigned char saved_note_num = 0;
		unsigned char saved_note_prev_vel = 0;
		int is_eot = 0;

		delta = (unsigned int)((double)delta * factor + 0.5);

		// If starting slice > 1, restore Tempo, ChannelState, and re-trigger split notes at tick 0
		if (slice_max_bytes > 0 && slice_idx > 1 && tcur == midi_write)
		{
			int c, n;
			if (has_tempo)
			{
				*tcur++ = 0x00;
				*tcur++ = 0xFF;
				*tcur++ = 0x51;
				*tcur++ = 0x03;
				*tcur++ = tempo_b0;
				*tcur++ = tempo_b1;
				*tcur++ = tempo_b2;
			}
			ch_state_emit_restore(&ch_state, &tcur, 1, NULL, NULL, 0);

			// Re-trigger sustained notes (pads/bass) split across the MIDI slice boundary
			for (c = 0; c < MAX_CHANNELS; c++)
			{
				for (n = 0; n < MAX_NOTES; n++)
				{
					if (active_notes[c][n] > 0)
					{
						*tcur++ = 0x00;
						*tcur++ = (unsigned char)(0x90 | c);
						*tcur++ = (unsigned char)n;
						*tcur++ = active_notes[c][n];
					}
				}
			}
			tcur_checkpoint = tcur;
		}

		write_vlq(&tcur, delta);

		// Copy event and update channel state trackers (saving previous note state for rollback)
		if (0x80 == (*pos & 0xF0))
		{
			unsigned char ch = *pos & 0x0F;
			unsigned char note = *(pos + 1);
			saved_note_ch = ch;
			saved_note_num = note;
			saved_note_prev_vel = active_notes[ch][note];
			active_notes[ch][note] = 0;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0x90 == (*pos & 0xF0))
		{
			unsigned char ch = *pos & 0x0F;
			unsigned char note = *(pos + 1);
			unsigned char vel = *(pos + 2);
			saved_note_ch = ch;
			saved_note_num = note;
			saved_note_prev_vel = active_notes[ch][note];
			active_notes[ch][note] = (vel > 0) ? vel : 0;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xB0 == (*pos & 0xF0))
		{
			ch_state_track_cc(&ch_state, *pos & 0x0F, *(pos + 1), *(pos + 2));
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xA0 == (*pos & 0xF0))
		{
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xC0 == (*pos & 0xF0))
		{
			ch_state.prog[*pos & 0x0F] = *(pos + 1);
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xD0 == (*pos & 0xF0))
		{
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xE0 == (*pos & 0xF0))
		{
			ch_state_track_pitch(&ch_state, *pos & 0x0F, *(pos + 1), *(pos + 2));
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xF0 == *pos || 0xF7 == *pos)
		{
			unsigned long exlen;
			*tcur++ = *pos++;
			exlen = read_vlq(&pos, dcur);
			write_vlq(&tcur, exlen);
			while (exlen--)
			{
				*tcur++ = *pos++;
			}
		}
		else if (0xFF == *pos)
		{
			*tcur++ = *pos++;
			if (0x2F == *pos)
			{
				*tcur++ = *pos++;
				*tcur++ = *pos++;
				is_eot = 1;
			}
			else if (0x51 == *pos)
			{
				*tcur++ = *pos++;
				*tcur++ = *pos++;
				tempo_b0 = *(pos + 0);
				tempo_b1 = *(pos + 1);
				tempo_b2 = *(pos + 2);
				has_tempo = 1;
				qnlen = ((unsigned int)tempo_b0 << 16) +
					((unsigned int)tempo_b1 << 8) +
					(unsigned int)tempo_b2;
				*tcur++ = *pos++;
				*tcur++ = *pos++;
				*tcur++ = *pos++;
			}
			else
			{
				unsigned long mlen;
				*tcur++ = *pos++;
				mlen = read_vlq(&pos, dcur);
				write_vlq(&tcur, mlen);
				while (mlen--)
				{
					*tcur++ = *pos++;
				}
			}
		}
		else
		{
			pos++;
		}

		// Check if current MIDI slice reached the byte ceiling
		if (slice_max_bytes > 0 && !is_eot)
		{
			unsigned int active_cnt = 0;
			int c, n;
			unsigned int projected_size;

			for (c = 0; c < MAX_CHANNELS; c++)
			{
				for (n = 0; n < MAX_NOTES; n++)
				{
					if (active_notes[c][n] > 0)
					{
						active_cnt++;
					}
				}
			}

			// Projected size = 22-byte header + track bytes + pending Note-Offs + 4-byte EOT
			projected_size = 22 + (unsigned int)(tcur - midi_write) + (active_cnt * 4) + 4;

			if (projected_size > slice_max_bytes && tcur_checkpoint > midi_write)
			{
				char slice_path[PATH_BUF_SIZE];
				unsigned int tlen;

				// Roll back pointers AND synthesizer/note states to before the overflowing event
				tcur = tcur_checkpoint;
				pos = delta_start_pos;
				qnlen = saved_qnlen;
				ch_state = saved_ch_state;
				if (saved_note_ch < MAX_CHANNELS)
				{
					active_notes[saved_note_ch][saved_note_num] = saved_note_prev_vel;
				}

				// Close all hanging notes at the end of this slice (keep velocity in active_notes for next slice)
				for (c = 0; c < MAX_CHANNELS; c++)
				{
					for (n = 0; n < MAX_NOTES; n++)
					{
						if (active_notes[c][n] > 0)
						{
							*tcur++ = 0x00;
							*tcur++ = (unsigned char)(0x80 | c);
							*tcur++ = (unsigned char)n;
							*tcur++ = 0x00;
						}
					}
				}

				*tcur++ = 0x00;
				*tcur++ = 0xFF;
				*tcur++ = 0x2F;
				*tcur++ = 0x00;

				tlen = (unsigned int)(tcur - midi_write);
				build_slice_path(target_path, slice_idx++, slice_path, sizeof(slice_path));
				write_mid_file(slice_path, midi_write, tlen);

				tcur = midi_write;
			}
		}
	}

	// Write the final (or single) MIDI file exact to its byte length
	{
		unsigned int tlen = (unsigned int)(tcur - midi_write);
		if (tlen > 0)
		{
			if (slice_max_bytes > 0)
			{
				char slice_path[PATH_BUF_SIZE];
				build_slice_path(target_path, slice_idx, slice_path, sizeof(slice_path));
				write_mid_file(slice_path, midi_write, tlen);
			}
			else
			{
				write_mid_file(target_path, midi_write, tlen);
			}
		}
	}

	free(midi_write);
}

// Top-level orchestrator for XMI -> MIDI conversion
void convert_xmi_to_mid(unsigned char *midi_data, long fsize, const char *in_filename,
	const char *out_filename, unsigned int slice_max_bytes)
{
	unsigned int lEVNT = 0;
	char pt[PATH_BUF_SIZE];
	const char *target_path = out_filename;
	unsigned char *evnt_start = xmi_locate_evnt_chunk(midi_data, &lEVNT);
	unsigned char *midi_decode = (unsigned char *)malloc((size_t)fsize * 4 + 4096);
	unsigned int dlen;

	if (!midi_decode)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		exit(errno);
	}
	if (!target_path)
	{
		build_out_path(in_filename, ".mid", pt, sizeof(pt));
		target_path = pt;
	}
	qnlen = DEFAULT_QN;
	dlen = xmi_decode_to_60ppqn(evnt_start, lEVNT, midi_decode);
	mid_rescale_and_write(midi_decode, dlen, target_path, slice_max_bytes);
	free(midi_decode);
}

//==============================================================================
// 5. MIDI -> XMI Pipeline Helpers
//==============================================================================
// Parses all MTrk chunks and returns a sorted flat array of RawMidiEvent structs
static struct RawMidiEvent *mid_parse_and_merge_tracks(unsigned char *cur, unsigned char *end,
	unsigned short ntrks, size_t *out_ev_count,
	unsigned short *timbres, size_t *out_timb_count)
{
	unsigned short t;
	unsigned char bank_msb[MAX_CHANNELS];
	size_t ev_cap = INITIAL_EVENT_CAP;
	size_t ev_count = 0;
	size_t timb_count = 0;
	struct RawMidiEvent *events = (struct RawMidiEvent *)malloc(ev_cap * sizeof(struct RawMidiEvent));

	if (!events)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		exit(errno);
	}

	memset(bank_msb, 0, sizeof(bank_msb));

	for (t = 0; t < ntrks && cur < end; t++)
	{
		unsigned int trk_len;
		unsigned char *trk_end;
		unsigned long abs_tick = 0;
		unsigned char running_status = 0;

		if (cur + 8 > end || memcmp(cur, "MTrk", 4))
		{
			break;
		}
		cur += 4;
		trk_len = _byteswap_ulong(*(unsigned int *)cur);
		cur += 4;
		trk_end = cur + trk_len;
		if (trk_end > end)
		{
			trk_end = end;
		}

		while (cur < trk_end)
		{
			unsigned long delta = read_vlq(&cur, trk_end);
			unsigned char status, cmd, ch;
			struct RawMidiEvent *ev;
			abs_tick += delta;
			if (cur >= trk_end)
			{
				break;
			}
			status = *cur;
			if (status & 0x80)
			{
				cur++;
				if (status < 0xF0)
				{
					running_status = status;
				}
			}
			else
			{
				status = running_status;
			}
			if (ev_count >= ev_cap)
			{
				ev_cap *= 2;
				events = (struct RawMidiEvent *)realloc(events, ev_cap * sizeof(struct RawMidiEvent));
				if (!events)
				{
					fprintf(stderr, "Error: Memory reallocation failed\n");
					exit(errno);
				}
			}
			ev = &events[ev_count];
			memset(ev, 0, sizeof(struct RawMidiEvent));
			ev->abs_tick = abs_tick;
			ev->status = status;
			ev->seq_order = (unsigned long)ev_count;
			cmd = status & 0xF0;
			ch = status & 0x0F;
			if (cmd == 0x80 || cmd == 0x90 || cmd == 0xA0 || cmd == 0xB0 || cmd == 0xE0)
			{
				ev->d1 = *cur++;
				ev->d2 = *cur++;
				if (cmd == 0x90 && ev->d2 == 0)
				{
					ev->status = (unsigned char)(0x80 | ch);
				}
				if (cmd == 0xB0 && ev->d1 == 0)
				{
					bank_msb[ch] = ev->d2;
				}
				ev_count++;
			}
			else if (cmd == 0xC0 || cmd == 0xD0)
			{
				ev->d1 = *cur++;
				if (cmd == 0xC0)
				{
					unsigned short tb = (unsigned short)(ev->d1 | (bank_msb[ch] << 8));
					size_t k;
					int found = 0;
					for (k = 0; k < timb_count; k++)
					{
						if (timbres[k] == tb)
						{
							found = 1;
							break;
						}
					}
					if (!found && timb_count < MAX_TIMB_CNT)
					{
						timbres[timb_count++] = tb;
					}
				}
				ev_count++;
			}
			else if (status == 0xFF)
			{
				unsigned char meta_type = *cur++;
				unsigned long meta_len = read_vlq(&cur, trk_end);
				ev->d1 = meta_type;
				ev->meta_len = meta_len;
				ev->meta_data = cur;
				cur += meta_len;
				if (meta_type != 0x2F)
				{
					ev_count++;
				}
			}
			else if (status == 0xF0 || status == 0xF7)
			{
				unsigned long sys_len = read_vlq(&cur, trk_end);
				ev->meta_len = sys_len;
				ev->meta_data = cur;
				cur += sys_len;
				ev_count++;
			}
			else
			{
				cur++;
			}
		}

		cur = trk_end;
	}

	qsort(events, ev_count, sizeof(struct RawMidiEvent), comp_raw_events);

	*out_ev_count = ev_count;
	*out_timb_count = timb_count;

	return events;
}

// Quantizes event timestamps to 120 Hz using John Miles' MIDIFORM DDA accumulator
static void mid_quantize_to_xmi_ticks(struct RawMidiEvent *events, size_t ev_count,
	int is_smpte, double smpte_ticks_per_sec,
	unsigned short division, unsigned int quant_hz)
{
	const long double q_int = 100000000.0L / (long double)quant_hz;
	long double tick_time = is_smpte
		? (100000000.0L / (long double)smpte_ticks_per_sec)
		: (50000000.0L / (long double)division);
	long double dda_sum = 0.0L;
	unsigned long xmi_interval = 0;
	unsigned long last_midi_tick = 0;
	size_t i;

	for (i = 0; i < ev_count; i++)
	{
		unsigned long event_delta = events[i].abs_tick - last_midi_tick;
		last_midi_tick = events[i].abs_tick;
		if (event_delta > 0)
		{
			dda_sum += (long double)event_delta * tick_time;
			while (dda_sum >= q_int)
			{
				dda_sum -= q_int;
				xmi_interval++;
			}
		}
		events[i].abs_tick = xmi_interval;

        // Update tick duration on Tempo Meta-Events (0xFF 0x51) in PPQN mode
        if (!is_smpte && events[i].status == 0xFF && events[i].d1 == 0x51 && events[i].meta_len == 3)
        {
            unsigned char *md = events[i].meta_data;
            unsigned long tempo_us = ((unsigned long)(*(md + 0)) << 16) |
                                     ((unsigned long)(*(md + 1)) << 8) |
                                     (unsigned long)(*(md + 2));
            tick_time = (100.0L * (long double)tempo_us) / (long double)division;
        }
		// Must be like this otherwise gets ULTRA SLOW:
		//if (!is_smpte && events[i].status == 0xFF && events[i].d1 == 0x51 && events[i].meta_len == 3)
		//{
		//	unsigned char *md = events[i].meta_data;
		//	unsigned long tempo_us = ((unsigned long)(*(md + 0)) << 16) |
		//		((unsigned long)(*(md + 1)) << 8) |
		//		(unsigned long)(*(md + 2));
		//	tick_time = (100.0L * (long double)tempo_us) / (long double)division;
		//}

	}
}

// Pairs every Note-On (0x90) with its corresponding Note-Off (0x80) to compute duration
static void mid_pair_note_durations(struct RawMidiEvent *events, size_t ev_count)
{
	size_t i, j;
	for (i = 0; i < ev_count; i++)
	{
		if ((events[i].status & 0xF0) == 0x90)
		{
			unsigned char ch = events[i].status & 0x0F;
			unsigned char note = events[i].d1;
			unsigned long dur = 1;
			for (j = i + 1; j < ev_count; j++)
			{
				if (!events[j].skip && (events[j].status & 0xF0) == 0x80 &&
					(events[j].status & 0x0F) == ch && events[j].d1 == note)
				{
					dur = events[j].abs_tick - events[i].abs_tick;
					if (dur == 0)
					{
						dur = 1;
					}
					events[j].skip = 1;
					break;
				}
			}
			events[i].duration_ticks = dur;
		}
	}
}

// Builds EVNT stream (with -slim filtering and -slice splitting) and writes XMI file(s)
static void mid_build_and_save_xmi(struct RawMidiEvent *events, size_t ev_count, long fsize,
	const char *target_path, unsigned short *full_timbres,
	size_t full_timb_count, int slim_mode, unsigned int quant_hz,
	unsigned int slice_max_bytes)
{
	unsigned char *evnt_buf = (unsigned char *)malloc((size_t)fsize * 4 + 65536);
	unsigned char *ecur = evnt_buf;
	unsigned long prev_xmi_tick = 0;
	short last_cc_val[MAX_CHANNELS][MAX_NOTES];
	int last_pitch_val[MAX_CHANNELS];
	struct ChannelState ch_state;
	unsigned short slice_timbres[MAX_TIMB_CNT];
	size_t slice_timb_cnt = 0;
	unsigned int slice_idx = 1;
	unsigned int lEVNT;
	size_t i;

	if (!evnt_buf)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		exit(errno);
	}

	if (slice_max_bytes > 0 && slice_max_bytes < MIN_SLICE_BYTES)
	{
		fprintf(stderr, "Error: -slice size too small (minimum %d bytes)\n", MIN_SLICE_BYTES);
		free(evnt_buf);
		exit(-1);
	}

	ch_state_init(&ch_state);

	if (slim_mode)
	{
		int c;
		memset(last_cc_val, -1, sizeof(last_cc_val));
		for (c = 0; c < MAX_CHANNELS; c++)
		{
			last_pitch_val[c] = -1;
		}
	}

	for (i = 0; i < ev_count; i++)
	{
		unsigned char st, cmd, ch;
		unsigned long delay;
		if (events[i].skip)
		{
			continue;
		}
		st = events[i].status;
		cmd = st & 0xF0;
		ch = st & 0x0F;
		if (cmd == 0x80)
		{
			continue;
		}

		// Apply -slim filtering
		if (slim_mode)
		{
			if (cmd == 0xA0 || cmd == 0xD0)
			{
				continue;
			}
			if (st == 0xFF && events[i].d1 >= 0x01 && events[i].d1 <= 0x07)
			{
				continue;
			}
			if (cmd == 0xB0)
			{
				if (i + 1 < ev_count && !events[i + 1].skip &&
					events[i + 1].abs_tick == events[i].abs_tick &&
					events[i + 1].status == st &&
					events[i + 1].d1 == events[i].d1)
				{
					continue;
				}
				if (last_cc_val[ch][events[i].d1] == events[i].d2)
				{
					continue;
				}
				last_cc_val[ch][events[i].d1] = events[i].d2;
			}
			if (cmd == 0xE0)
			{
				int pval = ((int)events[i].d2 << 7) | (int)events[i].d1;
				if (last_pitch_val[ch] == pval)
				{
					continue;
				}
				last_pitch_val[ch] = pval;
			}
		}

		// Restore active channel state at the start of slice > 1
		if (slice_max_bytes > 0 && slice_idx > 1 && ecur == evnt_buf)
		{
			// Pass current even tick for XMI
			ch_state_emit_restore(&ch_state, &ecur, 0, slice_timbres, &slice_timb_cnt, events[i].abs_tick);
			prev_xmi_tick = events[i].abs_tick;
		}

		delay = events[i].abs_tick - prev_xmi_tick;

		// Check projected total XMI file size before writing this event
		if (slice_max_bytes > 0)
		{
			unsigned int cur_evnt_len = (unsigned int)(ecur - evnt_buf);
			unsigned int next_timb_cnt = (unsigned int)slice_timb_cnt;
			unsigned int ev_bytes = xmi_delay_byte_len(delay);
			unsigned int projected_evnt, projected_pad, projected_total;
			if (cmd == 0x90)
			{
				ev_bytes += 3 + vlq_byte_len(events[i].duration_ticks);
			}
			else if (cmd == 0xA0 || cmd == 0xB0 || cmd == 0xE0)
			{
				ev_bytes += 3;
			}
			else if (cmd == 0xC0 || cmd == 0xD0)
			{
				ev_bytes += 2;
				if (cmd == 0xC0)
				{
					unsigned short tb = (unsigned short)(events[i].d1 | (ch_state.bank[ch] << 8));
					size_t k;
					int found = 0;
					for (k = 0; k < slice_timb_cnt; k++)
					{
						if (slice_timbres[k] == tb)
						{
							found = 1;
							break;
						}
					}
					if (!found && next_timb_cnt < MAX_TIMB_CNT)
					{
						next_timb_cnt++;
					}
				}
			}
			else if (st == 0xFF)
			{
				ev_bytes += 2 + vlq_byte_len(events[i].meta_len) + (unsigned int)events[i].meta_len;
			}
			else if (st == 0xF0 || st == 0xF7)
			{
				ev_bytes += 1 + vlq_byte_len(events[i].meta_len) + (unsigned int)events[i].meta_len;
			}
			if (next_timb_cnt == 0)
			{
				next_timb_cnt = 1;
			}

			projected_evnt = cur_evnt_len + ev_bytes + 3;
			projected_pad = (projected_evnt % 2 != 0) ? 1 : 0;
			projected_total = 58 + (next_timb_cnt * 2) + projected_evnt + projected_pad;

			if (projected_total > slice_max_bytes && cur_evnt_len > 0)
			{
				char slice_path[PATH_BUF_SIZE];
				*ecur++ = 0xFF;
				*ecur++ = 0x2F;
				*ecur++ = 0x00;
				lEVNT = (unsigned int)(ecur - evnt_buf);
				build_slice_path(target_path, slice_idx++, slice_path, sizeof(slice_path));
				write_xmi_file(slice_path, evnt_buf, lEVNT, slice_timbres, slice_timb_cnt, quant_hz);
				ecur = evnt_buf;
				slice_timb_cnt = 0;
				if (slim_mode)
				{
					int c;
					memset(last_cc_val, -1, sizeof(last_cc_val));
					for (c = 0; c < MAX_CHANNELS; c++)
					{
						last_pitch_val[c] = -1;
					}
				}
				i--;
				continue;
			}
		}

		// Update shared ChannelState tracker
		if (cmd == 0x90)
		{
			// Write note end time
			ch_state.active_vel[ch][events[i].d1] = events[i].d2;
			ch_state.note_end_tick[ch][events[i].d1] = events[i].abs_tick + events[i].duration_ticks;
		}
		else if (cmd == 0xB0)
		{
			ch_state_track_cc(&ch_state, ch, events[i].d1, events[i].d2);
		}
		else if (cmd == 0xC0)
		{
			unsigned short tb = (unsigned short)(events[i].d1 | (ch_state.bank[ch] << 8));
			size_t k;
			int found = 0;
			ch_state.prog[ch] = events[i].d1;
			for (k = 0; k < slice_timb_cnt; k++)
			{
				if (slice_timbres[k] == tb)
				{
					found = 1;
					break;
				}
			}
			if (!found && slice_timb_cnt < MAX_TIMB_CNT)
			{
				slice_timbres[slice_timb_cnt++] = tb;
			}
		}
		else if (cmd == 0xE0)
		{
			ch_state_track_pitch(&ch_state, ch, events[i].d1, events[i].d2);
		}

		// Write XMIDI delay and event bytes
		write_xmi_delay(&ecur, delay);
		prev_xmi_tick = events[i].abs_tick;

		if (cmd == 0x90)
		{
			*ecur++ = st;
			*ecur++ = events[i].d1;
			*ecur++ = events[i].d2;
			write_vlq(&ecur, events[i].duration_ticks);
		}
		else if (cmd == 0xA0 || cmd == 0xB0 || cmd == 0xE0)
		{
			*ecur++ = st;
			*ecur++ = events[i].d1;
			*ecur++ = events[i].d2;
		}
		else if (cmd == 0xC0 || cmd == 0xD0)
		{
			*ecur++ = st;
			*ecur++ = events[i].d1;
		}
		else if (st == 0xFF)
		{
			*ecur++ = 0xFF;
			*ecur++ = events[i].d1;
			write_vlq(&ecur, events[i].meta_len);
			if (events[i].meta_len > 0)
			{
				memcpy(ecur, events[i].meta_data, events[i].meta_len);
				ecur += events[i].meta_len;
			}
		}
		else if (st == 0xF0 || st == 0xF7)
		{
			*ecur++ = st;
			write_vlq(&ecur, events[i].meta_len);
			if (events[i].meta_len > 0)
			{
				memcpy(ecur, events[i].meta_data, events[i].meta_len);
				ecur += events[i].meta_len;
			}
		}
	}

	// Write final End-Of-Track meta event
	*ecur++ = 0xFF;
	*ecur++ = 0x2F;
	*ecur++ = 0x00;
	lEVNT = (unsigned int)(ecur - evnt_buf);
	if (slice_max_bytes > 0)
	{
		char slice_path[PATH_BUF_SIZE];
		build_slice_path(target_path, slice_idx, slice_path, sizeof(slice_path));
		write_xmi_file(slice_path, evnt_buf, lEVNT, slice_timbres, slice_timb_cnt, quant_hz);
	}
	else
	{
		write_xmi_file(target_path, evnt_buf, lEVNT, full_timbres, full_timb_count, quant_hz);
	}

	free(evnt_buf);
}

// Top-level orchestrator for MIDI -> XMI conversion
void convert_mid_to_xmi(unsigned char *midi_data, long fsize, const char *in_filename,
	const char *out_filename, int slim_mode, unsigned int quant_hz,
	unsigned int slice_max_bytes)
{
	unsigned char *cur = midi_data;
	unsigned char *end = midi_data + fsize;
	unsigned int hdr_len;
	unsigned short format, ntrks, division;
	int is_smpte = 0;
	double smpte_ticks_per_sec = 0.0;
	size_t ev_count = 0;
	size_t timb_count = 0;
	unsigned short timbres[MAX_TIMB_CNT];
	struct RawMidiEvent *events = NULL;
	char pt[PATH_BUF_SIZE];
	const char *target_path = out_filename;
	if (fsize < 14 || memcmp(cur, "MThd", 4))
	{
		fprintf(stderr, "Error: Invalid MIDI header (MThd missing)\n");
		exit(-1);
	}

	// Parse MThd header (pointer dereferences ensure big-endian 16-bit reads)
	// "Pointer stars" must be placed like this otherwise TEMPO gets SUPER FAST!
	//cur += 4;
	//hdr_len = _byteswap_ulong(*(unsigned int *)cur);
	//cur += 4;
	//format = _byteswap_ushort(*(unsigned short *)cur);
	//cur += 2;
	//ntrks = _byteswap_ushort(*(unsigned short *)cur);
	//cur += 2;
	//division = _byteswap_ushort(*(unsigned short *)cur);
	//cur += 2;

	cur += 4;
	hdr_len = _byteswap_ulong(*(unsigned int *)cur);
	cur += 4;
	format = _byteswap_ushort(*(unsigned short *)cur);
	cur += 2;
	ntrks = _byteswap_ushort(*(unsigned short *)cur);
	cur += 2;
	division = _byteswap_ushort(*(unsigned short *)cur);
	cur += 2;

	if (hdr_len > 6)
	{
		cur += (hdr_len - 6);
	}

	// Parse TPQN or SMPTE time division
	if (division & 0x8000)
	{
		unsigned char hi = (unsigned char)((division >> 8) & 0xFF);
		int fps_val = (hi >= 0xE0) ? -(signed char)hi : (hi & 0x7F);
		unsigned char tpf = (unsigned char)(division & 0xFF);
		double fps;
		is_smpte = 1;
		if (fps_val <= 0 || fps_val > 60)
		{
			fps_val = 25;
		}
		fps = (fps_val == 29) ? 29.97 : (double)fps_val;
		if (tpf == 0)
		{
			tpf = 40;
		}
		smpte_ticks_per_sec = fps * (double)tpf;
		printf("MIDI Format: %d, Tracks: %d, SMPTE: %.2f FPS, %d ticks/frame (%.1f Hz)\n",
			format, ntrks, fps, tpf, smpte_ticks_per_sec);
	}
	else
	{
		printf("MIDI Format: %d, Tracks: %d, Division (TPQN): %d\n", format, ntrks, division);
	}
	if (slim_mode)
	{
		printf("Slim mode ON: stripping Aftertouch & duplicate CCs (keeping Pitch Bend)\n");
	}
	if (slice_max_bytes > 0)
	{
		printf("Slice mode ON: max %u bytes per file\n", slice_max_bytes);
	}
	if (!target_path)
	{
		build_out_path(in_filename, ".xmi", pt, sizeof(pt));
		target_path = pt;
	}

	// Execute modular 4-stage MIDI -> XMI conversion pipeline
	events = mid_parse_and_merge_tracks(cur, end, ntrks, &ev_count, timbres, &timb_count);
	mid_quantize_to_xmi_ticks(events, ev_count, is_smpte, smpte_ticks_per_sec, division, quant_hz);
	mid_pair_note_durations(events, ev_count);
	mid_build_and_save_xmi(events, ev_count, fsize, target_path, timbres, timb_count,
		slim_mode, quant_hz, slice_max_bytes);
	free(events);
}

//==============================================================================
// 7. File Merging Engine (-merge & -mergename)
//==============================================================================

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

// Comparator for sorting filenames alphabetically/numerically (_01, _02, ...)
static int comp_filenames(const void *pa, const void *pb)
{
	const char *sa = *(const char * const *)pa;
	const char *sb = *(const char * const *)pb;
	return _stricmp(sa, sb);
}

// Extracts root name before "_XX" suffix (e.g. "e1l4_01.xmi" -> "e1l4")
// Returns 1 if file matches "<root>_<digits>.<ext>" pattern, 0 otherwise
static int extract_slice_root(const char *filename, char *root_out, size_t root_sz, char *ext_out, size_t ext_sz)
{
	const char *dot = strrchr(filename, '.');
	const char *underscore = NULL;
	size_t name_len, root_len, k;
	const char *p;

	if (!dot)
	{
		return 0;
	}
	if (_stricmp(dot, ".mid") != 0 && _stricmp(dot, ".xmi") != 0)
	{
		return 0;
	}

	strncpy(ext_out, dot, ext_sz - 1);
	ext_out[ext_sz - 1] = '\0';

	name_len = (size_t)(dot - filename);
	for (k = 0; k < name_len; k++)
	{
		if (filename[k] == '_')
		{
			underscore = filename + k;
		}
	}

	if (!underscore || underscore == filename)
	{
		return 0;
	}

	// Verify all characters between '_' and '.' are ASCII digits '0'..'9'
	p = underscore + 1;
	if (p >= dot)
	{
		return 0;
	}
	while (p < dot)
	{
		if (*p < '0' || *p > '9')
		{
			return 0;
		}
		p++;
	}

	root_len = (size_t)(underscore - filename);
	if (root_len >= root_sz)
	{
		root_len = root_sz - 1;
	}
	memcpy(root_out, filename, root_len);
	root_out[root_len] = '\0';

	return 1;
}

// Checks if filename matches explicit root prefix (e.g. "e1l4*") and isn't already "_merged"
static int matches_explicit_root(const char *filename, const char *root_prefix, char *ext_out, size_t ext_sz)
{
	const char *dot = strrchr(filename, '.');
	size_t prefix_len = strlen(root_prefix);

	if (!dot)
	{
		return 0;
	}
	if (_stricmp(dot, ".mid") != 0 && _stricmp(dot, ".xmi") != 0)
	{
		return 0;
	}
	if (_strnicmp(filename, root_prefix, prefix_len) != 0)
	{
		return 0;
	}
	// Skip exact base file (e.g. "e1l4.mid") and already merged output ("e1l4_merged.mid")
	if ((size_t)(dot - filename) == prefix_len)
	{
		return 0;
	}
	if (strstr(filename, "_merged.") != NULL)
	{
		return 0;
	}

	strncpy(ext_out, dot, ext_sz - 1);
	ext_out[ext_sz - 1] = '\0';
	return 1;
}

// Loads a single file into memory buffer (caller must free)
static unsigned char *load_file_to_buffer(const char *path, long *out_size)
{
	FILE *fp = NULL;
	long sz;
	unsigned char *buf;

	if (fopen_s(&fp, path, "rb"))
	{
		fprintf(stderr, "Error: Cannot open file %s\n", path);
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (sz <= 0)
	{
		fprintf(stderr, "Error: Empty or invalid file %s\n", path);
		fclose(fp);
		return NULL;
	}

	buf = (unsigned char *)malloc((size_t)sz);
	if (!buf)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		fclose(fp);
		return NULL;
	}

	if (sz != (long)fread_s(buf, (size_t)sz, 1, (size_t)sz, fp))
	{
		fprintf(stderr, "Error: Failed reading file %s\n", path);
		fclose(fp);
		free(buf);
		return NULL;
	}

	fclose(fp);
	*out_size = sz;
	return buf;
}

// Helper: Converts any input file (MIDI or XMI) into normalized 120Hz RawMidiEvent array
static struct RawMidiEvent *load_as_xmi_events(const char *path, long *out_fsize,
	size_t *out_ev_count, unsigned long *out_total_ticks,
	unsigned short *timbres, size_t *timb_count,
	unsigned int quant_hz)
{
	long fsize = 0;
	unsigned char *data = load_file_to_buffer(path, &fsize);
	struct RawMidiEvent *events = NULL;
	size_t ev_count = 0;
	unsigned long max_tick = 0;
	size_t i;

	if (!data)
	{
		return NULL;
	}

	*out_fsize = fsize;

	if (fsize >= 4 && memcmp(data, "MThd", 4) == 0)
	{
		// Standard MIDI input: parse header, merge tracks, quantize to 120Hz, pair notes
		unsigned char *cur = data + 4;
		unsigned int hdr_len = _byteswap_ulong(*(unsigned int *)cur);
		unsigned short ntrks, division;
		int is_smpte = 0;
		double smpte_hz = 0.0;

		cur += 6;
		ntrks = _byteswap_ushort(*(unsigned short *)cur);
		cur += 2;
		division = _byteswap_ushort(*(unsigned short *)cur);
		cur += 2;
		if (hdr_len > 6)
		{
			cur += (hdr_len - 6);
		}

		if (division & 0x8000)
		{
			unsigned char hi = (unsigned char)((division >> 8) & 0xFF);
			int fps_val = (hi >= 0xE0) ? -(signed char)hi : (hi & 0x7F);
			unsigned char tpf = (unsigned char)(division & 0xFF);
			double fps = (fps_val == 29) ? 29.97 : (double)((fps_val <= 0 || fps_val > 60) ? 25 : fps_val);
			if (tpf == 0) tpf = 40;
			is_smpte = 1;
			smpte_hz = fps * (double)tpf;
		}

		events = mid_parse_and_merge_tracks(cur, data + fsize, ntrks, &ev_count, timbres, timb_count);
		mid_quantize_to_xmi_ticks(events, ev_count, is_smpte, smpte_hz, division, quant_hz);
		mid_pair_note_durations(events, ev_count);
	}
	else if (fsize >= 4 && memcmp(data, "FORM", 4) == 0)
	{
		// XMIDI input: decode to 60PPQN MIDI in memory, wrap in MTrk header, and parse events
		unsigned int lEVNT = 0;
		unsigned char *evnt_start = xmi_locate_evnt_chunk(data, &lEVNT);
		unsigned char *midi_decode = (unsigned char *)malloc((size_t)fsize * 4 + 4104);
		unsigned int dlen;
		unsigned int bs_dlen;

		if (!midi_decode)
		{
			free(data);
			return NULL;
		}

		// Decode XMI events starting at offset +8 to leave room for the 8-byte "MTrk" header
		dlen = xmi_decode_to_60ppqn(evnt_start, lEVNT, midi_decode + 8);

		memcpy(midi_decode, "MTrk", 4);
		bs_dlen = _byteswap_ulong(dlen);
		memcpy(midi_decode + 4, &bs_dlen, 4);

		events = mid_parse_and_merge_tracks(midi_decode, midi_decode + 8 + dlen, 1, &ev_count, timbres, timb_count);
		mid_quantize_to_xmi_ticks(events, ev_count, 0, 0.0, DEFAULT_TIMEBASE, quant_hz);
		mid_pair_note_durations(events, ev_count);

		free(midi_decode);
	}
	else
	{
		fprintf(stderr, "Error: Unsupported file format in %s\n", path);
		free(data);
		return NULL;
	}

	// Calculate total span of this file (including trailing note durations)
	for (i = 0; i < ev_count; i++)
	{
		unsigned long end_tick = events[i].abs_tick;
		if (!events[i].skip && (events[i].status & 0xF0) == 0x90)
		{
			end_tick += events[i].duration_ticks;
		}
		if (end_tick > max_tick)
		{
			max_tick = end_tick;
		}
	}

	// Note: Keep 'data' alive if meta_data pointers point into it, or duplicate meta_data
	for (i = 0; i < ev_count; i++)
	{
		if (events[i].meta_len > 0 && events[i].meta_data != NULL)
		{
			unsigned char *copy = (unsigned char *)malloc(events[i].meta_len);
			if (copy)
			{
				memcpy(copy, events[i].meta_data, events[i].meta_len);
				events[i].meta_data = copy;
			}
		}
	}

	free(data);
	*out_ev_count = ev_count;
	*out_total_ticks = max_tick;
	return events;
}

// Merges an explicit ordered list of MIDI/XMI files into a single output file
void merge_files_list(const char **file_list, int file_count, const char *out_filename,
	int slim_mode, unsigned int quant_hz)
{
	struct RawMidiEvent *merged = NULL;
	size_t merged_count = 0;
	size_t merged_cap = INITIAL_EVENT_CAP;
	unsigned long current_tick_offset = 0;
	long total_fsize = 0;
	unsigned short timbres[MAX_TIMB_CNT];
	size_t timb_count = 0;
	char default_out[PATH_BUF_SIZE];
	const char *target_path = out_filename;
	int f_idx;

	if (file_count < 2)
	{
		fprintf(stderr, "Error: Need at least 2 files to merge\n");
		return;
	}

	merged = (struct RawMidiEvent *)malloc(merged_cap * sizeof(struct RawMidiEvent));
	if (!merged)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		exit(errno);
	}

	printf("Merging %d files...\n", file_count);

	for (f_idx = 0; f_idx < file_count; f_idx++)
	{
		const char *path = *(file_list + f_idx);
		long fsize = 0;
		size_t ev_cnt = 0;
		unsigned long file_ticks = 0;
		struct RawMidiEvent *part_events;
		size_t i;

		part_events = load_as_xmi_events(path, &fsize, &ev_cnt, &file_ticks, timbres, &timb_count, quant_hz);
		if (!part_events)
		{
			fprintf(stderr, "Skipping unreadable file: %s\n", path);
			continue;
		}

		printf("  [%d/%d] %s (events: %u, length: %lu ticks)\n",
			f_idx + 1, file_count, path, (unsigned int)ev_cnt, file_ticks);

		total_fsize += fsize;

		while (merged_count + ev_cnt >= merged_cap)
		{
			merged_cap *= 2;
			merged = (struct RawMidiEvent *)realloc(merged, merged_cap * sizeof(struct RawMidiEvent));
			if (!merged)
			{
				fprintf(stderr, "Error: Memory reallocation failed\n");
				exit(errno);
			}
		}

		for (i = 0; i < ev_cnt; i++)
		{
			merged[merged_count] = part_events[i];
			merged[merged_count].abs_tick += current_tick_offset;
			merged[merged_count].seq_order = (unsigned long)merged_count;
			merged_count++;
		}

		current_tick_offset += file_ticks;
		free(part_events);
	}

	if (merged_count == 0)
	{
		fprintf(stderr, "Error: No valid events collected during merge\n");
		free(merged);
		return;
	}

	// Determine output path and target format (.xmi or .mid)
	if (!target_path)
	{
		const char *first = *file_list;
		const char *dot = strrchr(first, '.');
		const char *ext = (dot && _stricmp(dot, ".mid") == 0) ? ".mid" : ".xmi";
		char root[PATH_BUF_SIZE];
		char ext_tmp[SUFFIX_BUF_SIZE];

		if (extract_slice_root(first, root, sizeof(root), ext_tmp, sizeof(ext_tmp)))
		{
			sprintf(default_out, "%s_merged%s", root, ext);
		}
		else
		{
			build_out_path(first, "_merged", default_out, sizeof(default_out));
			strcat(default_out, ext);
		}
		target_path = default_out;
	}

	// Save merged stream as XMI first, and if target extension is .mid, convert to MIDI
	{
		const char *out_dot = strrchr(target_path, '.');
		if (out_dot && _stricmp(out_dot, ".mid") == 0)
		{
			char tmp_xmi[PATH_BUF_SIZE];
			long xmi_sz = 0;
			unsigned char *xmi_buf;
			build_out_path(target_path, ".tmp_merge.xmi", tmp_xmi, sizeof(tmp_xmi));
			mid_build_and_save_xmi(merged, merged_count, total_fsize, tmp_xmi,
				timbres, timb_count, slim_mode, quant_hz, 0);
			xmi_buf = load_file_to_buffer(tmp_xmi, &xmi_sz);
			if (xmi_buf)
			{
				convert_xmi_to_mid(xmi_buf, xmi_sz, tmp_xmi, target_path, 0);
				free(xmi_buf);
			}
			remove(tmp_xmi);
		}
		else
		{
			mid_build_and_save_xmi(merged, merged_count, total_fsize, target_path,
				timbres, timb_count, slim_mode, quant_hz, 0);
		}
	}

	// Free duplicated meta_data payloads
	{
		size_t i;
		for (i = 0; i < merged_count; i++)
		{
			if (merged[i].meta_len > 0 && merged[i].meta_data != NULL)
			{
				free(merged[i].meta_data);
			}
		}
	}
	free(merged);
}

// Scans current directory for "_XX." files and merges each matching group
void merge_directory_by_root(const char *root_prefix, const char *out_filename,
	int slim_mode, unsigned int quant_hz)
{
	char *all_files[MAX_MERGE_FILES];
	int total_found = 0;
	int used[MAX_MERGE_FILES];
	int i, j;
	memset(used, 0, sizeof(used));
#ifdef _WIN32
	{
		struct _finddata_t fd;
		intptr_t hFind = _findfirst("*.*", &fd);
		if (hFind != -1)
		{
			do
			{
				if (!(fd.attrib & _A_SUBDIR) && total_found < MAX_MERGE_FILES)
				{
					char r_buf[PATH_BUF_SIZE];
					char e_buf[SUFFIX_BUF_SIZE];
					int matched = 0;

					if (root_prefix)
					{
						matched = matches_explicit_root(fd.name, root_prefix, e_buf, sizeof(e_buf));
					}
					else
					{
						matched = extract_slice_root(fd.name, r_buf, sizeof(r_buf), e_buf, sizeof(e_buf));
					}

					if (matched)
					{
						all_files[total_found] = _strdup(fd.name);
						total_found++;
					}
				}
			}
			while (_findnext(hFind, &fd) == 0);
			_findclose(hFind);
		}
	}
#else
	{
		DIR *dir = opendir(".");
		struct dirent *ent;
		if (dir)
		{
			while ((ent = readdir(dir)) != NULL && total_found < MAX_MERGE_FILES)
			{
				char r_buf[PATH_BUF_SIZE];
				char e_buf[SUFFIX_BUF_SIZE];
				int matched = 0;

				if (root_prefix)
				{
					matched = matches_explicit_root(fd.name, root_prefix, e_buf, sizeof(e_buf));
				}
				else
				{
					matched = extract_slice_root(fd.name, r_buf, sizeof(r_buf), e_buf, sizeof(e_buf));
				}

				if (matched)
				{
					all_files[total_found] = _strdup(fd.name);
					total_found++;
				}
			}
			closedir(dir);
		}
	}
#endif
	if (total_found == 0)
	{
		fprintf(stderr, "No matching slice files (*_01.mid / *_01.xmi) found in current directory.\n");
		return;
	}

	// Sort all matched filenames alphabetically/numerically
	qsort(all_files, (size_t)total_found, sizeof(char *), comp_filenames);

	// Group files by identical (root + extension) and merge each group
	for (i = 0; i < total_found; i++)
	{
		char root_a[PATH_BUF_SIZE];
		char ext_a[SUFFIX_BUF_SIZE];
		const char *batch[MAX_MERGE_FILES];
		int batch_cnt = 0;

		if (used[i])
		{
			continue;
		}

		if (root_prefix)
		{
			strcpy(root_a, root_prefix);
			matches_explicit_root(all_files[i], root_prefix, ext_a, sizeof(ext_a));
		}
		else
		{
			extract_slice_root(all_files[i], root_a, sizeof(root_a), ext_a, sizeof(ext_a));
		}

		batch[batch_cnt++] = all_files[i];
		used[i] = 1;

		for (j = i + 1; j < total_found; j++)
		{
			char root_b[PATH_BUF_SIZE];
			char ext_b[SUFFIX_BUF_SIZE];
			if (!used[j])
			{
				if (root_prefix)
				{
					strcpy(root_b, root_prefix);
					matches_explicit_root(all_files[j], root_prefix, ext_b, sizeof(ext_b));
				}
				else
				{
					extract_slice_root(all_files[j], root_b, sizeof(root_b), ext_b, sizeof(ext_b));
				}

				if (_stricmp(root_a, root_b) == 0 && _stricmp(ext_a, ext_b) == 0)
				{
					batch[batch_cnt++] = all_files[j];
					used[j] = 1;
				}
			}
		}

		if (batch_cnt >= 2)
		{
			char auto_out[PATH_BUF_SIZE];
			const char *group_out = out_filename;
			if (!group_out)
			{
				sprintf(auto_out, "%s_merged%s", root_a, ext_a);
				group_out = auto_out;
			}
			printf("\nFound group '%s*%s' (%d files):\n", root_a, ext_a, batch_cnt);
			merge_files_list(batch, batch_cnt, group_out, slim_mode, quant_hz);
		}
		else
		{
			printf("Skipping single file '%s' (need at least 2 files to merge)\n", all_files[i]);
		}
	}

	for (i = 0; i < total_found; i++)
	{
		free(all_files[i]);
	}

}

//==============================================================================
// 8. CLI Argument Parser & Main Entry Point
//==============================================================================
int main(int argc, char **argv)
{
	FILE *pFi = NULL;
	const char *in_file = NULL;
	const char *out_file = NULL;
	int i, slim_mode = 0;
	unsigned int quant_hz = XMI_FREQ;
	unsigned int slice_max_bytes = 0;
	long fsize;
	unsigned char *midi_data = NULL;

	int merge_mode = 0;
	int mergename_mode = 0;
	const char *merge_root = NULL;
	const char *merge_list[MAX_MERGE_FILES];
	int merge_list_cnt = 0;

	// Parse positional filenames and optional flags in any order
	for (i = 1; i < argc; i++)
	{
		if (_stricmp(argv[i], "-slim") == 0 || _stricmp(argv[i], "/slim") == 0)
		{
			slim_mode = 1;
		}
		else if (strncmp(argv[i], "-q:", 3) == 0 || strncmp(argv[i], "-Q:", 3) == 0 ||
			strncmp(argv[i], "/q:", 3) == 0 || strncmp(argv[i], "/Q:", 3) == 0)
		{
			quant_hz = (unsigned int)strtoul(argv[i] + 3, NULL, 10);
			if (quant_hz == 0)
			{
				quant_hz = XMI_FREQ;
			}
		}
		else if (strncmp(argv[i], "-slice:", 7) == 0 || strncmp(argv[i], "-SLICE:", 7) == 0 ||
			strncmp(argv[i], "/slice:", 7) == 0 || strncmp(argv[i], "/SLICE:", 7) == 0)
		{
			slice_max_bytes = (unsigned int)strtoul(argv[i] + 7, NULL, 10);
		}
		else if (_stricmp(argv[i], "-slice") == 0 || _stricmp(argv[i], "/slice") == 0)
		{
			if (i + 1 < argc)
			{
				slice_max_bytes = (unsigned int)strtoul(argv[++i], NULL, 10);
			}
			else
			{
				fprintf(stderr, "Error: -slice requires a byte size argument\n");
				return -1;
			}
		}
		else if (_stricmp(argv[i], "-merge") == 0 || _stricmp(argv[i], "/merge") == 0)
		{
			merge_mode = 1;
			// Optional root prefix argument (if next arg does not start with '-' or '/')
			if (i + 1 < argc && *argv[i + 1] != '-' && *argv[i + 1] != '/')
			{
				merge_root = argv[++i];
			}
		}
		else if (_stricmp(argv[i], "-mergename") == 0 || _stricmp(argv[i], "/mergename") == 0)
		{
			mergename_mode = 1;
			while (i + 1 < argc && *argv[i + 1] != '-' && *argv[i + 1] != '/')
			{
				if (merge_list_cnt < MAX_MERGE_FILES)
				{
					merge_list[merge_list_cnt++] = argv[++i];
				}
				else
				{
					i++;
				}
			}
		}
		else if (!in_file)
		{
			in_file = argv[i];
		}
		else if (!out_file)
		{
			out_file = argv[i];
		}
		else
		{
			fprintf(stderr, "Error: Unexpected argument: %s\n", argv[i]);
			return -1;
		}
	}

    if (mergename_mode)
    {
		// If user passed output file as last item in list (e.g. "a.xmi b.xmi out.xmi"),
		// or via separate out_file, detect if last file doesn't exist yet on disk
        const char *explicit_out = out_file;
        if (!explicit_out && merge_list_cnt >= 3)
        {
            FILE *test_f = NULL;
            if (fopen_s(&test_f, merge_list[merge_list_cnt - 1], "rb") != 0)
            {
                explicit_out = merge_list[--merge_list_cnt];
            }
            else
            {
                fclose(test_f);
            }
        }
        merge_files_list(merge_list, merge_list_cnt, explicit_out, slim_mode, quant_hz);
        return 0;
    }

	if (merge_mode)
	{
		merge_directory_by_root(merge_root, out_file, slim_mode, quant_hz);
		return 0;
	}

	if (!in_file)
	{
		printf("XMI2MID07 - Bidirectional MIDI <-> XMIDI Converter, Slicer & Merger\n\n");
		printf("Usage:\n");
		printf("  %s <infile> [outfile] [-slim] [-q:Hz] [-slice <bytes>]\n", *argv);
		printf("  %s -merge [root] [outfile] [-slim] [-q:Hz]\n", *argv);
		printf("  %s -mergename <file1> <file2> ... [outfile] [-slim] [-q:Hz]\n\n", *argv);
		printf("Options:\n");
		printf("  -slim          : Strip Aftertouch & duplicate CCs (keeps Pitch Bend)\n");
		printf("  -q:Hz          : Set XMIDI quantization rate in Hz (default: 120)\n");
		printf("  -slice <bytes> : Slice output into multiple valid files <= <bytes>\n");
		printf("  -merge [root]  : Auto-merge sliced *_01.* files in current folder\n");
		printf("  -mergename ... : Merge explicitly listed files in order\n\n");
		printf("Examples:\n");
		printf("  %s e1l4.mid e1l4.xmi -slim\n", *argv);
		printf("  %s e1l4.mid e1l4.xmi -slim -slice 65536\n", *argv);
		printf("  %s -merge\n", *argv);
		printf("  %s -merge e1l4\n", *argv);
		printf("  %s -mergename part1.xmi part2.xmi full.xmi\n", *argv);
		return -1;
	}

	if (fopen_s(&pFi, in_file, "rb"))
	{
		fprintf(stderr, "Error: Cannot open file %s\n", in_file);
		return errno;
	}
	fseek(pFi, 0, SEEK_END);
	fsize = ftell(pFi);
	fseek(pFi, 0, SEEK_SET);

	if (fsize <= 0)
	{
		fprintf(stderr, "Error: Empty or invalid file\n");
		fclose(pFi);
		return -1;
	}

	midi_data = (unsigned char *)malloc((size_t)fsize);

	if (!midi_data)
	{
		fprintf(stderr, "Error: Memory allocation failed\n");
		fclose(pFi);
		return errno;
	}

	if (fsize != (long)fread_s(midi_data, (size_t)fsize, 1, (size_t)fsize, pFi))
	{
		fprintf(stderr, "Error: File read failed\n");
		fclose(pFi);
		free(midi_data);
		return errno;
	}

	fclose(pFi);

	// Auto-detect direction by magic header bytes
	if (fsize >= 4 && memcmp(midi_data, "FORM", 4) == 0)
	{
		convert_xmi_to_mid(midi_data, fsize, in_file, out_file, slice_max_bytes);
	}
	else if (fsize >= 4 && memcmp(midi_data, "MThd", 4) == 0)
	{
		convert_mid_to_xmi(midi_data, fsize, in_file, out_file, slim_mode, quant_hz, slice_max_bytes);
	}
	else
	{
		fprintf(stderr, "Error: Unknown file format (expected 'FORM' for XMI or 'MThd' for MIDI)\n");
		free(midi_data);
		return -1;
	}

	free(midi_data);

	return 0;
}



