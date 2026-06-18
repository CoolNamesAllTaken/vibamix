#ifndef VIBAMIX_QR_SCREEN_H
#define VIBAMIX_QR_SCREEN_H

#include "GUI.h"

#include <stdint.h>

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

/* Identity frame (the badge's resting home screen): an optional identity image
 * (2-bit grayscale, authored on the 264x176 landscape canvas) filling the main
 * area, over a thin bottom banner holding the attendee name (left) + attendee ID
 * (right). Pass img=NULL for no image (main area stays blank). This static draw
 * is the partial-refresh baseline; the awake loop ticks the countdown on top of
 * it via identity_countdown_overlay. The status variant adds the battery + a
 * shrinking countdown bar (shown after a config disconnect, until sleep). */
void identity_screen_draw(GUI &gui, const char *name, const char *table,
                          const uint8_t *img, uint8_t img_fmt,
                          uint16_t img_w, uint16_t img_h);

/* Draw the bottom name/ID banner over an already-filled framebuffer WITHOUT clearing
 * it (e.g. a full-screen B/W identity image blitted in). 1-bit, so it supports the
 * live awake-window countdown. */
void identity_banner_over(GUI &gui, const char *name, const char *table);
void identity_status_screen_draw(GUI &gui, const char *name, const char *table,
                                 int batt_mv, int batt_pct,
                                 int remaining_sec, int total_sec);

/* Re-composite the identity frame's bottom banner + a thin countdown fill onto an
 * already-built framebuffer (call identity_screen_draw first, push it as the
 * partial-refresh base, then call this each tick + refresh_partial). The fill
 * shrinks from full toward empty as remaining/total falls. When event_name is
 * non-empty (an event mesh heartbeat is being heard) it replaces the attendee
 * name on the banner's left. Idempotent — repaints its whole region. */
void identity_countdown_overlay(GUI &gui, const char *name, const char *table,
                                const char *event_name, int remaining_sec,
                                int total_sec);

/* Composite a small "asleep" indicator (crescent moon + ascending "z z z") into
 * the top-right corner of an already-built identity frame (call
 * identity_screen_draw first, then this, then push a full refresh). Drawn with
 * white halos so it reads over a blank corner or a dark identity image. Used to
 * leave the bistable panel showing the badge is in deep sleep. */
void identity_sleep_overlay(GUI &gui);

/* Bake the bottom name/ID banner (opaque) and, if sleeping, the asleep moon, into a
 * 2-bit grayscale source image, so a single 4-gray render shows the full-screen image
 * with the banner/moon over it. `scratch` is a 1-bit work framebuffer (the GUI
 * framebuffer), overwritten. Used by the gray-identity path (a 1-bit partial overlay
 * can't run over a 4-gray base). */
void identity_bake_overlays(uint8_t *src, uint16_t w, uint16_t h, uint8_t *scratch,
                            const char *name, const char *table, bool sleeping);

#endif /* VIBAMIX_QR_SCREEN_H */
