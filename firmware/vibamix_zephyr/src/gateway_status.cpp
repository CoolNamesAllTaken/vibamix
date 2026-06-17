#include "gateway_status.h"

#include "Display_EPD_W21.h"
#include "GUI_Paint.h"
#include "fonts.h"

#include <stdio.h>

// ROTATE_270 drawing surface is EPD_HEIGHT wide x EPD_WIDTH tall.
static constexpr int kCanvasW = EPD_HEIGHT; // 264
static constexpr int kBannerH = 20;

static bool     s_active;
static int      s_cmd = -1;
static uint32_t s_count;
static bool     s_ka_alive;
static bool     s_ka_blink;

static const char *label_for(int cmd)
{
	switch (cmd) {
	case GW_CMD_NAME:     return "Name";
	case GW_CMD_FUNFACT:  return "Fun fact";
	case GW_CMD_LED:      return "LED color";
	case GW_CMD_IMAGE:    return "Image";
	case GW_CMD_SCREEN:   return "Text screen";
	case GW_CMD_DISPLAY:  return "Display screen";
	case GW_CMD_ATTENDEE: return "Table ID";
	case GW_CMD_FRAMELED: return "Frame LED";
	default:              return "";
	}
}

void gateway_status_set_active(bool on)
{
	s_active = on;
	if (!on) {
		s_cmd = -1;
		s_count = 0;
	}
}

bool gateway_status_active(void)              { return s_active; }
void gateway_status_note(int cmd)             { s_cmd = cmd; s_count++; }
uint32_t gateway_status_count(void)           { return s_count; }
const char *gateway_status_label(void)        { return label_for(s_cmd); }
void gateway_status_set_keepalive(bool a, bool b) { s_ka_alive = a; s_ka_blink = b; }

static void fill(int x0, int y0, int x1, int y1, uint16_t color)
{
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			Paint_SetPixel(x, y, color);
		}
	}
}

void gateway_status_overlay(uint8_t *fb)
{
	if (!s_active || s_count == 0) {
		return;
	}
	// Re-bind Paint to the (already-built) framebuffer without clearing it, then
	// draw a solid black banner with white text across the top.
	Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
	fill(0, 0, kCanvasW - 1, kBannerH - 1, BLACK);

	// Keepalive dot (white): filled when the app's keepalive is fresh + on-phase,
	// hollow otherwise.
	if (s_ka_alive && s_ka_blink) {
		fill(5, 5, 13, 13, WHITE);
	} else {
		fill(5, 5, 13, 5, WHITE);
		fill(5, 13, 13, 13, WHITE);
		fill(5, 5, 5, 13, WHITE);
		fill(13, 5, 13, 13, WHITE);
	}

	char buf[40];
	snprintf(buf, sizeof(buf), "Broadcast: %s  x%u", label_for(s_cmd), (unsigned)s_count);
	Paint_DrawString_P(20, 2, buf, &PoppinsMd16, BLACK, WHITE); // white text on black
}
