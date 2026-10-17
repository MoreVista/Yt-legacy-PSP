#include "mp4demux.h"
#include "../util/log.h"
#include <pspiofilemgr.h>

/* ---- big-endian readers (MP4 is network byte order) ---- */

static uint32_t rd32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const unsigned char *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint64_t rd64(const unsigned char *p)
{
	return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static void wr32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void wr16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

/* A 15 minute 360p clip is ~32k video samples, so a dynamic array is right. */
#define INITIAL_SAMPLE_CAP 512

/* ------------------------------------------------------------------ */
/* box walking                                                         */
/* ------------------------------------------------------------------ */

static bool box_header(SceUID fd, uint64_t pos, uint64_t limit,
                       char *type, uint64_t *size, uint64_t *hdr)
{
	unsigned char buf[8];
	if (pos + 8 > limit)
		return false;
	if (sceIoLseek(fd, (SceOff)pos, SEEK_SET) < 0)
		return false;
	if (sceIoRead(fd, buf, 8) != 8)
		return false;

	uint32_t s = rd32(buf);
	memcpy(type, buf + 4, 4);
	type[4] = 0;

	if (s == 1) {
		unsigned char ext[8];
		if (sceIoRead(fd, ext, 8) != 8)
			return false;
		*size = rd64(ext);
		*hdr = 16;
	} else if (s == 0) {
		*size = limit - pos;
		*hdr = 8;
	} else {
		*size = s;
		*hdr = 8;
	}
	return *size >= *hdr;
}

/* Collects the sample tables for one track. */
typedef struct {
	uint32_t *stts_count;
	uint32_t *stts_delta;
	int       stts_entries;

	uint32_t *stss_index;
	int       stss_entries;

	uint32_t  sample_size;      /* non-zero when every sample is equal */
	uint32_t *stsz_size;
	int       stsz_entries;

	uint32_t *stsc_first_chunk;
	uint32_t *stsc_samples_per_chunk;
	int       stsc_entries;

	uint32_t *chunk_offset;
	uint32_t *chunk_samples;
	int       chunk_count;
	int       chunk_cap;
} tables_t;

/* Parameter sets lifted out of the avcC box. */
typedef struct {
	uint8_t sps[64];
	int     sps_size;
	uint8_t pps[64];
	int     pps_size;
	uint8_t nal_length_size;
	uint8_t profile_idc;
	uint8_t profile_compat;
	uint8_t level_idc;
} avcc_info_t;

/*
 * Parses an avcC configuration record, which is not a box: it starts with
 * configurationVersion, then the profile/level triple, then the NAL length
 * size, then length-prefixed SPS and PPS lists.
 */
static void read_avcc(SceUID fd, uint64_t payload, uint64_t size, avcc_info_t *out)
{
	memset(out, 0, sizeof(*out));
	out->nal_length_size = 4;

	if (size < 7)
		return;

	unsigned char head[6];
	if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) < 0)
		return;
	if (sceIoRead(fd, head, 6) != 6)   /* ver, profile, compat, level, lenSize, numSPS */
		return;

	out->profile_idc = head[1];
	out->profile_compat = head[2];
	out->level_idc = head[3];
	out->nal_length_size = (uint8_t)((head[4] & 0x03) + 1);

	int num_sps = head[5] & 0x1F;
	if (num_sps > 0) {
		unsigned char lb[2];
		if (sceIoRead(fd, lb, 2) != 2)
			return;
		int len = rd16(lb);
		if (len > 0 && len <= (int)sizeof(out->sps) &&
		    sceIoRead(fd, out->sps, len) == len)
			out->sps_size = len;
	}

	unsigned char npps;
	if (sceIoRead(fd, &npps, 1) != 1)
		return;
	if (npps > 0) {
		unsigned char lb[2];
		if (sceIoRead(fd, lb, 2) != 2)
			return;
		int len = rd16(lb);
		if (len > 0 && len <= (int)sizeof(out->pps) &&
		    sceIoRead(fd, out->pps, len) == len)
			out->pps_size = len;
	}
}

