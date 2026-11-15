/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BILI_PROVISION_LINE_CAPACITY 768
#define BILI_PROVISION_NAMESPACE "hosino_bili"
#define BILI_PROVISION_BLOB_KEY "bridge_v1"
typedef struct { char base[256]; char token[193]; } bili_provision_config_t;
typedef enum { BILI_PROVISION_IGNORE=0, BILI_PROVISION_OK, BILI_PROVISION_INVALID,
    BILI_PROVISION_TOO_LONG, BILI_PROVISION_STORAGE } bili_provision_result_t;
typedef struct {
    char bytes[BILI_PROVISION_LINE_CAPACITY];
    size_t length;
    bool dropping;
    bool recognized;
} bili_provision_line_t;
/* READY: bytes holds a NUL-terminated line; caller must clear after parsing. */
typedef enum { BILI_LINE_NONE=0, BILI_LINE_READY, BILI_LINE_TOO_LONG } bili_line_result_t;
bili_line_result_t bili_provision_feed(bili_provision_line_t *line, unsigned char byte);
void bili_provision_clear(void *memory,size_t size);
bili_provision_result_t bili_provision_parse(const char *line,bili_provision_config_t *config);
bili_provision_result_t bili_provision_parse_request(const char *line,bili_provision_config_t *config,char request_id[17]);
bool bili_provision_sync_id(const char *line,char request_id[17]);
bool bili_provision_validate_config(bili_provision_config_t *config);
/* Performs parser validation before touching NVS. Fixed namespace/keys only. */
bili_provision_result_t bili_provision_apply(const char *line);
bili_provision_result_t bili_provision_apply_request(const char *line,char request_id[17]);
const char *bili_provision_ack(bili_provision_result_t result);
#ifdef __cplusplus
}
#endif
