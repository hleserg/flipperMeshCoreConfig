#include "meshcore_messages.h"

#include <stdio.h>
#include <string.h>

void meshcore_messages_reset(MeshCoreMessages* messages) {
    memset(messages, 0, sizeof(*messages));
}

void meshcore_messages_add(MeshCoreMessages* messages, const MeshCoreMessage* message) {
    size_t slot;

    if(messages->count < MESHCORE_MESSAGES_MAX) {
        slot = (messages->head + messages->count) % MESHCORE_MESSAGES_MAX;
        messages->count++;
    } else {
        /* Full: overwrite the oldest and move the window along. */
        slot = messages->head;
        messages->head = (messages->head + 1) % MESHCORE_MESSAGES_MAX;
    }

    messages->items[slot] = *message;
    messages->total++;
}

const MeshCoreMessage* meshcore_messages_at(const MeshCoreMessages* messages, size_t index) {
    if(index >= messages->count) return NULL;
    return &messages->items[(messages->head + index) % MESHCORE_MESSAGES_MAX];
}

bool meshcore_message_from_event(const mc_event_t* event, MeshCoreMessage* out) {
    memset(out, 0, sizeof(*out));
    out->direction = MeshCoreMessageIncoming;

    switch(event->code) {
    /* V3 differs only by carrying SNR and RSSI; the library has already
     * normalised that into the same struct. */
    case MC_RESP_CONTACT_MSG_RECV:
    case MC_RESP_CONTACT_MSG_RECV_V3: {
        const mc_contact_msg_t* msg = &event->u.contact_msg;
        memcpy(out->peer, msg->pubkey_prefix, MESHCORE_PEER_LEN);
        out->is_channel = false;
        out->timestamp = msg->sender_ts;
        out->snr_q4 = msg->snr_q4;
        out->path_len = msg->path_len;
        snprintf(out->text, sizeof(out->text), "%s", msg->text);
        return true;
    }

    case MC_RESP_CHANNEL_MSG_RECV:
    case MC_RESP_CHANNEL_MSG_RECV_V3: {
        const mc_channel_msg_t* msg = &event->u.channel_msg;
        out->is_channel = true;
        /* channel_idx is a slot number 0-7 on the wire (parsed into an int8_t). */
        out->channel_idx = (uint8_t)msg->channel_idx;
        out->timestamp = msg->sender_ts;
        out->snr_q4 = msg->snr_q4;
        out->path_len = msg->path_len;
        /* For channels the text already reads "Sender: body". */
        snprintf(out->text, sizeof(out->text), "%s", msg->text);
        return true;
    }

    default:
        return false;
    }
}

bool meshcore_message_is_from(const MeshCoreMessage* message, const uint8_t* public_key) {
    if(message->is_channel) return false;
    return memcmp(message->peer, public_key, MESHCORE_PEER_LEN) == 0;
}

size_t meshcore_messages_count_for(const MeshCoreMessages* messages, const uint8_t* public_key) {
    size_t found = 0;
    for(size_t i = 0; i < messages->count; i++) {
        const MeshCoreMessage* message = meshcore_messages_at(messages, i);
        if(meshcore_message_is_from(message, public_key)) found++;
    }
    return found;
}