static void tables_init(tables_t *t)
{
	memset(t, 0, sizeof(*t));
}

static void tables_free(tables_t *t)
{
	free(t->stts_count);
	free(t->stss_index);
	free(t->stsz_size);
	free(t->stsc_first_chunk);
	free(t->chunk_offset);
	free(t->chunk_samples);
	memset(t, 0, sizeof(*t));
}

static int tables_push_chunk(tables_t *t, uint64_t off, uint32_t samples)
{
	if (t->chunk_count == t->chunk_cap) {
		int ncap = t->chunk_cap ? t->chunk_cap * 2 : 1024;
		uint32_t *no = realloc(t->chunk_offset, (size_t)ncap * sizeof(uint32_t));
		if (!no)
			return -1;
		t->chunk_offset = no;
		uint32_t *ns = realloc(t->chunk_samples, (size_t)ncap * sizeof(uint32_t));
		if (!ns)
			return -1;
		t->chunk_samples = ns;
		t->chunk_cap = ncap;
	}
	t->chunk_offset[t->chunk_count] = (uint32_t)off;
	t->chunk_samples[t->chunk_count] = samples;
	t->chunk_count++;
	return 0;
}

/*
 * Reads `count` entries of `entry_size` bytes in one sceIoRead. The tables
 * of a long video run to hundreds of thousands of entries, and reading them
 * one entry per call made opening a 1.5-hour file take minutes on the PSP --
 * long enough to starve a download writing to the same memory stick.
 */
static unsigned char *read_entries(SceUID fd, uint32_t count, int entry_size)
{
	uint32_t bytes = count * (uint32_t)entry_size;
	unsigned char *raw = malloc(bytes ? bytes : 1);
	if (!raw)
		return NULL;
	if (bytes && sceIoRead(fd, raw, bytes) != (int)bytes) {
		free(raw);
		return NULL;
	}
	return raw;
}

/*
 * Reads a FullBox sample table. `payload` points just past the box header, so
 * the 4 bytes of version/flags come first, then a 32-bit entry count, then the
 * entries themselves (each entry_size bytes: 4 for a single value, 8 for a
 * pair stored as two consecutive uint32 in the returned array).
 */
static uint32_t *read_entry_table(SceUID fd, uint64_t payload, int *out_count,
                                  int entry_size)
{
	unsigned char head[8];
	if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) < 0)
		return NULL;
	if (sceIoRead(fd, head, 8) != 8)   /* version/flags + entry_count */
		return NULL;

	uint32_t count = rd32(head + 4);
	if (count > (1u << 24))
		return NULL;

	uint32_t *out = count ? malloc((size_t)count * (entry_size == 8 ? 2 : 1) *
	                              sizeof(uint32_t)) : NULL;
	if (count && !out)
		return NULL;
	unsigned char *raw = read_entries(fd, count, entry_size);
	if (!raw) {
		free(out);
		return NULL;
	}
	for (uint32_t i = 0; i < count; i++) {
		const unsigned char *buf = raw + (size_t)i * entry_size;
		if (entry_size == 4)
			out[i] = rd32(buf);
		else {
			out[i * 2]     = rd32(buf);
			out[i * 2 + 1] = rd32(buf + 4);
		}
	}
	free(raw);
	*out_count = (int)count;
	return out;
}

/*
 * Reads an stsc table into a compact (first_chunk, samples_per_chunk) pair
 * list. stsc entries are 12 bytes each, the third field being a sample
 * description index that this demuxer has no use for.
 */
