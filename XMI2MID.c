//==================== XMI2MID07 Utility by Darkcrafter07 ============================
// License: MIT. I do NOT take any responsibility for a potential damage caused by it.
// ANSI C compatible, MSVC 2017 (v140_xp, WinXP 32-bit) & modern compilers
// Uses John Miles MIDIFORM DDA quantization logic + SMPTE fix + -slim mode
// Fixes overlapping notes when converting from XMI to MIDI.
// Can convert MIDI to XMI, as well as XMI to MIDI.
//====================================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#define _byteswap_ulong __builtin_bswap32
#define _byteswap_ushort __builtin_bswap16
#define _stricmp strcasecmp
#define fopen_s(fp, name, mode) ((*(fp) = fopen((name), (mode))) == NULL ? errno : 0)
#define fread_s(buf, bsz, esz, cnt, fp) fread((buf), (esz), (cnt), (fp))
#endif

#define DEFAULT_TEMPO 120UL
#define XMI_FREQ 120UL
#define DEFAULT_TIMEBASE 60UL
#define DEFAULT_QN 500000UL

static unsigned char midiheader[18] =
{
	'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 60, 'M', 'T', 'r', 'k'
};

static unsigned short timebase = 960;
static unsigned int qnlen = DEFAULT_QN;

struct NOEVENTS
{
	unsigned int delta;
	unsigned char off[3];
};

static struct NOEVENTS off_events[2048];

static int comp_events(const void *pa, const void *pb)
{
	const struct NOEVENTS *a = (const struct NOEVENTS *)pa;
	const struct NOEVENTS *b = (const struct NOEVENTS *)pb;
	if (a->delta < b->delta) return -1;
	if (a->delta > b->delta) return 1;
	return 0;
}

//============================================================================
// Structures and helpers for MIDI -> XMI conversion
//============================================================================
struct RawMidiEvent
{
	unsigned long abs_tick;
	unsigned char status;
	unsigned char d1;
	unsigned char d2;
	unsigned char *meta_data;
	unsigned long meta_len;
	unsigned long duration_ticks;
	int skip;
	unsigned long seq_order;
};

static int comp_raw_events(const void *pa, const void *pb)
{
	const struct RawMidiEvent *ea = (const struct RawMidiEvent *)pa;
	const struct RawMidiEvent *eb = (const struct RawMidiEvent *)pb;
	if (ea->abs_tick < eb->abs_tick) return -1;
	if (ea->abs_tick > eb->abs_tick) return 1;
	if (ea->seq_order < eb->seq_order) return -1;
	if (ea->seq_order > eb->seq_order) return 1;
	return 0;
}

static unsigned long read_vlq(unsigned char **ptr, unsigned char *end)
{
	unsigned long val = 0;
	unsigned char c;
	while (*ptr < end)
	{
		c = *(*ptr)++;
		val = (val << 7) | (c & 0x7F);
		if (!(c & 0x80)) break;
	}
	return val;
}

