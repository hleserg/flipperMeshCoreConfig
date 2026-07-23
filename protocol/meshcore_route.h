/*
 * Where does a decoded event go?
 *
 * Deliberately free of furi and of threads: this is the policy the session
 * worker applies to every frame that arrives, and it is the part most likely
 * to be subtly wrong (a reply mistaken for a push loses the reply; a push
 * mistaken for a reply loses an incoming message). Keeping it pure means the
 * host tests can cover it without emulating a scheduler.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshcore_c/meshcore_companion.h"

typedef enum {
    /** Completes the request in flight (the awaited code, or a node error). */
    MeshCoreRouteReply,
    /** Offer to the streaming collector; if it declines, treat as an event. */
    MeshCoreRouteStream,
    /** Unsolicited — hand to the application. */
    MeshCoreRouteEvent,
} MeshCoreRoute;

/**
 * @param pending     a request is in flight
 * @param want_code   the response code that completes it
 * @param has_stream  the request collects a multi-frame reply
 * @param ev_code     the code just decoded
 */
MeshCoreRoute meshcore_route_event(bool pending, uint8_t want_code, bool has_stream, uint8_t ev_code);