static uint32_t *read_entry_table12(SceUID fd, uint64_t payload, int *out_count)
{
	unsigned char head[8];
	if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) < 0)
		return NULL;
	if (sceIoRead(fd, head, 8) != 8)
		return NULL;

	uint32_t count = rd32(head + 4);
	if (count > (1u << 20))
		return NULL;

	uint32_t *out = count ? malloc((size_t)count * 2 * sizeof(uint32_t)) : NULL;
	if (count && !out)
		return NULL;
	unsigned char *raw = read_entries(fd, count, 12);
	if (!raw) {
		free(out);
		return NULL;
	}
	for (uint32_t i = 0; i < count; i++) {
		const unsigned char *e = raw + (size_t)i * 12;
		out[i * 2]     = rd32(e);      /* first_chunk           */
		out[i * 2 + 1] = rd32(e + 4);  /* samples_per_chunk     */
	}
	free(raw);
	*out_count = (int)count;
	return out;
}

/* Walks a stbl box, filling `t`, and separately picks up mdhd timescale. */
static int parse_stbl(SceUID fd, uint64_t start, uint64_t end, tables_t *t,
                      uint32_t *timescale, uint32_t *width, uint32_t *height,
                      int *channels, int *sample_rate, avcc_info_t *avcc_out)
{
	uint64_t p = start;

	while (p < end) {
		char type[5];
		uint64_t size, hdr;
		if (!box_header(fd, p, end, type, &size, &hdr))
			break;
		if (size < hdr)
			break;

		uint64_t payload = p + hdr;
		uint64_t pend = p + size;

		if (!strcmp(type, "mdhd")) {
			unsigned char buf[24];
			if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0 &&
			    sceIoRead(fd, buf, 24) == 24) {
				/* version(1) flags(3) creation(4) modification(4) */
				if (buf[0] == 1) {
					if (timescale)
						*timescale = rd32(buf + 20);
				} else if (timescale) {
					*timescale = rd32(buf + 12);
				}
			}
		} else if (!strcmp(type, "tkhd")) {
			unsigned char buf[84];
			if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0) {
				int n = sceIoRead(fd, buf, sizeof(buf));
				if (n >= 84 && buf[0] == 0) {
					if (width)
						*width = rd32(buf + 76) >> 16;
					if (height)
						*height = rd32(buf + 80) >> 16;
				}
			}
		} else if (!strcmp(type, "hdlr")) {
			unsigned char buf[12];
			if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0 &&
			    sceIoRead(fd, buf, 12) == 12) {
				if (channels)
					*channels = rd16(buf + 10);
			}
		} else if (!strcmp(type, "stsd")) {
			/* stsd is a FullBox: version/flags, entry_count, then the
			 * sample entries. A VisualSampleEntry stores width/height at
			 * offset 32/34 and an AudioSampleEntry stores channel count at
			 * 24 and sample rate at 32. For video the entry also holds
			 * child boxes, the first of which is avcC. */
			unsigned char buf[8];
			if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0 &&
			    sceIoRead(fd, buf, 8) == 8) {
				int entries = rd32(buf + 4);
				if (width && entries > 0) {
					unsigned char e[40];
					if (sceIoRead(fd, e, sizeof(e)) == (int)sizeof(e)) {
						*width = rd16(e + 32);
						*height = rd16(e + 34);
					}
					/* Child boxes start 86 bytes after the start of the
					 * sample entry: 8 bytes of box header plus 78 bytes of
					 * VisualSampleEntry fields. */
					if (avcc_out) {
						uint64_t child = payload + 8 + 78 + 8;
						uint64_t limit = p + size;
						while (child + 8 <= limit) {
							char ct[5];
							uint64_t cs, ch;
							if (!box_header(fd, child, limit, ct, &cs, &ch))
								break;
							if (!strcmp(ct, "avcC")) {
								read_avcc(fd, child + ch, cs - ch, avcc_out);
								break;
							}
							if (cs < ch)
								break;
							child += cs;
						}
					}
				} else if (sample_rate && channels && entries > 0) {
					unsigned char e[36];
					if (sceIoRead(fd, e, sizeof(e)) == (int)sizeof(e)) {
						*channels = rd16(e + 24);
						*sample_rate = rd16(e + 32);
					}
				}
			}
		} else if (!strcmp(type, "stts")) {
			/* stts_count and stts_delta alias the same allocation. */
			free(t->stts_count);
			t->stts_count = t->stts_delta = NULL;
			t->stts_count = read_entry_table(fd, payload,
			                                 &t->stts_entries, 8);
			t->stts_delta = t->stts_count;
		} else if (!strcmp(type, "stss")) {
			free(t->stss_index);
			t->stss_index = read_entry_table(fd, payload,
			                                 &t->stss_entries, 4);
		} else if (!strcmp(type, "stsz")) {
			unsigned char buf[12];
			if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0 &&
			    sceIoRead(fd, buf, 12) == 12) {
				t->sample_size = rd32(buf + 4);
				uint32_t count = rd32(buf + 8);
				if (t->sample_size == 0 && count) {
					free(t->stsz_size);
					t->stsz_size = malloc((size_t)count * sizeof(uint32_t));
					unsigned char *raw = t->stsz_size && count <= (1u << 24)
					                   ? read_entries(fd, count, 4) : NULL;
					if (raw) {
						for (uint32_t i = 0; i < count; i++)
							t->stsz_size[i] = rd32(raw + (size_t)i * 4);
						t->stsz_entries = (int)count;
						free(raw);
					}
				}
			}
		} else if (!strcmp(type, "stsc")) {
			/* stsc_first_chunk and stsc_samples_per_chunk alias one buffer.
			 * Each entry is 12 bytes: first_chunk, samples_per_chunk and
			 * sample_description_index. Only the first two matter here, so
			 * they are compacted into a (first_chunk, spc) pair list. */
			free(t->stsc_first_chunk);
			t->stsc_first_chunk = t->stsc_samples_per_chunk = NULL;
			t->stsc_first_chunk = read_entry_table12(fd, payload,
			                                         &t->stsc_entries);
			t->stsc_samples_per_chunk = t->stsc_first_chunk;
		} else if (!strcmp(type, "stco") || !strcmp(type, "co64")) {
			int entries = 0;
			int esize = !strcmp(type, "stco") ? 4 : 8;
			if (esize == 8) {
				/* co64: not emitted by the itag 18 muxer, but handle it. */
				unsigned char head[8];
				if (sceIoLseek(fd, (SceOff)payload, SEEK_SET) >= 0 &&
				    sceIoRead(fd, head, 8) == 8) {
					uint32_t count = rd32(head + 4);
					unsigned char *raw = count <= (1u << 24)
					                   ? read_entries(fd, count, 8) : NULL;
					for (uint32_t i = 0; raw && i < count; i++)
						if (tables_push_chunk(t, rd64(raw + (size_t)i * 8), 0) < 0)
							break;
					free(raw);
					entries = (int)count;
				}
			} else {
				uint32_t *offs = read_entry_table(fd, payload,
				                                   &entries, 4);
				if (offs) {
					for (int i = 0; i < entries; i++)
						if (tables_push_chunk(t, offs[i], 0) < 0)
							break;
					free(offs);
				}
			}
			(void)entries;
		}

		p += size;
	}
	return 0;
}

