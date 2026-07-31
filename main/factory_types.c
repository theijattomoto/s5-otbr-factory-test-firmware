#include "factory_types.h"

const char *factory_result_code(factory_result_t result)
{
    switch (result) {
        case FACTORY_OK: return "ok";
        case FACTORY_ERR_INVALID_ARGUMENT: return "invalid_parameter";
        case FACTORY_ERR_INVALID_FRAME: return "invalid_frame";
        case FACTORY_ERR_INVALID_JSON: return "invalid_json";
        case FACTORY_ERR_INVALID_REQUEST: return "invalid_request";
        case FACTORY_ERR_FRAME_TOO_LONG: return "frame_too_long";
        case FACTORY_ERR_UNKNOWN_COMMAND: return "unknown_command";
        case FACTORY_ERR_NOT_FOUND: return "unknown_test";
        case FACTORY_ERR_INVALID_STATE: return "invalid_state";
        case FACTORY_ERR_IMMUTABLE: return "result_immutable";
        case FACTORY_ERR_UNAUTHORIZED: return "test_owner_mismatch";
        case FACTORY_ERR_INCOMPLETE: return "incomplete_or_failed";
        case FACTORY_ERR_TIMEOUT: return "session_timeout";
        case FACTORY_ERR_CLEANUP: return "cleanup_failed";
        case FACTORY_ERR_TRANSPORT: return "transport_error";
        case FACTORY_ERR_NO_MEMORY: return "no_memory";
        case FACTORY_ERR_HARDWARE: return "hardware_error";
        case FACTORY_ERR_MEASUREMENT_RANGE:
            return "measurement_out_of_range";
        case FACTORY_ERR_FREQUENCY_RANGE:
            return "frequency_out_of_range";
        default: return "internal_error";
    }
}

factory_result_t factory_apply_error_cleanup(
    factory_result_t result, factory_error_cleanup_fn_t cleanup)
{
    if (result == FACTORY_OK) {
        return FACTORY_OK;
    }
    if (cleanup == NULL || cleanup() != FACTORY_OK) {
        return FACTORY_ERR_CLEANUP;
    }
    return result;
}
