#ifndef FACTORY_IDENTITY_H
#define FACTORY_IDENTITY_H

#include "factory_types.h"

typedef struct {
    char target[16];
    uint32_t silicon_revision;
    uint32_t core_count;
    uint32_t flash_size_bytes;
    char base_mac[18];
    char thread_eui64[24];
    uint32_t reset_reason;
} factory_identity_t;

factory_result_t factory_identity_read(factory_identity_t *identity);

#endif
