#include "invidious.h"
#include "http.h"
#include "../util/log.h"

#define REQ_BUF 1024

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void copy_str(char *dst, int dst_size, const char *src)
{
	if (!src) {
		dst[0] = 0;
		return;
	}
	snprintf(dst, dst_size, "%s", src);

	/* Never leave a multi-byte UTF-8 sequence cut in half at the end. */
	int len = (int)strlen(dst);
	int lead = len;
	while (lead > 0 && ((unsigned char)dst[lead - 1] & 0xC0) == 0x80)
		lead--;
	if (lead > 0 && ((unsigned char)dst[lead - 1] & 0x80)) {
		unsigned char c = (unsigned char)dst[lead - 1];
		int need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
		           (c & 0xF8) == 0xF0 ? 4 : 1;
		if (len - (lead - 1) < need)
			dst[lead - 1] = 0;
	}
}

/* Invidious reports thumbnail URLs as instance-relative paths. */
static void pick_thumbnail(const json_value_t *thumbs, char *dst, int dst_size)
{
	dst[0] = 0;
	if (!thumbs || thumbs->type != JSON_ARRAY)
		return;

	static const char *preferred[] = { "hqdefault", "sddefault", "default" };
	for (unsigned p = 0; p < sizeof(preferred) / sizeof(preferred[0]); p++) {
		for (const json_value_t *t = thumbs->first; t; t = t->next) {
			const char *q = json_str(json_get(t, "quality"), "");
			if (!strcmp(q, preferred[p])) {
				const char *url = json_str(json_get(t, "url"), "");
				if (url && *url) {
					copy_str(dst, dst_size, url);
					return;
				}
			}
		}
	}
	/* Fall back to whatever is present. */
	const char *url = json_str(json_get(json_at(thumbs, 0), "url"), "");
	if (url)
		copy_str(dst, dst_size, url);
}

/* Itags the PSP can actually play, best first. */
static int itag_rank(const char *itag)
{
	if (!strcmp(itag, "18"))  return 0;   /* 360p progressive H.264+AAC MP4 */
	if (!strcmp(itag, "22"))  return 1;   /* 360p progressive, legacy          */
	if (!strcmp(itag, "36"))  return 2;   /* 240p progressive                  */
	if (!strcmp(itag, "5"))   return 3;   /* 240p progressive, legacy           */
	if (!strcmp(itag, "34"))  return 4;   /* 360p video-only adaptive          */
	if (!strcmp(itag, "133")) return 5;   /* 240p video-only adaptive          */
	if (!strcmp(itag, "160")) return 6;   /* 144p video-only adaptive          */
	return -1;
}

/* A progressive stream declares both audio and video in its type string. */
static void classify_type(const char *type, bool *has_video, bool *has_audio)
{
	*has_video = *has_audio = false;
	if (!type)
		return;
	if (strstr(type, "video/"))
		*has_video = true;
	if (strstr(type, "audio/"))
		*has_audio = true;
	/* A muxed stream is "video/mp4; codecs=\"avc1..., mp4a...\"". */
	if (strstr(type, "mp4a"))
		*has_audio = true;
	if (!*has_video && !*has_audio) {
		*has_video = true;   /* assume muxed when undetermined */
	}
}

static void parse_bitrate_kbps(const json_value_t *bitrate, int *vbr, int *abr)
{
	int total = json_int(bitrate, 0);
	if (total <= 0)
		return;

	/* adaptiveFormats reports one aggregate bitrate; split it with the
	 * common 1:5 audio-to-video ratio so callers get usable numbers. */
	*abr = total / 6;
	*vbr = total - *abr;
}

