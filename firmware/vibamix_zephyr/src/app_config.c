#include "app_config.h"

#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define CFG_SUBTREE "vibamix"

static struct app_config s_cfg;

static int cfg_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	ssize_t rc;

	if (settings_name_steq(key, "name", &next) && !next) {
		rc = read_cb(cb_arg, s_cfg.name, sizeof(s_cfg.name) - 1);
		if (rc > 0) {
			s_cfg.name[rc] = '\0';
			s_cfg.has_name = true;
		}
		return 0;
	}
	if (settings_name_steq(key, "fact", &next) && !next) {
		rc = read_cb(cb_arg, s_cfg.fun_fact, sizeof(s_cfg.fun_fact) - 1);
		if (rc > 0) {
			s_cfg.fun_fact[rc] = '\0';
		}
		return 0;
	}
	if (settings_name_steq(key, "color", &next) && !next) {
		uint8_t rgb[3];

		rc = read_cb(cb_arg, rgb, sizeof(rgb));
		if (rc == sizeof(rgb)) {
			s_cfg.r = rgb[0];
			s_cfg.g = rgb[1];
			s_cfg.b = rgb[2];
			s_cfg.has_color = true;
		}
		return 0;
	}
	if (settings_name_steq(key, "img", &next) && !next) {
		uint8_t flag = 0;

		rc = read_cb(cb_arg, &flag, sizeof(flag));
		if (rc == sizeof(flag)) {
			s_cfg.has_custom_image = (flag != 0);
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(vibamix_cfg, CFG_SUBTREE, NULL, cfg_set, NULL, NULL);

int app_config_init(void)
{
	return settings_subsys_init();
}

const struct app_config *app_config_get(void)
{
	return &s_cfg;
}

void app_config_set_name(const char *s, size_t len)
{
	if (len >= sizeof(s_cfg.name)) {
		len = sizeof(s_cfg.name) - 1;
	}
	memcpy(s_cfg.name, s, len);
	s_cfg.name[len] = '\0';
	s_cfg.has_name = true;
	settings_save_one(CFG_SUBTREE "/name", s_cfg.name, len);
	printk("cfg: name=\"%s\"\n", s_cfg.name);
	/* The name/fact identity screen replaces any custom image. */
	app_config_set_has_image(false);
}

void app_config_set_fun_fact(const char *s, size_t len)
{
	if (len >= sizeof(s_cfg.fun_fact)) {
		len = sizeof(s_cfg.fun_fact) - 1;
	}
	memcpy(s_cfg.fun_fact, s, len);
	s_cfg.fun_fact[len] = '\0';
	settings_save_one(CFG_SUBTREE "/fact", s_cfg.fun_fact, len);
	printk("cfg: fun_fact=\"%s\"\n", s_cfg.fun_fact);
	app_config_set_has_image(false);
}

void app_config_set_color(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t rgb[3] = { r, g, b };

	s_cfg.r = r;
	s_cfg.g = g;
	s_cfg.b = b;
	s_cfg.has_color = true;
	settings_save_one(CFG_SUBTREE "/color", rgb, sizeof(rgb));
	printk("cfg: color=#%02x%02x%02x\n", r, g, b);
}

void app_config_set_has_image(bool has_image)
{
	uint8_t flag = has_image ? 1 : 0;

	if (s_cfg.has_custom_image == has_image) {
		return;
	}
	s_cfg.has_custom_image = has_image;
	settings_save_one(CFG_SUBTREE "/img", &flag, sizeof(flag));
	printk("cfg: has_custom_image=%d\n", flag);
}
