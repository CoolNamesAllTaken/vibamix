#include "app_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
	if (settings_name_steq(key, "aid", &next) && !next) {
		rc = read_cb(cb_arg, s_cfg.attendee_id, sizeof(s_cfg.attendee_id) - 1);
		if (rc > 0) {
			s_cfg.attendee_id[rc] = '\0';
			s_cfg.has_attendee = true;
		}
		return 0;
	}
	if (settings_name_steq(key, "disp", &next) && !next) {
		uint8_t d[2];

		rc = read_cb(cb_arg, d, sizeof(d));
		if (rc == sizeof(d)) {
			s_cfg.disp_kind = d[0];
			s_cfg.disp_idx = d[1];
			s_cfg.has_disp = true;
		}
		return 0;
	}
	if (settings_name_steq(key, "iled", &next) && next) {
		/* next = "<slot>" — per-image-frame LED config (4 bytes). */
		char *end;
		long idx = strtol(next, &end, 10);

		if (idx < 0 || idx >= APP_CFG_IMAGE_SLOTS || *end != '\0') {
			return -ENOENT;
		}
		uint8_t v[4];

		rc = read_cb(cb_arg, v, sizeof(v));
		if (rc == sizeof(v)) {
			s_cfg.image_led[idx] = (struct frame_led){ v[0], v[1], v[2], v[3] };
		}
		return 0;
	}
	if (settings_name_steq(key, "scr", &next) && next) {
		/* next = "<idx>/<field>"; field is 'h' (header), 'b' (body) or 'l' (LED). */
		char *end;
		long idx = strtol(next, &end, 10);

		if (idx < 0 || idx >= APP_CFG_SCREEN_COUNT || *end != '/') {
			return -ENOENT;
		}
		struct badge_screen *scr = &s_cfg.screens[idx];
		const char *field = end + 1;

		if (strcmp(field, "h") == 0) {
			rc = read_cb(cb_arg, scr->header, sizeof(scr->header) - 1);
			if (rc >= 0) {
				scr->header[rc] = '\0';
				scr->present = true;
			}
			return 0;
		}
		if (strcmp(field, "b") == 0) {
			rc = read_cb(cb_arg, scr->body, sizeof(scr->body) - 1);
			if (rc >= 0) {
				scr->body[rc] = '\0';
				scr->present = true;
			}
			return 0;
		}
		if (strcmp(field, "l") == 0) {
			uint8_t v[4];

			rc = read_cb(cb_arg, v, sizeof(v));
			if (rc == sizeof(v)) {
				scr->led = (struct frame_led){ v[0], v[1], v[2], v[3] };
			}
			return 0;
		}
		return -ENOENT;
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

void app_config_set_attendee_id(const char *s, size_t len)
{
	if (len >= sizeof(s_cfg.attendee_id)) {
		len = sizeof(s_cfg.attendee_id) - 1;
	}
	memcpy(s_cfg.attendee_id, s, len);
	s_cfg.attendee_id[len] = '\0';
	s_cfg.has_attendee = (len > 0);
	settings_save_one(CFG_SUBTREE "/aid", s_cfg.attendee_id, len);
	printk("cfg: attendee_id=\"%s\"\n", s_cfg.attendee_id);
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

void app_config_set_screen(uint8_t idx, const char *hdr, size_t hlen,
			   const char *body, size_t blen)
{
	if (idx >= APP_CFG_SCREEN_COUNT) {
		return;
	}
	struct badge_screen *scr = &s_cfg.screens[idx];
	char key[24];

	if (hlen >= sizeof(scr->header)) {
		hlen = sizeof(scr->header) - 1;
	}
	memcpy(scr->header, hdr, hlen);
	scr->header[hlen] = '\0';

	if (blen >= sizeof(scr->body)) {
		blen = sizeof(scr->body) - 1;
	}
	memcpy(scr->body, body, blen);
	scr->body[blen] = '\0';
	scr->present = true;

	snprintf(key, sizeof(key), CFG_SUBTREE "/scr/%u/h", idx);
	settings_save_one(key, scr->header, hlen);
	snprintf(key, sizeof(key), CFG_SUBTREE "/scr/%u/b", idx);
	settings_save_one(key, scr->body, blen);
	printk("cfg: screen %u set (hdr \"%s\", %u body bytes)\n", idx, scr->header,
	       (unsigned)blen);
}

const struct badge_screen *app_config_get_screen(uint8_t idx)
{
	if (idx >= APP_CFG_SCREEN_COUNT || !s_cfg.screens[idx].present) {
		return NULL;
	}
	return &s_cfg.screens[idx];
}

void app_config_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
			      uint8_t r, uint8_t g, uint8_t b)
{
	struct frame_led *led;
	char key[24];

	if (kind == APP_DISP_KIND_TEXT && idx < APP_CFG_SCREEN_COUNT) {
		led = &s_cfg.screens[idx].led;
		snprintf(key, sizeof(key), CFG_SUBTREE "/scr/%u/l", idx);
	} else if (kind == APP_DISP_KIND_IMAGE && idx < APP_CFG_IMAGE_SLOTS) {
		led = &s_cfg.image_led[idx];
		snprintf(key, sizeof(key), CFG_SUBTREE "/iled/%u", idx);
	} else {
		return;
	}

	*led = (struct frame_led){ anim, r, g, b };
	uint8_t v[4] = { anim, r, g, b };

	settings_save_one(key, v, sizeof(v));
	printk("cfg: frame led kind=%u idx=%u anim=%u #%02x%02x%02x\n",
	       kind, idx, anim, r, g, b);
}

bool app_config_get_frame_led(uint8_t kind, uint8_t idx, struct frame_led *out)
{
	if (kind == APP_DISP_KIND_TEXT && idx < APP_CFG_SCREEN_COUNT) {
		*out = s_cfg.screens[idx].led;
		return true;
	}
	if (kind == APP_DISP_KIND_IMAGE && idx < APP_CFG_IMAGE_SLOTS) {
		*out = s_cfg.image_led[idx];
		return true;
	}
	return false;
}

void app_config_set_display(uint8_t kind, uint8_t idx)
{
	uint8_t d[2] = { kind, idx };

	s_cfg.disp_kind = kind;
	s_cfg.disp_idx = idx;
	s_cfg.has_disp = true;
	settings_save_one(CFG_SUBTREE "/disp", d, sizeof(d));
	printk("cfg: display kind=%u idx=%u\n", kind, idx);
}