/*
 * Expands the sample tables into a flat list of (offset, size, timing)
 * entries covering every sample in the track.
 */
static int build_samples(tables_t *t, mp4_sample_t **out, int *out_count)
{
	if (t->stts_entries <= 0 || t->chunk_count <= 0)
		return -1;

	int total = 0;
	for (int i = 0; i < t->stts_entries; i++)
		total += (int)t->stts_count[i * 2];
	if (total <= 0)
		return -1;

	mp4_sample_t *samples = malloc((size_t)total * sizeof(mp4_sample_t));
	if (!samples)
		return -1;

	/* Resolve samples-per-chunk for every chunk from the stsc run table. */
	uint32_t *per_chunk = malloc((size_t)t->chunk_count * sizeof(uint32_t));
	if (!per_chunk) {
		free(samples);
		return -1;
	}
	for (int i = 0; i < t->chunk_count; i++)
		per_chunk[i] = 1;

	if (t->stsc_entries > 0) {
		int sc = 0;
		for (int c = 0; c < t->chunk_count; c++) {
			uint32_t chunk_no = (uint32_t)(c + 1);
			/* stsc entries are (first_chunk, samples_per_chunk) pairs, so
			 * run i occupies [2*i] and [2*i+1]. Move to the last run
			 * whose first_chunk is at or before this chunk. */
			while (sc + 1 < t->stsc_entries &&
			       t->stsc_first_chunk[(sc + 1) * 2] <= chunk_no)
				sc++;
			per_chunk[c] = t->stsc_first_chunk[sc * 2 + 1];
		}
	}

	int written = 0;
	uint32_t time_acc = 0;
	int stts_i = 0;
	uint32_t stts_left = t->stts_count[0];
	int sync_cursor = 0;      /* index into stss_index */

	for (int c = 0; c < t->chunk_count && written < total; c++) {
		uint32_t off = t->chunk_offset[c];
		uint32_t n = per_chunk[c];

		for (uint32_t k = 0; k < n && written < total; k++) {
			mp4_sample_t *s = &samples[written];

			uint32_t size;
			if (t->sample_size) {
				size = t->sample_size;
			} else if (t->stsz_size && written < t->stsz_entries) {
				size = t->stsz_size[written];
			} else {
				free(per_chunk);
				free(samples);
				return -1;
			}

			s->offset = off;
			s->size = size;
			s->time_offset = time_acc;
			off += size;

			/* duration from the current stts run. Entries are interleaved
			 * (count, delta) pairs in a single array, so entry i lives at
			 * [2*i] for the count and [2*i+1] for the delta. */
			while (stts_left == 0 && stts_i + 1 < t->stts_entries) {
				stts_i++;
				stts_left = t->stts_count[stts_i * 2];
			}
			s->duration = stts_left ? t->stts_count[stts_i * 2 + 1] : 0;
			if (stts_left)
				stts_left--;
			time_acc += s->duration;

			/* sync flag */
			s->is_sync = 1;
			if (t->stss_entries > 0) {
				uint32_t sample_no = (uint32_t)written + 1;
				s->is_sync = (sync_cursor < t->stss_entries &&
				              t->stss_index[sync_cursor] == sample_no);
				if (s->is_sync)
					sync_cursor++;
			}

			written++;
		}
	}

	free(per_chunk);
	*out = samples;
	*out_count = written;
	return 0;
}

