#include "common.h"
#include "util/log.h"
#include "util/config.h"
#include "util/input.h"
#include "net/http.h"
#include "net/invidious.h"
#include "ui/gui.h"
#include "ui/keyboard.h"
#include "media/download.h"
#include "media/player.h"
#include "pspdebug.h"
#include "pspsdk.h"
#include "pspdisplay.h"
#include "pspgu.h"
#include "pspiofilemgr.h"

PSP_MODULE_INFO("YtLegacyPSP", 0, 1, 0);

PSP_MAIN_THREAD_PRIORITY(32);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);

/*
 * HOME-menu exit, as in the SDK samples: a callback thread that the system
 * signals when the user picks "quit" from the HOME menu.
 */
static int exit_callback(int arg1, int arg2, void *common)
{
	(void)arg1; (void)arg2; (void)common;
	sceKernelExitGame();
	return 0;
}

static int callback_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	sceKernelSleepThreadCB();
	return 0;
}

static void setup_exit_callback(void)
{
	int thid = sceKernelCreateThread("update_thread", callback_thread,
	                                 0x11, 0xFA0, 0, 0);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, 0);
}

static app_config_t  g_cfg;
static char          g_status[160];
static osk_context_t g_kb;

static void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
}

static void fmt_duration(char *out, int out_size, int seconds)
{
	if (seconds < 0)
		seconds = 0;
	int h = seconds / 3600;
	int m = (seconds / 60) % 60;
	int s = seconds % 60;
	if (h > 0)
		snprintf(out, out_size, "%d:%02d:%02d", h, m, s);
	else
		snprintf(out, out_size, "%d:%02d", m, s);
}

static void cache_path(char *out, int out_size, const char *video_id)
{
	snprintf(out, out_size, "ms0:/PSP/GAME/YTPSP/%s.mp4", video_id);
}

/* A one-off screen for blocking steps, so the display never sits on a stale
 * frame while the network works. */
static void show_message(const char *line1, const char *line2)
{
	gui_clear(COLOR_BG);
	gui_draw_text(12, 110, line1, COLOR_TEXT);
	if (line2)
		gui_draw_text(12, 126, line2, COLOR_DIM);
	gui_end_frame();
}

/*
 * Outcome of an action the user started: stays on screen until CIRCLE or
 * CROSS, and is kept as the list's status line afterwards.
 */
static void notify(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
	log_printf("[ui] %s", g_status);

	input_poll();                 /* drop the press that started the action */
	for (;;) {
		uint32_t hit = input_poll();
		gui_clear(COLOR_BG);
		gui_draw_text(12, 110, g_status, COLOR_TEXT);
		gui_draw_text(12, 140, "CIRCLE  OK", COLOR_DIM);
		gui_end_frame();
		if (hit & (BTN_CIRCLE | BTN_CROSS | BTN_START))
			break;
	}
}

typedef struct {
	const char  *title;
	const char  *quality;
	unsigned int start_us;
} dl_view_t;

/* The download behind the current playback, if any. */
static download_state_t g_dl = { .thread = -1 };

/* Stops the background download, if one is running, and waits for it. */
static void stop_download(void)
{
	if (g_dl.thread < 0)
		return;
	if (!g_dl.done)
		g_dl.cancel = true;          /* download_run deletes the partial file */
	download_join(&g_dl);
}

