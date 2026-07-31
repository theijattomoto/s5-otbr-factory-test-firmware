#ifndef FACTORY_TYPES_H
#define FACTORY_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FACTORY_OK = 0,
    FACTORY_ERR_INVALID_ARGUMENT,
    FACTORY_ERR_INVALID_FRAME,
    FACTORY_ERR_INVALID_JSON,
    FACTORY_ERR_INVALID_REQUEST,
    FACTORY_ERR_FRAME_TOO_LONG,
    FACTORY_ERR_UNKNOWN_COMMAND,
    FACTORY_ERR_NOT_FOUND,
    FACTORY_ERR_INVALID_STATE,
    FACTORY_ERR_IMMUTABLE,
    FACTORY_ERR_UNAUTHORIZED,
    FACTORY_ERR_INCOMPLETE,
    FACTORY_ERR_TIMEOUT,
    FACTORY_ERR_CLEANUP,
    FACTORY_ERR_TRANSPORT,
    FACTORY_ERR_HARDWARE,
    FACTORY_ERR_NO_MEMORY
} factory_result_t;

const char *factory_result_code(factory_result_t result);
typedef factory_result_t (*factory_error_cleanup_fn_t)(void);
factory_result_t factory_apply_error_cleanup(
    factory_result_t result, factory_error_cleanup_fn_t cleanup);

#endif
