#ifndef FACTORY_MANIFEST_H
#define FACTORY_MANIFEST_H

#include "factory_types.h"

#define FACTORY_MANIFEST_UNIT_MAX       16
#define FACTORY_MANIFEST_DETAIL_MAX     96

typedef enum {
    FACTORY_TEST_PENDING = 0,
    FACTORY_TEST_PASS,
    FACTORY_TEST_FAIL
} factory_test_status_t;

typedef enum {
    FACTORY_TEST_AUTOMATIC = 0,
    FACTORY_TEST_STATION
} factory_test_owner_t;

typedef struct {
    const char *id;
    const char *description;
    factory_test_owner_t owner;
    factory_test_status_t status;
    bool has_value;
    double value;
    char unit[FACTORY_MANIFEST_UNIT_MAX];
    char detail[FACTORY_MANIFEST_DETAIL_MAX];
} factory_manifest_entry_t;

typedef struct {
    uint32_t pending;
    uint32_t passed;
    uint32_t failed;
} factory_manifest_summary_t;

void factory_manifest_reset(void);
size_t factory_manifest_count(void);
factory_result_t factory_manifest_get(size_t index,
                                      factory_manifest_entry_t *entry);
factory_result_t factory_manifest_record(const char *test_id,
                                         factory_test_owner_t actor,
                                         bool passed,
                                         bool has_value,
                                         double value,
                                         const char *unit,
                                         const char *detail);
void factory_manifest_summary(factory_manifest_summary_t *summary);
const char *factory_test_status_name(factory_test_status_t status);
const char *factory_test_owner_name(factory_test_owner_t owner);

#endif