/* One frame of the download / buffering screen. */
static void draw_download(const download_state_t *st, const dl_view_t *view,
                          const char *heading)
{
	unsigned int now = sceKernelGetSystemTimeLow();
	double got_mb = st->received / 1048576.0;
	double secs = (now - view->start_us) / 1000000.0;
	int kbps = (secs > 0.0) ? (int)(st->received / 1024.0 / secs) : 0;

	gui_clear(COLOR_BG);
	gui_draw_text(12, 60, heading, COLOR_ACCENT);
	char title[64];
	utf8_copy_trunc(title, sizeof(title), view->title);
	gui_draw_text(12, 80, title, COLOR_TEXT);

	char line[96];
	const int bar_x = 12, bar_y = 110, bar_w = SCREEN_WIDTH - 24;
	gui_fill_rect(bar_x, bar_y, bar_w, 8, COLOR_PANEL);
	if (st->expected > 0) {
		int fill = (int)((double)st->received / st->expected * bar_w);
		if (fill > bar_w)
			fill = bar_w;
		gui_fill_rect(bar_x, bar_y, fill, 8, COLOR_ACCENT);
		snprintf(line, sizeof(line), "%s  %.1f / %.1f MB  %d%%  %d KB/s",
		         view->quality, got_mb, st->expected / 1048576.0,
		         (int)(st->received * 100 / st->expected), kbps);
	} else {
		snprintf(line, sizeof(line), "%s  %.1f MB  %d KB/s",
		         view->quality, got_mb, kbps);
	}
	gui_draw_text(12, 126, line, COLOR_TEXT);
	gui_draw_text(12, 150, "CROSS  cancel", COLOR_DIM);
	gui_end_frame();
}

static long file_size(const char *path)
{
	SceIoStat st;
	memset(&st, 0, sizeof(st));
	if (sceIoGetstat(path, &st) < 0)
		return -1;
	return (long)st.st_size;
}