static void write_vlq(unsigned char **ptr, unsigned long val)
{
	unsigned long buffer = val & 0x7F;
	while ((val >>= 7))
	{
		buffer <<= 8;
		buffer |= ((val & 0x7F) | 0x80);
	}
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

static void write_xmi_delay(unsigned char **ptr, unsigned long delay)
{
	if (delay == 0) return;
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

//============================================================================
// XMI -> MIDI Conversion
// ============================================================================
static void convert_xmi_to_mid(unsigned char *midi_data, long fsize, const char *in_filename, const char *out_filename)
{
	FILE *pFo = NULL;
	unsigned char *cur = midi_data;
	unsigned char *midi_decode = NULL;
	unsigned char *midi_write = NULL;
	unsigned char *dcur, *tcur, *pos, *st;
	unsigned int lFORM, lINFO, lCAT, lFORM2, lTIMB, lEVNT, dlen, tlen, oevents;
	unsigned short seqCount;
	int next_is_delta;
	char pt[512];
	const char *target_path;

	off_events[0].delta = 0xFFFFFFFFUL;
	off_events[0].off[0] = 0;
	off_events[0].off[1] = 0;
	off_events[0].off[2] = 0;

	if (memcmp(cur, "FORM", 4))
	{
		fprintf(stderr, "Warning: Missing FORM\n");
	}
	cur += 4;
	lFORM = _byteswap_ulong(*(unsigned int *)cur);
	(void)lFORM;
	cur += 4;

	if (memcmp(cur, "XDIR", 4))
	{
		fprintf(stderr, "Warning: Missing XDIR\n");
	}
	cur += 4;

	if (memcmp(cur, "INFO", 4))
	{
		fprintf(stderr, "Warning: Missing INFO\n");
	}
	cur += 4;
	lINFO = _byteswap_ulong(*(unsigned int *)cur);
	(void)lINFO;
	cur += 4;

	seqCount = *(unsigned short *)cur;
	cur += 2;
	printf("seqCount: %d\n", seqCount);

	if (memcmp(cur, "CAT ", 4))
	{
		fprintf(stderr, "Warning: Missing CAT \n");
	}
	cur += 4;
	lCAT = _byteswap_ulong(*(unsigned int *)cur);
	(void)lCAT;
	cur += 4;

	if (memcmp(cur, "XMID", 4))
	{
		fprintf(stderr, "Warning: Missing XMID\n");
	}
	cur += 4;

	if (memcmp(cur, "FORM", 4))
	{
		fprintf(stderr, "Warning: Missing inner FORM\n");
	}
	cur += 4;
	lFORM2 = _byteswap_ulong(*(unsigned int *)cur);
	(void)lFORM2;
	cur += 4;

	if (memcmp(cur, "XMID", 4))
	{
		fprintf(stderr, "Warning: Missing inner XMID\n");
	}
	cur += 4;

	if (!memcmp(cur, "TIMB", 4))
	{
		unsigned int i;
		cur += 4;
		lTIMB = _byteswap_ulong(*(unsigned int *)cur);
		cur += 4;
		for (i = 0; i < lTIMB; i += 2)
		{
			cur += 2;
		}
	}

	if (!memcmp(cur, "RBRN", 4))
	{
		unsigned int lRBRN;
		unsigned short nBranch;
		cur += 4;
		lRBRN = _byteswap_ulong(*(unsigned int *)cur);
		(void)lRBRN;
		cur += 4;
		nBranch = *(unsigned short *)cur;
		cur += 2 + (nBranch * 6);
	}

	if (memcmp(cur, "EVNT", 4))
	{
		fprintf(stderr, "Error: Not XMIDI file (EVNT missing)\n");
		exit(-1);
	}
	cur += 4;

	lEVNT = _byteswap_ulong(*(unsigned int *)cur);
	cur += 4;
	printf("EVNT length: %u bytes\n", lEVNT);

	midi_decode = (unsigned char *)malloc((size_t)fsize * 4 + 4096);
	if (!midi_decode)
	{
		fprintf(stderr, "Memory allocation error\n");
		exit(errno);
	}
	dcur = midi_decode;
	next_is_delta = 1;
	st = cur;
	oevents = 0;

	while ((unsigned int)(cur - st) < lEVNT)
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

			while (oevents > 0 && delay > off_events[0].delta)
			{
				unsigned int no_delta = off_events[0].delta;
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
				*dcur++ = off_events[0].off[0] & 0x8F;
				*dcur++ = off_events[0].off[1];
				*dcur++ = 0x7F;

				delay -= off_events[0].delta;
				for (i = 1; i < oevents; i++)
				{
					off_events[i].delta -= off_events[0].delta;
				}
				off_events[0].delta = 0xFFFFFFFFUL;
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
						*dcur++ = off_events[i].off[0] & 0x8F;
						*dcur++ = off_events[i].off[1];
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
					unsigned long mlen = read_vlq(&cur, st + lEVNT);
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

				// If this exact note on this channel is already active, close it first to prevent overlap
				for (k = 0; k < oevents; k++)
				{
					if (off_events[k].off[0] == st_byte && off_events[k].off[1] == note_byte)
					{
						*dcur++ = (unsigned char)(st_byte & 0x8F);
						*dcur++ = note_byte;
						*dcur++ = 0x00;
						*dcur++ = 0x00; // zero delta before the new Note On

						off_events[k].delta = 0xFFFFFFFFUL;
						qsort(off_events, oevents, sizeof(struct NOEVENTS), comp_events);
						oevents--;
						break;
					}
				}

				*dcur++ = st_byte;
				*dcur++ = note_byte;
				*dcur++ = vel_byte;

				if (oevents < 2048)
				{
					off_events[oevents].delta = delta;
					off_events[oevents].off[0] = st_byte;
					off_events[oevents].off[1] = note_byte;
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
					unsigned long slen = read_vlq(&cur, st + lEVNT);
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

	dlen = (unsigned int)(dcur - midi_decode);
	midi_write = (unsigned char *)malloc((size_t)dlen * 2 + 4096);
	if (!midi_write)
	{
		fprintf(stderr, "Memory allocation error\n");
		free(midi_decode);
		exit(errno);
	}
	tcur = midi_write;
	pos = midi_decode;

	while (pos < dcur)
	{
		unsigned int delta = 0;
		unsigned int tdelta;
		double factor;

		while (*pos & 0x80)
		{
			delta += *pos++ & 0x7F;
			delta <<= 7;
		}
		delta += *pos++ & 0x7F;

		factor = (double)timebase * (double)DEFAULT_QN / ((double)qnlen * (double)DEFAULT_TIMEBASE);
		delta = (unsigned int)((double)delta * factor + 0.5);

		tdelta = delta & 0x7F;
		while ((delta >>= 7))
		{
			tdelta <<= 8;
			tdelta |= (delta & 0x7F) | 0x80;
		}
		while (1)
		{
			*tcur++ = (unsigned char)(tdelta & 0xFF);
			if (tdelta & 0x80)
			{
				tdelta >>= 8;
			}
			else
			{
				break;
			}
		}

		if (0x80 == (*pos & 0xF0) || 0x90 == (*pos & 0xF0) || 0xA0 == (*pos & 0xF0) ||
			0xB0 == (*pos & 0xF0) || 0xE0 == (*pos & 0xF0))
		{
			*tcur++ = *pos++;
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xC0 == (*pos & 0xF0) || 0xD0 == (*pos & 0xF0))
		{
			*tcur++ = *pos++;
			*tcur++ = *pos++;
		}
		else if (0xF0 == *pos || 0xF7 == *pos)
		{
			unsigned int exlen = 0;
			*tcur++ = *pos++;
			while (*pos & 0x80)
			{
				exlen += *pos & 0x7F;
				exlen <<= 7;
				*tcur++ = *pos++;
			}
			exlen += *pos & 0x7F;
			*tcur++ = *pos++;
			while (exlen--)
			{
				*tcur++ = *pos++;
			}
		}
		else if (0xFF == *pos)
		{
			*tcur++ = *pos++;
			if (0x51 == *pos)
			{
				*tcur++ = *pos++;
				*tcur++ = *pos++;
				qnlen = ((unsigned int)pos[0] << 16) + ((unsigned int)pos[1] << 8) + (unsigned int)pos[2];
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
	}
	tlen = (unsigned int)(tcur - midi_write);
	target_path = out_filename;
	if (!target_path)
	{
		build_out_path(in_filename, ".mid", pt, sizeof(pt));
		target_path = pt;
	}
	if (fopen_s(&pFo, target_path, "wb"))
	{
		fprintf(stderr, "Cannot open output file %s\n", target_path);
		free(midi_decode);
		free(midi_write);
		exit(errno);
	}
	{
		unsigned short *mh_timebase = (unsigned short *)&midiheader[12];
		unsigned int bs_tlen = _byteswap_ulong(tlen);
		*mh_timebase = _byteswap_ushort(timebase);
		fwrite(midiheader, 1, 18, pFo);
		fwrite(&bs_tlen, 4, 1, pFo);
		fwrite(midi_write, 1, tlen, pFo);
	}
	fclose(pFo);
	free(midi_decode);
	free(midi_write);
	printf("Converted XMI -> %s (%u bytes)\n", target_path, tlen + 22);
}

//============================================================================
// MIDI -> XMI Conversion (MIDIFORM DDA algorithm + SMPTE fix + -slim mode)
//============================================================================
static void convert_mid_to_xmi(unsigned char *midi_data, long fsize, const char *in_filename,
	const char *out_filename, int slim_mode, unsigned int quant_hz)
{
	unsigned char *cur = midi_data;
	unsigned char *end = midi_data + fsize;
	unsigned int hdr_len;
	unsigned short format, ntrks, division, t;
	int is_smpte = 0;
	double smpte_ticks_per_sec = 0.0;
	size_t ev_cap = 16384;
	size_t ev_count = 0;
	struct RawMidiEvent *events = NULL;
	unsigned char bank_msb[16];
	unsigned short timbres[256];
	size_t timb_count = 0;
	size_t i;

	if (fsize < 14 || memcmp(cur, "MThd", 4))
	{
		fprintf(stderr, "Invalid MIDI header (MThd)\n");
		exit(-1);
	}

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

	if (division & 0x8000)
	{
		unsigned char hi = (unsigned char)((division >> 8) & 0xFF);
		int fps_val = (hi >= 0xE0) ? -(signed char)hi : (hi & 0x7F);
		unsigned char tpf = (unsigned char)(division & 0xFF);
		double fps; is_smpte = 1;
		if (fps_val <= 0 || fps_val > 60) fps_val = 25;
		fps = (fps_val == 29) ? 29.97 : (double)fps_val;
		if (tpf == 0) tpf = 40; smpte_ticks_per_sec = fps * (double)tpf;
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
	
	memset(bank_msb, 0, sizeof(bank_msb));
	events = (struct RawMidiEvent *)malloc(ev_cap * sizeof(struct RawMidiEvent));
	if (!events)
	{
		fprintf(stderr, "Memory allocation error\n");
		exit(errno);
	}
	
	for (t = 0; t < ntrks && cur < end; t++)
	{
		unsigned int trk_len;
		unsigned char *trk_end;
		unsigned long abs_tick = 0;
		unsigned char running_status = 0; if (cur + 8 > end || memcmp(cur, "MTrk", 4))
		{
			break;
		}

		cur += 4;
		trk_len = _byteswap_ulong(*(unsigned int *)cur);
		cur += 4;
		trk_end = cur + trk_len;

		if (trk_end > end) trk_end = end; while (cur < trk_end)
		{
			unsigned long delta = read_vlq(&cur, trk_end);
			unsigned char status, cmd, ch;
			struct RawMidiEvent *ev; abs_tick += delta;
			if (cur >= trk_end) break; status = *cur;
			if (status & 0x80)
			{
				cur++;
				if (status < 0xF0) running_status = status;
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
					fprintf(stderr, "Memory reallocation error\n");
					exit(errno);
				}
			}
			
			ev = &events[ev_count];
			memset(ev, 0, sizeof(struct RawMidiEvent));
			ev->abs_tick = abs_tick;
			ev->status = status;
			ev->seq_order = (unsigned long)ev_count; cmd = status & 0xF0;
			ch = status & 0x0F; if (cmd == 0x80 || cmd == 0x90 || cmd == 0xA0 || cmd == 0xB0 || cmd == 0xE0)
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
					if (!found && timb_count < 256)
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
	
	// John Miles MIDIFORM DDA accumulator algorithm
	qsort(events, ev_count, sizeof(struct RawMidiEvent), comp_raw_events);
	{
		const long double q_int = 100000000.0L / (long double)quant_hz;
		long double tick_time;
		long double dda_sum = 0.0L;
		unsigned long xmi_interval = 0;
		unsigned long last_midi_tick = 0; if (is_smpte)
		{
			tick_time = 100000000.0L / (long double)smpte_ticks_per_sec;
		}
		else
		{
			tick_time = 50000000.0L / (long double)division;
		}
		
		for (i = 0; i < ev_count; i++)
		{
			unsigned long event_delta = events[i].abs_tick - last_midi_tick;
			last_midi_tick = events[i].abs_tick; if (event_delta > 0)
			{
				dda_sum += (long double)event_delta * tick_time;
				while (dda_sum >= q_int)
				{
					dda_sum -= q_int;
					xmi_interval++;
				}
			}
			events[i].abs_tick = xmi_interval;		
			if (!is_smpte && events[i].status == 0xFF && events[i].d1 == 0x51 && events[i].meta_len == 3)
			{
				unsigned long tempo_us = ((unsigned long)events[i].meta_data[0] << 16) |
					((unsigned long)events[i].meta_data[1] << 8) |
					(unsigned long)events[i].meta_data[2];
				tick_time = (100.0L * (long double)tempo_us) / (long double)division;
			}
		}
	}
	
	// Pair Note On with Note Off
	for (i = 0; i < ev_count; i++)
	{
		if ((events[i].status & 0xF0) == 0x90)
		{
			unsigned char ch = events[i].status & 0x0F;
			unsigned char note = events[i].d1;
			unsigned long dur = 1;
			size_t j; for (j = i + 1; j < ev_count; j++)
			{
				if (!events[j].skip && (events[j].status & 0xF0) == 0x80 &&
					(events[j].status & 0x0F) == ch && events[j].d1 == note)
				{
					dur = events[j].abs_tick - events[i].abs_tick;
					if (dur == 0) dur = 1;
					events[j].skip = 1;
					break;
				}
			}
			events[i].duration_ticks = dur;
		}
	}
	
	// Build EVNT payload
	{
		unsigned char *evnt_buf = (unsigned char *)malloc((size_t)fsize * 4 + 4096);
		unsigned char *ecur;
		unsigned long prev_xmi_tick = 0;
		short last_cc_val[16][128];
		int last_pitch_val[16];
		unsigned int lEVNT, evnt_pad, lTIMB, lFORM_inner, lCAT, lFORM_outer, bs_val;
		unsigned short seqCount = 1;
		char pt[512];
		const char *target_path;

		FILE *pFo = NULL; if (!evnt_buf)
		{
			fprintf(stderr, "Memory allocation error\n");
			free(events);
			exit(errno);
		}
		
		if (slim_mode)
		{
			int c;
			memset(last_cc_val, -1, sizeof(last_cc_val));
			for (c = 0; c < 16; c++) last_pitch_val[c] = -1;
		}
		
		ecur = evnt_buf; for (i = 0; i < ev_count; i++)
		{
			unsigned char st, cmd, ch;
			unsigned long delay; if (events[i].skip) continue;
			st = events[i].status;
			cmd = st & 0xF0;

			ch = st & 0x0F; if (cmd == 0x80) continue;
			
			if (slim_mode)
			{
				if (cmd == 0xA0 || cmd == 0xD0) continue;
				if (st == 0xFF && events[i].d1 >= 0x01 && events[i].d1 <= 0x07) continue;
				
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

			delay = events[i].abs_tick - prev_xmi_tick;
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

		*ecur++ = 0xFF;
		*ecur++ = 0x2F;
		*ecur++ = 0x00;
		lEVNT = (unsigned int)(ecur - evnt_buf);
		evnt_pad = (lEVNT % 2 != 0) ? 1 : 0;

		if (timb_count == 0)
		{
			timbres[timb_count++] = 0x0000;
		}

		lTIMB = (unsigned int)(timb_count * 2);
		lFORM_inner = 4 + 8 + lTIMB + 8 + lEVNT + evnt_pad;
		lCAT = 4 + 8 + lFORM_inner;
		lFORM_outer = 4 + 8 + 2;
		target_path = out_filename;

		if (!target_path)
		{
			build_out_path(in_filename, ".xmi", pt, sizeof(pt));
			target_path = pt;
		}

		if (fopen_s(&pFo, target_path, "wb"))
		{
			fprintf(stderr, "Cannot open output file %s\n", target_path);
			free(evnt_buf);
			free(events);
			exit(errno);
		}

		fwrite("FORM", 1, 4, pFo);
		bs_val = _byteswap_ulong(lFORM_outer);
		fwrite(&bs_val, 4, 1, pFo);
		fwrite("XDIRINFO", 1, 8, pFo);
		bs_val = _byteswap_ulong(2);
		fwrite(&bs_val, 4, 1, pFo);
		fwrite(&seqCount, 2, 1, pFo);
		fwrite("CAT ", 1, 4, pFo);
		bs_val = _byteswap_ulong(lCAT);
		fwrite(&bs_val, 4, 1, pFo);
		fwrite("XMIDFORM", 1, 8, pFo);
		bs_val = _byteswap_ulong(lFORM_inner);
		fwrite(&bs_val, 4, 1, pFo);
		fwrite("XMID", 1, 4, pFo);
		fwrite("TIMB", 1, 4, pFo);
		bs_val = _byteswap_ulong(lTIMB);
		fwrite(&bs_val, 4, 1, pFo);

		for (i = 0; i < timb_count; i++)
		{
			unsigned char p = (unsigned char)(timbres[i] & 0xFF);
			unsigned char b = (unsigned char)((timbres[i] >> 8) & 0xFF);
			fwrite(&p, 1, 1, pFo);
			fwrite(&b, 1, 1, pFo);
		}

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
		free(evnt_buf);
		free(events);
		printf("TIMB: %u entries, EVNT: %u bytes (Quant: %u Hz)\n", (unsigned int)timb_count, lEVNT, quant_hz);
		printf("Converted MIDI -> %s\n", target_path);
	}
}

//============================================================================
// Main Entry Point
//============================================================================
int main(int argc, char **argv)
{
	FILE *pFi = NULL;
	const char *in_file = NULL;
	const char *out_file = NULL;
	int slim_mode = 0;
	unsigned int quant_hz = XMI_FREQ;
	int i;
	long fsize;
	unsigned char *midi_data = NULL;
	
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
			if (quant_hz == 0) quant_hz = XMI_FREQ;
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
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			return -1;
		}
	}

	if (!in_file)
	{
		printf("Usage: %s [outfile] [-slim] [-q:Hz]\n", argv[0]);
		printf(" -slim : Strip Aftertouch & duplicate CCs (keeps Pitch Bend)\n");
		printf(" -q:Hz : Set XMIDI quantization rate in Hz (default: 120)\n");
		printf("Examples:\n");
		printf(" %s e1l4.mid e1l4.xmi -slim\n", argv[0]);
		printf(" %s e1l4.mid e1l4.xmi -slim -q:480\n", argv[0]);
		return -1;
	}

	if (fopen_s(&pFi, in_file, "rb"))
	{
		fprintf(stderr, "Cannot open file %s\n", in_file);
		return errno;
	}
	
	fseek(pFi, 0, SEEK_END);
	fsize = ftell(pFi);

	fseek(pFi, 0, SEEK_SET); if (fsize <= 0)
	{
		fprintf(stderr, "Empty or invalid file\n");
		fclose(pFi);
		return -1;
	}
	
	midi_data = (unsigned char *)malloc((size_t)fsize);

	if (!midi_data)
	{
		fprintf(stderr, "Memory allocation error\n");
		fclose(pFi);
		return errno;
	}

	if (fsize != (long)fread_s(midi_data, (size_t)fsize, 1, (size_t)fsize, pFi))
	{
		fprintf(stderr, "File read error\n");
		fclose(pFi);
		free(midi_data);
		return errno;
	}

	fclose(pFi); if (fsize >= 4 && memcmp(midi_data, "FORM", 4) == 0)
	{
		convert_xmi_to_mid(midi_data, fsize, in_file, out_file);
	}
	else if (fsize >= 4 && memcmp(midi_data, "MThd", 4) == 0)
	{
		convert_mid_to_xmi(midi_data, fsize, in_file, out_file, slim_mode, quant_hz);
	}
	else
	{
		fprintf(stderr, "Unknown file format (expected FORM or MThd)\n");
		free(midi_data);
		return -1;
	}
	
	free(midi_data);

	return 0;
}


