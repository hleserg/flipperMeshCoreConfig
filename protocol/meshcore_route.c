#include "meshcore_route.h"

MeshCoreRoute
    meshcore_route_event(bool pending, uint8_t want_code, bool has_stream, uint8_t ev_code) {
    /* Nothing asked for anything: everything is unsolicited. This is the case
     * that matters for the messenger — a MSG_WAITING push arriving while the
     * user is idle must still reach the application. */
    if(!pending) return MeshCoreRouteEvent;

    /* The awaited code finishes the request. Checked before the error case so
     * that a request which genuinely awaits ERR still works. */
    if(ev_code == want_code) return MeshCoreRouteReply;

    /* A node-side error ends any request; the caller gets to see the code and
     * tell it apart from silence. */
    if(ev_code == MC_RESP_ERR) return MeshCoreRouteReply;

    /* Multi-frame replies (GET_CONTACTS and friends) are offered to the
     * collector, which declines anything that is not part of its stream — a
     * push that lands mid-stream then falls through to the application
     * instead of being swallowed. */
    if(has_stream) return MeshCoreRouteStream;

    return MeshCoreRouteEvent;
}