long mp4_header_end(const char *path, long avail)
{
	if (avail < 16)
		return 0;
	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0)
		return 0;

	long result = 0;
	uint64_t pos = 0;
	for (;;) {
		char type[5];
		uint64_t size, hdr;
		if (!box_header(fd, pos, (uint64_t)avail, type, &size, &hdr))
			break;                       /* header not downloaded yet */
		if (!strcmp(type, "moov")) {
			if (pos + size <= (uint64_t)avail)
				result = (long)(pos + size);
			break;
		}
		if (!strcmp(type, "mdat")) {
			result = -1;                 /* sample data before the index */
			break;
		}
		pos += size;
	}
	sceIoClose(fd);
	return result;
}

int mp4_open(const char *path, mp4_track_t *track)
{
	memset(track, 0, sizeof(*track));

	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0) {
		log_printf("[mp4] cannot open %s (0x%08x)", path, fd);
		return -1;
	}

	SceOff file_size = sceIoLseek(fd, 0, SEEK_END);
	if (file_size <= 0) {
		sceIoClose(fd);
		return -1;
	}
	log_printf("[mp4] %s (%lld bytes)", path, (long long)file_size);

	/* Locate moov. */
	uint64_t pos = 0, moov_start = 0, moov_end = 0;
	bool found = false;
	while (pos < (uint64_t)file_size) {
		char type[5];
		uint64_t size, hdr;
		if (!box_header(fd, pos, (uint64_t)file_size, type, &size, &hdr))
			break;
		if (!strcmp(type, "moov")) {
			moov_start = pos + hdr;
			moov_end = pos + size;
			found = true;
			break;
		}
		pos += size;
	}
	if (!found) {
		log_printf("[mp4] no moov box");
		sceIoClose(fd);
		return -1;
	}

	bool got_video = false, got_audio = false;

	uint64_t p = moov_start;
	while (p < moov_end) {
		char type[5];
		uint64_t size, hdr;
		if (!box_header(fd, p, moov_end, type, &size, &hdr))
			break;
		if (size < hdr)
			break;

		if (!strcmp(type, "trak")) {
			/* mdia/hdlr: 8 bytes into hdlr is the handler type. */
			bool is_video = false, is_audio = false;
			uint64_t q = p + hdr, qend = p + size;
			while (q < qend) {
				char t2[5];
				uint64_t s2, h2;
				if (!box_header(fd, q, qend, t2, &s2, &h2))
					break;
				if (!strcmp(t2, "mdia")) {
					uint64_t r = q + h2;
					while (r < q + s2) {
						char t3[5];
						uint64_t s3, h3;
						if (!box_header(fd, r, q + s2, t3, &s3, &h3))
							break;
						if (!strcmp(t3, "hdlr")) {
							unsigned char hb[12];
							if (sceIoLseek(fd, (SceOff)(r + h3), SEEK_SET) >= 0 &&
							    sceIoRead(fd, hb, 12) == 12) {
								if (!memcmp(hb + 8, "vide", 4))
									is_video = true;
								else if (!memcmp(hb + 8, "soun", 4))
									is_audio = true;
							}
							break;
						}
						r += s3;
					}
				}
				q += s2;
			}

			if ((is_video && !got_video) || (is_audio && !got_audio)) {
				tables_t t;
				tables_init(&t);

				uint32_t timescale = 0, w = 0, h = 0;
				int ch = 0, rate = 0;
				avcc_info_t avcc;
				memset(&avcc, 0, sizeof(avcc));

				/* Locate moov/trak/mdia/minf/stbl: stbl sits under minf,
				 * two levels below mdia, so the search descends through
				 * minf rather than only checking direct children. */
				uint64_t a = p + hdr, aend = p + size;
				while (a < aend) {
					char t2[5];
					uint64_t s2, h2;
					if (!box_header(fd, a, aend, t2, &s2, &h2))
						break;
					if (!strcmp(t2, "mdia")) {
						uint64_t m = a + h2, mend = a + s2;
						while (m < mend) {
							char t3[5];
							uint64_t s3, h3;
							if (!box_header(fd, m, mend, t3, &s3, &h3))
								break;
							/* mdhd sits beside minf and carries the
							 * media timescale the sample tables use. */
							if (!strcmp(t3, "mdhd")) {
								unsigned char mb[24];
								if (sceIoLseek(fd, (SceOff)(m + h3), SEEK_SET) >= 0 &&
								    sceIoRead(fd, mb, 24) == 24) {
									timescale = (mb[0] == 1) ? rd32(mb + 20)
									                        : rd32(mb + 12);
								}
							}
							if (!strcmp(t3, "minf")) {
								uint64_t n = m + h3, nend = m + s3;
								while (n < nend) {
									char t4[5];
									uint64_t s4, h4;
									if (!box_header(fd, n, nend, t4, &s4, &h4))
										break;
									if (!strcmp(t4, "stbl")) {
										parse_stbl(fd, n + h4, n + s4, &t,
										           is_video ? &timescale : NULL,
										           is_video ? &w : NULL,
										           is_video ? &h : NULL,
										           is_audio ? &ch : NULL,
										           is_audio ? &rate : NULL,
										           is_video ? &avcc : NULL);
										break;
									}
									n += s4;
								}
								break;
							}
							m += s3;
						}
					}
					a += s2;
				}

				mp4_sample_t *samples = NULL;
				int count = 0;
				if (build_samples(&t, &samples, &count) == 0 && count > 0) {
					if (is_video) {
						track->video_samples = samples;
						track->video_count = count;
						track->video_capacity = count;
						track->video_timescale = timescale;
						track->video_width = w;
						track->video_height = h;
						memcpy(track->sps, avcc.sps, sizeof(track->sps));
						track->sps_size = avcc.sps_size;
						memcpy(track->pps, avcc.pps, sizeof(track->pps));
						track->pps_size = avcc.pps_size;
						track->nal_length_size = avcc.nal_length_size;
						track->profile_idc = avcc.profile_idc;
						track->profile_compat = avcc.profile_compat;
						track->level_idc = avcc.level_idc;
						track->duration = samples[count - 1].time_offset +
						                  samples[count - 1].duration;
						got_video = true;
					} else {
						track->audio_samples = samples;
						track->audio_count = count;
						track->audio_capacity = count;
						track->audio_timescale = timescale;
						track->audio_channels = ch;
						track->audio_sample_rate = rate;
						got_audio = true;
					}
				} else {
					free(samples);
				}
				tables_free(&t);
			}
		}
		p += size;
	}

	sceIoClose(fd);

	track->has_video = track->video_count > 0;
	track->has_audio = track->audio_count > 0;
	log_printf("[mp4] video=%d@%ux%u audio=%d @%dHz/%dch dur=%u",
	           track->video_count, track->video_width, track->video_height,
	           track->audio_count, track->audio_sample_rate,
	           track->audio_channels, track->duration);
	return (track->has_video || track->has_audio) ? 0 : -1;
}

