#ifndef VIBAMIX_QR_SCREEN_H
#define VIBAMIX_QR_SCREEN_H

#include "GUI.h"

/*
 * Renders the config screen into the GUI framebuffer (does NOT push to the panel
 * — the caller chooses a full refresh / base map for the first draw and a partial
 * refresh for each countdown tick).
 *
 * Layout: a top status bar (small battery icon + percent on the left, M:SS
 * countdown on the right) over a progress bar that shrinks from full toward empty
 * as `remaining_sec` falls to 0 of `total_sec`; below a divider, the QR encoding
 * `url` on the left and the heading / prompt / big `code` on the right.
 *
 * batt_mv < 0 renders the battery as unknown ("--").
 */
void qr_screen_draw(GUI &gui, const char *code, const char *url,
                    int batt_mv, int batt_pct, int remaining_sec, int total_sec);

/* "Connected" screen shown while a phone is connected over the config GATT
 * service: status bar (battery + "Connected" + a keepalive dot) and the attendee
 * name / table. The dot pulses solid/hollow each call while `app_alive` (the
 * laptop's keepalive writes are still arriving); it stays hollow when stale. */
void config_screen_connected(GUI &gui, const char *name, const char *table,
                             int batt_mv, int batt_pct, bool app_alive, bool blink);

/* Identity screen (name, "Table <id>", fun fact). Clean variant has no status
 * bar (the badge's resting screen); the status variant adds the battery + a
 * shrinking countdown bar (shown after a config disconnect, until sleep). */
void identity_screen_draw(GUI &gui, const char *name, const char *table,
                          const char *fun_fact);
void identity_status_screen_draw(GUI &gui, const char *name, const char *table,
                                 const char *fun_fact, int batt_mv, int batt_pct,
                                 int remaining_sec, int total_sec);

#endif /* VIBAMIX_QR_SCREEN_H */
