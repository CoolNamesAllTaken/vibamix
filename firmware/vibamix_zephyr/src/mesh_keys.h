#ifndef VIBAMIX_MESH_KEYS_H
#define VIBAMIX_MESH_KEYS_H

/*
 * Baked-in mesh credentials for the zero-config badge network.
 *
 * Every badge ships with the SAME network key, application key and device key,
 * so a fresh unit self-provisions at boot and immediately joins the one shared
 * mesh with no commissioning step. The controller (phone/laptop) must be seeded
 * with these same keys to talk to the fleet over the GATT proxy.
 *
 * SECURITY: a shared key means anyone who extracts it can inject to every badge.
 * This is an accepted tradeoff for an event-badge demo — do NOT reuse for
 * anything that needs real isolation between nodes.
 */

#define VIBAMIX_NET_KEY  { 0x56, 0x49, 0x42, 0x41, 0x4d, 0x49, 0x58, 0x4e, \
                           0x45, 0x54, 0x4b, 0x45, 0x59, 0x30, 0x30, 0x31 }
#define VIBAMIX_APP_KEY  { 0x56, 0x49, 0x42, 0x41, 0x4d, 0x49, 0x58, 0x41, \
                           0x50, 0x50, 0x4b, 0x45, 0x59, 0x30, 0x30, 0x31 }
#define VIBAMIX_DEV_KEY  { 0x56, 0x49, 0x42, 0x41, 0x4d, 0x49, 0x58, 0x44, \
                           0x45, 0x56, 0x4b, 0x45, 0x59, 0x30, 0x30, 0x31 }

#define VIBAMIX_NET_IDX  0
#define VIBAMIX_APP_IDX  0

/* Group every badge subscribes to. Sending here addresses "all badges". */
#define VIBAMIX_GROUP_ADDR 0xC000

#endif /* VIBAMIX_MESH_KEYS_H */