int main(int argc, char **argv)
{

	log_init();
	log_printf("[boot] %s", APP_TITLE);
	setup_exit_callback();

	input_init();
	log_printf("[boot] input ready");

	gui_init();
	log_printf("[boot] gu ready");
	gui_start_frame();
	log_printf("[boot] frame started");
	gui_end_frame();
	log_printf("[boot] first frame presented");
	gui_start_frame();

	/* disc0: already points at ms0: on a retail PSP, so no sceIoAssign needed. */
	sceIoMkdir("ms0:/PSP/GAME", 0777);
	sceIoMkdir("ms0:/PSP/GAME/YTPSP", 0777);
	log_printf("[boot] ms0 ready");

	config_load(&g_cfg);
	log_printf("[boot] config host=%s port=%d", g_cfg.host, g_cfg.port);

	/*
	 * Hardware H.264 (see media/video.h). cooleyesBridge.prx sits next to
	 * the EBOOT. Early, so the decoder's 4 MB-aligned work area is taken
	 * before the heap fragments. Search and download work without it.
	 */
	char bridge[256];
	snprintf(bridge, sizeof(bridge), "%s", (argc > 0 && argv[0]) ? argv[0]
	         : "ms0:/PSP/GAME/YTPSP/EBOOT.PBP");
	char *slash = strrchr(bridge, '/');
	if (slash)
		snprintf(slash + 1, sizeof(bridge) - (slash + 1 - bridge),
		         "cooleyesBridge.prx");
	if (video_system_init(bridge) != 0)
		log_printf("[boot] playback disabled: no hardware decoder");

	bool net_ok = false;
	if (net_init() == 0) {
		if (net_wait_wifi(25) == 0) {
			net_ok = true;
			set_status("wifi ok -> %s:%d", g_cfg.host, g_cfg.port);
		} else {
			set_status("wifi unavailable");
		}
	} else {
		set_status("network init failed");
	}

	/* The detail struct is ~19 KB, far too large for the main thread stack,
	 * so it lives on the heap alongside the rest of the long-lived state. */
	inv_results_t *results = calloc(1, sizeof(*results));
	player_t      *player = calloc(1, sizeof(*player));
	inv_video_detail_t *detail = calloc(1, sizeof(*detail));
	if (!results || !player || !detail) {
		log_printf("[boot] out of memory");
		sceKernelExitGame();
		return 1;
	}
	int selected = 0;
	bool browse = false;

	if (net_ok) {
		int tr = inv_trending(g_cfg.host, g_cfg.port, results);
		log_printf("[boot] trending r=%d count=%d", tr, results->count);
		if (tr == 0 && results->count > 0) {
			browse = true;
			set_status("trending: %d videos", results->count);
		} else {
			set_status("press CIRCLE to search");
		}
	}

	bool have_player = false;
	unsigned frame = 0;
	unsigned hud_until = 0;
#if defined(PLAYER_AUDIO_TEST) && !defined(STREAM_TEST)
	/* Diagnostic builds: play a known file straight away. */
	if (player_open(player, PLAYER_AUDIO_TEST) == 0) {
		have_player = true;
		player_play(player);
	}
#endif
#ifdef STREAM_TEST
	/* Diagnostic builds: open one video id as if picked from the list. */
	bool stream_test_fired = false;
#endif
	log_printf("[loop] enter browse=%d", browse);

	for (;;) {
		uint32_t hit = input_poll();
#ifdef STREAM_TEST
		if (!stream_test_fired && net_ok) {
			stream_test_fired = true;
			results->count = 1;
			snprintf(results->items[0].id, sizeof(results->items[0].id),
			         "%s", STREAM_TEST);
			selected = 0;
			browse = true;
			hit |= BTN_CIRCLE;
		}
#endif
		/* Heartbeat: the last one in boot.log bounds when a crash happened. */
		if (++frame % 3600 == 0)
			log_printf("[loop] frame %u", frame);
		if (hit)
			log_printf("[loop] keys 0x%08x", hit);
		gui_clear(COLOR_BG);

		/* ---- playback ---- */
		if (have_player) {
			player_step(player);

			if (hit)
				hud_until = frame + 180;       /* show the HUD for 3 s */
			if (hit & BTN_CIRCLE) {
				if (player->state == PLAYER_ENDED) {
					player_seek(player, 0);
					player_play(player);
				} else {
					player_toggle_pause(player);
				}
			}
			if (hit & BTN_LEFT)
				player_seek(player,
				            (int)player_position_seconds(player) - 10);
			if (hit & BTN_RIGHT)
				player_seek(player,
				            (int)player_position_seconds(player) + 10);
			if (hit & BTN_CROSS) {
				log_printf("[player] closed: %d decoded, %d failed, %d late",
				           player->frames_decoded, player->frames_failed,
				           player->frames_late);
				player_close(player);
				stop_download();
				have_player = false;
				set_status("closed playback");
				gui_end_frame();
				continue;
			}
			if (hit & BTN_TRIANGLE) {
				gui_end_frame();
				break;
			}

			/* The picture, scaled to fit the screen with its aspect kept. */
			if (player->have_picture) {
				int w = player->width, h = player->height;
				int dw = SCREEN_WIDTH, dh = h * SCREEN_WIDTH / w;
				if (dh > SCREEN_HEIGHT) {
					dh = SCREEN_HEIGHT;
					dw = w * SCREEN_HEIGHT / h;
				}
				gui_fill_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0xFF000000u);
				gui_draw_rgba(player->decoder.rgb, player->decoder.pitch,
				              w, h, (SCREEN_WIDTH - dw) / 2,
				              (SCREEN_HEIGHT - dh) / 2, dw, dh);
			}

			bool show_hud = player->state != PLAYER_PLAYING ||
			                player->buffering || frame < hud_until;
			if (show_hud) {
				double pos = player_position_seconds(player);
				double dur = player_duration_seconds(player);
				int bar_w = SCREEN_WIDTH - 20;
				gui_fill_rect(0, SCREEN_HEIGHT - 26, SCREEN_WIDTH, 26,
				              COLOR_PANEL);
				if (dur > 0.0) {
					int fill = (int)((pos / dur) * bar_w);
					gui_fill_rect(10, SCREEN_HEIGHT - 8, bar_w, 4, COLOR_PANEL2);
					/* How much of the file is in, while it downloads. */
					int got = bar_w;
					if (player->dl && !g_dl.done && g_dl.expected > 0)
						got = (int)((double)g_dl.received / g_dl.expected *
						            bar_w);
					gui_fill_rect(10, SCREEN_HEIGHT - 8, got, 4, COLOR_DIM);
					gui_fill_rect(10, SCREEN_HEIGHT - 8, fill, 4,
					              COLOR_ACCENT);
				}
				const char *st =
					player->buffering              ? "BUFFER" :
					player->state == PLAYER_PAUSED ? "PAUSE" :
					player->state == PLAYER_ENDED  ? "END" : "PLAY";
				char hud[112];
				snprintf(hud, sizeof(hud),
				         "%s %d:%02d/%d:%02d  O pause  </> 10s  X back",
				         st, (int)pos / 60, (int)pos % 60,
				         (int)dur / 60, (int)dur % 60);
				gui_draw_text(8, SCREEN_HEIGHT - 22, hud, COLOR_TEXT);
			}

			gui_end_frame();
			continue;
		}

		/* ---- search prompt ---- */
		if (!browse) {
			gui_draw_text(12, 10, APP_TITLE, COLOR_ACCENT);
			gui_draw_text(12, 28, g_status, COLOR_TEXT);
			gui_draw_text(12, 48, "CIRCLE   search", COLOR_DIM);
			gui_draw_text(12, 62, "SELECT   configure server", COLOR_DIM);
			if (results->count > 0)
				gui_draw_text(12, 76, "CROSS    back to results", COLOR_DIM);
			gui_draw_text(12, 90, "TRIANGLE quit", COLOR_DIM);

			/* CIRCLE confirms, as on Japanese PSP software; START too. */
			if (results->count > 0 && (hit & BTN_CROSS)) {
				browse = true;
				gui_end_frame();
				continue;
			}

			if (hit & (BTN_CIRCLE | BTN_START)) {
				char query[128];
				if (osk_prompt(&g_kb, "Search YouTube", g_cfg.last_query,
				               query, sizeof(query))) {
					set_status("searching '%s'...", query);
					gui_end_frame();
					if (inv_search(g_cfg.host, g_cfg.port, query,
					               results) == 0 && results->count > 0) {
						selected = 0;
						browse = true;
						set_status("%d results for '%s'", results->count,
						           query);
						snprintf(g_cfg.last_query,
						         sizeof(g_cfg.last_query), "%s", query);
						config_save(&g_cfg);
					} else {
						set_status("no results for '%s'", query);
					}
				}
			}

			if (hit & BTN_SELECT) {
				char host[128];
				snprintf(host, sizeof(host), "%s", g_cfg.host);
				if (osk_prompt(&g_kb, "Invidious host (ip or name)", host,
				               host, sizeof(host))) {
					snprintf(g_cfg.host, sizeof(g_cfg.host), "%s", host);
					config_save(&g_cfg);
					set_status("server set to %s", g_cfg.host);
				}
			}

			if (hit & BTN_TRIANGLE) {
				gui_end_frame();
				break;
			}
			gui_end_frame();
			continue;
		}

		/* ---- results list ---- */
		gui_fill_rect(0, 0, SCREEN_WIDTH, 16, COLOR_PANEL);
		gui_draw_text(6, 4, g_status, COLOR_ACCENT);

		const int visible = 9;
		int first = selected - visible / 2;
		if (first < 0) first = 0;
		if (first > results->count - visible) first = results->count - visible;
		if (first < 0) first = 0;

		for (int i = 0; i < visible && first + i < results->count; i++) {
			int idx = first + i;
			inv_video_t *v = &results->items[idx];
			int y = 20 + i * 24;

			if (idx == selected)
				gui_fill_rect(2, y - 3, SCREEN_WIDTH - 4, 23,
				              COLOR_HIGHLIGHT);

			char line1[96];
			utf8_copy_trunc(line1, sizeof(line1), v->title);
			gui_draw_text(8, y, line1,
			              (idx == selected) ? COLOR_TEXT : COLOR_DIM);

			char dur[16];
			fmt_duration(dur, sizeof(dur), v->length_seconds);
			char line2[96];
			snprintf(line2, sizeof(line2), "%s  %s", v->author, dur);
			utf8_copy_trunc(line2, sizeof(line2), line2);
			gui_draw_text(8, y + 11, line2, COLOR_DIM);
		}

		/* Button guide, so the controls never have to be guessed. */
		gui_fill_rect(0, SCREEN_HEIGHT - 14, SCREEN_WIDTH, 14, COLOR_PANEL);
		gui_draw_text(6, SCREEN_HEIGHT - 11,
		              "CIRCLE open   CROSS search   TRIANGLE quit",
		              COLOR_DIM);

		if (hit & BTN_UP)
			selected = (selected > 0) ? selected - 1 : 0;
		if (hit & BTN_DOWN)
			selected = (selected + 1 < results->count)
			         ? selected + 1 : results->count - 1;

		/* ---- download + play ---- */
		if (!have_player && (hit & (BTN_CIRCLE | BTN_START))) {
			inv_video_t *v = &results->items[selected];
			log_printf("[ui] open %s", v->id);
			show_message("Fetching stream info...", v->id);

			if (inv_video_detail(g_cfg.host, g_cfg.port, v->id,
			                     detail) != 0) {
				notify("could not fetch video details");
			} else {
				int fi = inv_pick_format(detail);
				log_printf("[ui] picked format %d of %d", fi,
				           detail->format_count);
				if (fi < 0) {
					notify("no PSP-playable format (live or no 360p mp4)");
				} else {
					char path[160];
					cache_path(path, sizeof(path), v->id);

					inv_format_t *f = &detail->formats[fi];
					log_printf("[dl] itag=%s %s %s %.1fMB",
					           f->itag, f->quality_label, f->container,
					           f->content_length / 1048576.0);

					/*
					 * A complete earlier download plays straight away.
					 * Otherwise the file downloads in the background and
					 * playback starts as soon as its index (moov) is in;
					 * the player buffers if it catches up with the download.
					 */
					long have = file_size(path);
					if (f->content_length > 0 && have == f->content_length) {
						log_printf("[dl] cached %s (%ld bytes)", path, have);
						if (player_open(player, path) == 0) {
							have_player = true;
							hud_until = frame + 180;
							player_play(player);
						} else {
							notify("cannot play: %s", player->message);
						}
					} else if (download_start(&g_dl, f->url, path,
					                          f->content_length) != 0) {
						notify("download failed to start");
					} else {
						dl_view_t view = {
							.title = detail->title,
							.quality = f->quality_label,
							.start_us = sceKernelGetSystemTimeLow(),
						};
						bool streamable = true;
						long header_end = 0;
						unsigned wait = 0;
						input_poll();          /* drop the opening press */
						for (;;) {
							if (input_poll() & BTN_CROSS) {
								stop_download();
								notify("download cancelled");
								break;
							}
							/* The index check opens the file: twice a second
							 * is plenty. */
							if (streamable && header_end <= 0 &&
							    wait++ % 30 == 0) {
								header_end = mp4_header_end(path, g_dl.received);
								if (header_end < 0) {
									streamable = false;
									log_printf("[dl] index after data: "
									           "playing once complete");
								}
							}
							bool ready = g_dl.done ||
							    (streamable && header_end > 0 &&
							     g_dl.received >= header_end + 64 * 1024);
							if (ready) {
								unsigned secs = (sceKernelGetSystemTimeLow() -
								                 view.start_us) / 1000000;
								log_printf("[dl] playable after %us, %ld bytes "
								           "(index %ld)", secs, g_dl.received,
								           header_end);
								if (g_dl.done && g_dl.result != 0) {
									stop_download();
									notify("download failed");
									break;
								}
								show_message("Opening...", view.title);
								if (player_open(player, path) == 0) {
									if (!g_dl.done)
										player_attach_download(player, &g_dl);
									have_player = true;
									hud_until = frame + 180;
									player_play(player);
								} else {
									stop_download();
									notify("cannot play: %s", player->message);
								}
								break;
							}
							draw_download(&g_dl, &view, streamable
							              ? "Buffering..." : "Downloading");
						}
					}
				}
			}
		}

		if (hit & (BTN_CROSS | BTN_SELECT)) {
			browse = false;
			set_status("press CIRCLE to search");
		}
		if (hit & BTN_TRIANGLE) {
			gui_end_frame();
			break;
		}

		gui_end_frame();
	}

	log_printf("[boot] quit");
	player_close(player);
	stop_download();
	net_term();
	/* Returning from main leaves the module resident with a dead screen. */
	sceKernelExitGame();
	return 0;
}