static void fill_format(const json_value_t *f, inv_format_t *out)
{
	memset(out, 0, sizeof(*out));
	out->content_length = -1;

	const char *itag = json_str(json_get(f, "itag"), "");
	copy_str(out->itag, sizeof(out->itag), itag);
	copy_str(out->container, sizeof(out->container), json_str(json_get(f, "container"), ""));
	copy_str(out->encoding, sizeof(out->encoding), json_str(json_get(f, "encoding"), ""));
	copy_str(out->quality_label, sizeof(out->quality_label),
	         json_str(json_get(f, "qualityLabel"), ""));
	copy_str(out->type, sizeof(out->type), json_str(json_get(f, "type"), ""));
	copy_str(out->url, sizeof(out->url), json_str(json_get(f, "url"), ""));

	out->width = json_int(json_get(f, "width"), 0);
	out->height = json_int(json_get(f, "height"), 0);
	out->fps = json_int(json_get(f, "fps"), 0);
	out->audio_channels = json_int(json_get(f, "audioChannels"), 0);
	out->audio_sample_rate = json_int(json_get(f, "audioSampleRate"), 0);

	classify_type(out->type, &out->has_video, &out->has_audio);

	/*
	 * Adaptive formats carry "clen"; muxed ones do not, and their "size" is
	 * the frame size ("640x360"), not bytes. The signed URL has clen= too.
	 */
	const char *clen = json_str(json_get(f, "clen"), "");
	if (clen && *clen) {
		out->content_length = atol(clen);
	} else {
		const char *q = strstr(out->url, "&clen=");
		if (q)
			out->content_length = atol(q + 6);
	}

	parse_bitrate_kbps(json_get(f, "bitrate"), &out->video_bitrate,
	                   &out->audio_bitrate);

	const char *aq = json_str(json_get(f, "audioQuality"), "");
	if (aq && *aq)
		out->audio_bitrate = out->audio_bitrate ? out->audio_bitrate : 64;
}

/* ------------------------------------------------------------------ */
/* video list parsing (search + trending share the shape)              */
/* ------------------------------------------------------------------ */

static int parse_video_list(const json_doc_t *doc, inv_results_t *out)
{
	json_value_t *arr = json_root(doc);
	if (!arr || arr->type != JSON_ARRAY)
		return -1;

	out->count = 0;
	for (const json_value_t *v = arr->first; v && out->count < INV_MAX_RESULTS; v = v->next) {
		const char *type = json_str(json_get(v, "type"), "video");
		if (strcmp(type, "video") != 0)
			continue;

		inv_video_t *item = &out->items[out->count];
		memset(item, 0, sizeof(*item));

		const char *id = json_str(json_get(v, "videoId"), "");
		if (!id || !*id)
			continue;

		copy_str(item->id, sizeof(item->id), id);
		copy_str(item->title, sizeof(item->title), json_str(json_get(v, "title"), ""));
		copy_str(item->author, sizeof(item->author), json_str(json_get(v, "author"), ""));
		item->length_seconds = json_int(json_get(v, "lengthSeconds"), 0);
		item->view_count = (long)json_num(json_get(v, "viewCount"), 0);
		pick_thumbnail(json_get(v, "videoThumbnails"), item->thumbnail,
		               sizeof(item->thumbnail));

		out->count++;
	}
	return 0;
}

int inv_search(const char *host, int port, const char *query, inv_results_t *out)
{
	char enc[512];
	url_encode(query, enc, sizeof(enc));

	char path[REQ_BUF];
	snprintf(path, sizeof(path), "/api/v1/search?q=%s&type=video", enc);

	char *body = NULL;
	int len = 0, status = 0;
	if (http_get(host, port, path, &body, &len, &status) < 0) {
		log_printf("[inv] search failed (status %d)", status);
		return -1;
	}

	json_doc_t *doc = json_parse(body, len);
	if (!doc) {
		log_printf("[inv] search: malformed JSON (%d bytes)", len);
		free(body);
		return -1;
	}

	int r = parse_video_list(doc, out);
	json_doc_free(doc);
	return r;
}

int inv_trending(const char *host, int port, inv_results_t *out)
{
	const char *path = "/api/v1/trending";

	char *body = NULL;
	int len = 0, status = 0;
	if (http_get(host, port, path, &body, &len, &status) < 0) {
		log_printf("[inv] trending failed (status %d)", status);
		return -1;
	}

	json_doc_t *doc = json_parse(body, len);
	if (!doc) {
		free(body);
		return -1;
	}

	int r = parse_video_list(doc, out);
	json_doc_free(doc);
	return r;
}

