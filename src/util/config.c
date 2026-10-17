#include "config.h"
#include "log.h"
#include <pspiofilemgr.h>

#define CONFIG_FILE "ms0:/PSP/GAME/YTPSP/cfg.ini"

void config_defaults(app_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->host, CONFIG_MAX_HOST, "%s", CONFIG_DEFAULT_HOST);
	cfg->port = CONFIG_DEFAULT_PORT;
	cfg->pref_quality = 0;
	snprintf(cfg->kernel_module, sizeof(cfg->kernel_module), "%s",
	         CONFIG_DEFAULT_KERNEL_MODULE);
}

int config_load(app_config_t *cfg)
{
	config_defaults(cfg);

	SceUID fd = sceIoOpen(CONFIG_FILE, PSP_O_RDONLY, 0);
	if (fd < 0) {
		log_printf("[config] no config file, using defaults");
		return -1;
	}

	char buf[1024];
	int n = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;

	char *line = buf;
	while (line && *line) {
		char key[64], val[256];
		if (sscanf(line, "%63[^=]=%255[^\r\n]", key, val) == 2) {
			if (!strcmp(key, "host"))
				snprintf(cfg->host, sizeof(cfg->host), "%.127s", val);
			else if (!strcmp(key, "port"))
				cfg->port = atoi(val);
			else if (!strcmp(key, "last_query"))
				snprintf(cfg->last_query, sizeof(cfg->last_query), "%.127s", val);
			else if (!strcmp(key, "quality"))
				cfg->pref_quality = atoi(val);
			else if (!strcmp(key, "kernel_module"))
				snprintf(cfg->kernel_module,
				         sizeof(cfg->kernel_module), "%.127s", val);
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	log_printf("[config] loaded %s:%d", cfg->host, cfg->port);
	return 0;
}

int config_save(const app_config_t *cfg)
{
	sceIoMkdir("ms0:/PSP/GAME/YTPSP", 0777);

	char buf[1024];
	int n = snprintf(buf, sizeof(buf),
		"host=%s\nport=%d\nlast_query=%s\nquality=%d\nkernel_module=%s\n",
		cfg->host, cfg->port, cfg->last_query, cfg->pref_quality,
		cfg->kernel_module);

	SceUID fd = sceIoOpen(CONFIG_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0) {
		log_printf("[config] failed to open config for write: 0x%08x", fd);
		return -1;
	}
	sceIoWrite(fd, buf, n);
	sceIoClose(fd);
	log_printf("[config] saved");
	return 0;
}
