#ifndef YT_UTIL_CONFIG_H
#define YT_UTIL_CONFIG_H

#include "../common.h"

/*
 * Settings persisted in ms0:/PSP/SAVEDATA/YTPSPCFG.DAT
 *
 * The PSP SDK SSL stack (pspssl.h, 2008) cannot negotiate TLS 1.2+, SNI or
 * ECC certificates, which every modern Invidious deployment presents. The
 * client therefore speaks plain HTTP and the user is expected to terminate
 * their self-hosted Invidious on an HTTP listener (or behind a reverse proxy
 * that exposes one).
 */
#define CONFIG_DEFAULT_HOST  "invidious.local"
#define CONFIG_DEFAULT_PORT  80
#define CONFIG_MAX_HOST      128
#define CONFIG_MAX_URL       256

/* A decoder is not configured by default: the module is not shipped here. */
#define CONFIG_DEFAULT_KERNEL_MODULE ""

typedef struct {
	char host[CONFIG_MAX_HOST];
	int  port;
	char last_query[128];
	int  pref_quality;   /* index into the supported-format table */

	/*
	 * Path of the kernel-side media engine module on the memory stick, used
	 * for hardware H.264 decoding. Empty means no decoder: playback is
	 * reported unavailable while search, download and demux keep working.
	 * The module itself is a signed third-party binary and is not
	 * distributed with this source.
	 */
	char kernel_module[128];
} app_config_t;

void config_defaults(app_config_t *cfg);
int  config_load(app_config_t *cfg);
int  config_save(const app_config_t *cfg);

#endif