void mp4_close(mp4_track_t *track)
{
	free(track->video_samples);
	free(track->audio_samples);
	memset(track, 0, sizeof(*track));
}

bool mp4_video_is_baseline(const mp4_track_t *track)
{
	if (!track || track->sps_size < 4)
		return false;
	if (track->profile_idc == H264_PROFILE_BASELINE)
		return true;
	if (track->profile_idc == H264_PROFILE_CONSTRAINED_BASELINE)
		return true;
	/* A constrained-baseline bit set also marks a stream as decodable
	 * without CABAC or B-frames even when profile_idc says Main. */
	if (track->profile_idc == H264_PROFILE_MAIN &&
	    (track->profile_compat & 0x40))
		return true;
	return false;
}

int mp4_read_at(SceUID *fd, const char *path, uint32_t offset,
                void *buf, int size)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		if (*fd < 0)
			*fd = sceIoOpen(path, PSP_O_RDONLY, 0);
		if (*fd < 0)
			return -1;
		int n = -1;
		if (sceIoLseek(*fd, (SceOff)offset, SEEK_SET) >= 0)
			n = sceIoRead(*fd, buf, size);
		if (n == size)
			return n;
		/* Stale view of a growing file: reopen to see the new length. */
		sceIoClose(*fd);
		*fd = -1;
		if (attempt == 1)
			return n;
	}
	return -1;
}

int mp4_read_sample(const char *path, const mp4_sample_t *s,
                    void *buf, int buf_size)
{
	if (!s || s->size == 0 || (int)s->size > buf_size)
		return 0;

	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	if (sceIoLseek(fd, (SceOff)s->offset, SEEK_SET) < 0) {
		sceIoClose(fd);
		return -1;
	}
	int n = sceIoRead(fd, buf, s->size);
	sceIoClose(fd);
	return n;
}
