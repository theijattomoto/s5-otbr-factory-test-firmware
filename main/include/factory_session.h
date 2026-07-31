#ifndef FACTORY_SESSION_H
#define FACTORY_SESSION_H

#include "factory_manifest.h"

#define FACTORY_SESSION_TIMEOUT_US       300000000LL
#define FACTORY_SESSION_UNIT_ID_MAX      48

typedef enum {
    FACTORY_SESSION_IDLE = 0,
    FACTORY_SESSION_ACTIVE,
    FACTORY_SESSION_EXPIRED,
    FACTORY_SESSION_COMPLETED,
    FACTORY_SESSION_ABORTED
} factory_session_state_t;

typedef struct {
    factory_session_state_t state;
    char unit_id[FACTORY_SESSION_UNIT_ID_MAX];
    int64_t started_us;
    int64_t last_activity_us;
    factory_manifest_summary_t manifest;
} factory_session_snapshot_t;

typedef factory_result_t (*factory_cleanup_fn_t)(void);

factory_result_t factory_session_init(factory_cleanup_fn_t cleanup);
factory_result_t factory_session_start(const char *unit_id, int64_t now_us);
factory_result_t factory_session_touch(int64_t now_us);
factory_result_t factory_session_expire(int64_t now_us, bool *expired);
factory_result_t factory_session_finish(factory_session_snapshot_t *snapshot);
factory_result_t factory_session_abort(void);
bool factory_session_is_active(void);
void factory_session_snapshot(factory_session_snapshot_t *snapshot);
const char *factory_session_state_name(factory_session_state_t state);

#endif
