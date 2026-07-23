#include "meshcore_log.h"

#include <stdarg.h>

struct MeshCoreLog {
    FuriString* text;
    FuriMutex* mutex;
};

MeshCoreLog* meshcore_log_alloc(void) {
    MeshCoreLog* log = malloc(sizeof(MeshCoreLog));
    log->text = furi_string_alloc();
    log->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    return log;
}

void meshcore_log_free(MeshCoreLog* log) {
    furi_assert(log);
    furi_mutex_free(log->mutex);
    furi_string_free(log->text);
    free(log);
}

/* Caller holds the mutex. Drops oldest text, then the leftover partial line so
 * the view never starts mid-entry. */
static void meshcore_log_trim(MeshCoreLog* log) {
    size_t size = furi_string_size(log->text);
    if(size <= MESHCORE_LOG_CAP) return;

    furi_string_right(log->text, size - MESHCORE_LOG_CAP);

    size_t newline = furi_string_search_char(log->text, '\n', 0);
    if(newline != FURI_STRING_FAILURE) {
        furi_string_right(log->text, newline + 1);
    }
}

void meshcore_log_printf(MeshCoreLog* log, const char* format, ...) {
    furi_assert(log);
    furi_mutex_acquire(log->mutex, FuriWaitForever);

    va_list args;
    va_start(args, format);
    furi_string_cat_vprintf(log->text, format, args);
    va_end(args);
    furi_string_cat_str(log->text, "\n");

    meshcore_log_trim(log);
    furi_mutex_release(log->mutex);
}

void meshcore_log_frame(MeshCoreLog* log, bool tx, const uint8_t* payload, size_t len) {
    furi_assert(log);
    furi_mutex_acquire(log->mutex, FuriWaitForever);

    /* Deliberately "TX"/"RX" and not '<'/'>'. In this protocol those glyphs are
     * the wire lead bytes and they mean the opposite of the intuitive
     * out/in: we transmit frames led by 0x3C '<' and receive ones led by
     * 0x3E '>'. Printing the arrows here would read backwards to anyone who
     * knows the protocol, which is exactly who uses this view. */
    furi_string_cat_printf(log->text, "%s %02X", tx ? "TX" : "RX", len ? payload[0] : 0);

    /* One frame is at most MC_MAX_PAYLOAD bytes; cap the dump so a burst of
     * traffic cannot flush the whole log in one entry. */
    size_t shown = len < 24 ? len : 24;
    for(size_t i = 1; i < shown; i++) {
        furi_string_cat_printf(log->text, " %02X", payload[i]);
    }
    if(shown < len) {
        furi_string_cat_printf(log->text, " +%u", (unsigned)(len - shown));
    }
    furi_string_cat_str(log->text, "\n");

    meshcore_log_trim(log);
    furi_mutex_release(log->mutex);
}

void meshcore_log_snapshot(MeshCoreLog* log, FuriString* out) {
    furi_assert(log);
    furi_mutex_acquire(log->mutex, FuriWaitForever);
    furi_string_set(out, log->text);
    furi_mutex_release(log->mutex);
}

void meshcore_log_clear(MeshCoreLog* log) {
    furi_assert(log);
    furi_mutex_acquire(log->mutex, FuriWaitForever);
    furi_string_reset(log->text);
    furi_mutex_release(log->mutex);
}
