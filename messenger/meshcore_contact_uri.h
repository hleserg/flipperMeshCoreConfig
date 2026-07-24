/*
 * meshcore_contact_uri — parse a MeshCore contact-share link.
 *
 * The official share format (docs.meshcore.io/qr_codes) is a query URI:
 *
 *   meshcore://contact/add?name=<url-encoded>&public_key=<64 hex>&type=<1-4>
 *
 * This turns one into an mc_contact_t ready for ADD_UPDATE_CONTACT (cmd 9) —
 * the same command the reference client (meshcore_py) uses to add a contact
 * from a public key, name and type. Kept furi-free so the host tests can cover
 * the fiddly parts: hex decoding, percent/plus decoding, and rejecting
 * malformed links before they reach the node.
 */
#pragma once

#include <stdbool.h>

#include "../protocol/meshcore_c/meshcore_companion.h"

/* Parse `uri` into `out`. Params may appear in any order; `name` is optional
 * and is percent/plus-decoded. `public_key` must be exactly 64 hex chars and
 * `type` must be 1..4, else this returns false and `out` is untouched-safe
 * (fully zeroed). On success `out` is zeroed then filled, with out_path_len set
 * to 0xFF so the node floods to rediscover the route to a brand-new contact. */
bool meshcore_contact_uri_parse(const char* uri, mc_contact_t* out);
