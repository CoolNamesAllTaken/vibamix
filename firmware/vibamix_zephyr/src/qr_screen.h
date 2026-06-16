#ifndef VIBAMIX_QR_SCREEN_H
#define VIBAMIX_QR_SCREEN_H

#include "GUI.h"

/*
 * Renders the config screen into the GUI framebuffer (does NOT push to the panel
 * — the caller chooses a full refresh / base map for the first draw and a partial
 * refresh for each countdown tick). Left: QR encoding `url`. Right: the human
 * `code`, a "scan to set up" prompt, the battery (voltage + percent + icon), and
 * a countdown of seconds remaining in config mode.
 *
 * batt_mv < 0 renders the battery as unknown ("--").
 */
void qr_screen_draw(GUI &gui, const char *code, const char *url,
                    int batt_mv, int batt_pct, int remaining_sec);

#endif /* VIBAMIX_QR_SCREEN_H */
