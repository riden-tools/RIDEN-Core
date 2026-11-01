#ifndef RIDEN_CORE_H
#define RIDEN_CORE_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#ifdef RIDEN_BUILD
#define RC_API __declspec(dllexport)
#else
#define RC_API __declspec(dllimport)
#endif
#else
#define RC_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define RC_REGISTERS 121
#define RC_ABI_VERSION 2
/* Error values are stable ABI. Writes are never retried automatically. */
enum {
    RC_OK = 0,
    RC_IO = -1,
    RC_TIMEOUT = -2,
    RC_PROTOCOL = -3,
    RC_CRC = -4,
    RC_CLOSED = -5,
    RC_RANGE = -6,
    RC_QUEUE_FULL = -7,
    RC_READ_ONLY = -8,
    RC_UNSUPPORTED = -9,
    RC_NOMEM = -10,
    RC_BUSY = -11
};
/* One worker calls callbacks. read returns 0 on timeout, negative on I/O error.
 * write may be partial; both callbacks must honor timeout_ms. Bytes form a
 * continuous stream: BLE adapters must fragment TX and reassemble notifications.
 * The transport remains owned by caller; close it only after rc_destroy(). */
typedef struct {
    void *context;
    int (*read)(void *, uint8_t *, size_t, uint32_t timeout_ms);
    int (*write)(void *, const uint8_t *, size_t, uint32_t timeout_ms);
} rc_transport;
typedef struct {
    uint32_t abi_version;
    uint8_t address;
    uint16_t poll_start, poll_count; /* hot=10/4, status=4/16, full=0/121 */
    uint32_t period_ms;              /* 0: completion-driven maximum throughput */
    uint32_t timeout_ms, frame_gap_us;
    int allow_writes, allow_multiwrite;
} rc_config;
typedef struct {
    uint16_t model;
    const char *name;
    uint16_t voltage_scale, current_scale;
    double max_voltage, max_current, max_ovp, max_ocp;
} rc_model_info;
typedef struct {
    uint16_t address;
    const char *name;
    const char *unit;
    uint16_t scale;
    uint16_t raw_min, raw_max;
    uint8_t known, writable, inferred;
} rc_register_info;
typedef struct {
    uint64_t sequence, completed_ns, request_ns;
    uint64_t transactions, failures, queue_rejected, coalesced, cache_hits;
    uint32_t queued;
    int last_error;
    uint16_t registers[RC_REGISTERS];
    uint8_t valid[RC_REGISTERS];
    uint64_t updated_ns[RC_REGISTERS];
} rc_snapshot;
typedef struct rc_core rc_core;
RC_API rc_config rc_default_config(void);
RC_API int rc_get_model_info(uint16_t model, rc_model_info *info);
/* Registers scale/limits for a model ID without a built-in profile; registers
 * are assumed to follow the RD60xx map. Scales are 10/100/1000/10000 and each
 * limit × scale must fit in 16 bits. Built-in IDs return RC_BUSY; at most 8 IDs.
 * Call before rc_create(); it is not synchronized with running cores. */
RC_API int rc_register_model(const rc_model_info *info);
RC_API int rc_get_register_info(uint16_t model, uint16_t address, rc_register_info *info);
RC_API rc_core *rc_create(const rc_transport *, const rc_config *);
RC_API void rc_destroy(rc_core *); /* do not race with new API calls */
RC_API int rc_snapshot_get(rc_core *, rc_snapshot *);
/* Returns current snapshot immediately if sequence > after, else waits. A
 * timeout returns RC_TIMEOUT; it must not be recorded as a new sample. */
RC_API int rc_snapshot_wait(rc_core *, uint64_t after, uint32_t timeout_ms, rc_snapshot *);
/* Freshness is checked per register. max_age_ms=0 forces a physical read.
 * Matching/covering active or pending reads share one wire transaction. */
RC_API int rc_read(rc_core *, uint16_t start, uint16_t count, uint32_t max_age_ms,
                   uint16_t *values);
/* Raw units. Recognized model/range policy is enforced. 0x10 is opt-in until
 * capability has been verified for that firmware. Echo is not readback. */
RC_API int rc_write(rc_core *, uint16_t start, uint16_t count, const uint16_t *values);
RC_API const char *rc_error_string(int);
RC_API uint64_t rc_monotonic_ns(void);
/* Native desktop serial transport; Android supplies the callback transport. */
#ifndef __ANDROID__
RC_API int rc_serial_open(const char *path, unsigned baud, rc_transport *);
RC_API void rc_serial_close(rc_transport *);
#endif
#ifdef __cplusplus
}
#endif
#endif
