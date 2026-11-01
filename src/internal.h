#ifndef RC_INTERNAL_H
#define RC_INTERNAL_H
#include "platform.h"
#include "riden_core.h"
uint16_t rc_crc(const uint8_t *, size_t);
int rc_exchange(const rc_transport *, const rc_config *, uint64_t *last_io, uint8_t function,
                uint16_t start, uint16_t count, uint16_t *values, uint64_t *sent,
                uint64_t *completed);
#endif