int inv_video_detail(const char *host, int port, const char *video_id,
                     inv_video_detail_t *out)
{
	char path[REQ_BUF];
	/*
	 * local=true makes Invidious rewrite stream URLs onto itself as plain
	 * HTTP /videoplayback paths; the PSP cannot reach googlevideo over TLS.
	 */
	snprintf(path, sizeof(path), "/api/v1/videos/%s?local=true", video_id);

	memset(out, 0, sizeof(*out));
	out->format_count = 0;
	copy_str(out->video_id, sizeof(out->video_id), video_id);

	char *body = NULL;
	int len = 0, status = 0;
	if (http_get(host, port, path, &body, &len, &status) < 0) {
		log_printf("[inv] detail failed (status %d)", status);
		return -1;
	}

	json_doc_t *doc = json_parse(body, len);
	if (!doc) {
		log_printf("[inv] detail: malformed JSON (%d bytes)", len);
		free(body);
		return -1;
	}

	json_value_t *root = json_root(doc);
	copy_str(out->title, sizeof(out->title), json_str(json_get(root, "title"), ""));
	copy_str(out->author, sizeof(out->author), json_str(json_get(root, "author"), ""));
	out->length_seconds = json_int(json_get(root, "lengthSeconds"), 0);
	copy_str(out->description, sizeof(out->description),
	         json_str(json_get(root, "description"), ""));

	/* Progressive muxed streams are the primary source. */
	json_value_t *streams = json_get(root, "formatStreams");
	if (streams) {
		for (const json_value_t *f = streams->first; f; f = f->next) {
			if (out->format_count >= INV_MAX_FORMATS)
				break;
			fill_format(f, &out->formats[out->format_count]);
			if (out->formats[out->format_count].url[0])
				out->format_count++;
		}
	}

	/* Then adaptive streams, so a manual fallback still exists. */
	json_value_t *adaptive = json_get(root, "adaptiveFormats");
	if (adaptive) {
		for (const json_value_t *f = adaptive->first; f; f = f->next) {
			if (out->format_count >= INV_MAX_FORMATS)
				break;
			inv_format_t fmt;
			fill_format(f, &fmt);
			if (!fmt.url[0] || !fmt.encoding[0])
				continue;
			/* Skip encodings the PSP cannot handle. */
			if (strstr(fmt.type, "video/") && strcmp(fmt.encoding, "h264") != 0)
				continue;
			if (strstr(fmt.container, "webm"))
				continue;
			bool dup = false;
			for (int i = 0; i < out->format_count; i++) {
				if (!strcmp(out->formats[i].itag, fmt.itag)) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
			out->formats[out->format_count++] = fmt;
		}
	}

	json_doc_free(doc);

	/* local=true URLs are host-relative; anchor them on the Invidious host. */
	for (int i = 0; i < out->format_count; i++) {
		inv_format_t *f = &out->formats[i];
		if (f->url[0] != '/')
			continue;
		char rel[INV_STREAM_URL_LEN];
		copy_str(rel, sizeof(rel), f->url);
		int n = snprintf(f->url, sizeof(f->url), "http://%s:%d%s",
		                 host, port, rel);
		if (n >= (int)sizeof(f->url)) {
			log_printf("[inv] itag %s url too long (%d)", f->itag, n);
			f->url[0] = 0;
		}
	}

	log_printf("[inv] detail %s: %d formats", out->video_id, out->format_count);
	return 0;
}

int inv_pick_format(const inv_video_detail_t *detail)
{
	int best = -1;
	int best_rank = 1 << 30;

	for (int i = 0; i < detail->format_count; i++) {
		const inv_format_t *f = &detail->formats[i];
		if (!f->url[0])
			continue;
		if (strcmp(f->container, "mp4") != 0)
			continue;
		if (f->has_video && strcmp(f->encoding, "h264") != 0)
			continue;
		/*
		 * Adaptive streams are fragmented MP4 (sidx/moof/mdat) carrying one
		 * track each, which the demuxer cannot read. Only muxed ones qualify.
		 */
		if (!f->has_video || !f->has_audio)
			continue;

		int rank = itag_rank(f->itag);
		if (rank < 0)
			rank = 50 + f->height;   /* unknown itag: prefer low resolution */

		if (rank < best_rank) {
			best_rank = rank;
			best = i;
		}
	}
	return best;
}
