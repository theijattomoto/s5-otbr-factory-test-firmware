#include "factory_session.h"

#include <ctype.h>
#include <string.h>

static factory_session_snapshot_t s_session;
static factory_cleanup_fn_t s_cleanup;

static bool valid_unit_id(const char *unit_id)
{
    if (unit_id == NULL) {
        return false;
    }
    const size_t length = strlen(unit_id);
    if (length == 0 || length >= FACTORY_SESSION_UNIT_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)unit_id[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static void refresh_summary(void)
{
    factory_manifest_summary(&s_session.manifest);
}

factory_result_t factory_session_init(factory_cleanup_fn_t cleanup)
{
    if (cleanup == NULL) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = FACTORY_SESSION_IDLE;
    s_cleanup = cleanup;
    factory_manifest_reset();
    refresh_summary();
    return FACTORY_OK;
}

factory_result_t factory_session_start(const char *unit_id, int64_t now_us)
{
    if (s_cleanup == NULL || !valid_unit_id(unit_id) || now_us < 0) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    if (s_session.state == FACTORY_SESSION_ACTIVE) {
        return FACTORY_ERR_INVALID_STATE;
    }

    const factory_result_t cleanup_result = s_cleanup();
    if (cleanup_result != FACTORY_OK) {
        s_session.state = FACTORY_SESSION_ABORTED;
        return FACTORY_ERR_CLEANUP;
    }

    factory_manifest_reset();
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = FACTORY_SESSION_ACTIVE;
    memcpy(s_session.unit_id, unit_id, strlen(unit_id) + 1);
    s_session.started_us = now_us;
    s_session.last_activity_us = now_us;
    refresh_summary();
    return FACTORY_OK;
}

factory_result_t factory_session_touch(int64_t now_us)
{
    if (s_session.state != FACTORY_SESSION_ACTIVE) {
        return FACTORY_ERR_INVALID_STATE;
    }
    if (now_us < s_session.last_activity_us) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    s_session.last_activity_us = now_us;
    return FACTORY_OK;
}

factory_result_t factory_session_expire(int64_t now_us, bool *expired)
{
    if (expired == NULL || now_us < 0) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    *expired = false;
    if (s_session.state != FACTORY_SESSION_ACTIVE) {
        return FACTORY_OK;
    }
    if (now_us < s_session.last_activity_us) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    if (now_us - s_session.last_activity_us < FACTORY_SESSION_TIMEOUT_US) {
        return FACTORY_OK;
    }

    *expired = true;
    s_session.state = FACTORY_SESSION_EXPIRED;
    refresh_summary();
    return s_cleanup() == FACTORY_OK ? FACTORY_ERR_TIMEOUT :
                                       FACTORY_ERR_CLEANUP;
}

factory_result_t factory_session_finish(factory_session_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return FACTORY_ERR_INVALID_ARGUMENT;
    }
    if (s_session.state != FACTORY_SESSION_ACTIVE) {
        factory_session_snapshot(snapshot);
        return FACTORY_ERR_INVALID_STATE;
    }

    refresh_summary();
    if (s_session.manifest.pending != 0 || s_session.manifest.failed != 0) {
        const factory_result_t cleanup_result = s_cleanup();
        factory_session_snapshot(snapshot);
        return cleanup_result == FACTORY_OK ? FACTORY_ERR_INCOMPLETE :
                                              FACTORY_ERR_CLEANUP;
    }

    if (s_cleanup() != FACTORY_OK) {
        s_session.state = FACTORY_SESSION_ABORTED;
        factory_session_snapshot(snapshot);
        return FACTORY_ERR_CLEANUP;
    }

    s_session.state = FACTORY_SESSION_COMPLETED;
    factory_session_snapshot(snapshot);
    return FACTORY_OK;
}

factory_result_t factory_session_abort(void)
{
    if (s_cleanup == NULL) {
        return FACTORY_ERR_INVALID_STATE;
    }

    /*
     * Safety cleanup is always idempotent, but terminal audit states must not
     * be rewritten by a repeated or late abort request.
     */
    if (s_session.state == FACTORY_SESSION_ACTIVE ||
        s_session.state == FACTORY_SESSION_IDLE) {
        s_session.state = FACTORY_SESSION_ABORTED;
    }
    refresh_summary();
    return s_cleanup() == FACTORY_OK ? FACTORY_OK : FACTORY_ERR_CLEANUP;
}

bool factory_session_is_active(void)
{
    return s_session.state == FACTORY_SESSION_ACTIVE;
}

void factory_session_snapshot(factory_session_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    refresh_summary();
    *snapshot = s_session;
}

const char *factory_session_state_name(factory_session_state_t state)
{
    switch (state) {
        case FACTORY_SESSION_ACTIVE: return "active";
        case FACTORY_SESSION_EXPIRED: return "expired";
        case FACTORY_SESSION_COMPLETED: return "completed";
        case FACTORY_SESSION_ABORTED: return "aborted";
        default: return "idle";
    }
}
