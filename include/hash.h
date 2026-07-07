#ifndef BLINKKV_HASH_H
#define BLINKKV_HASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t bk_hash_fnv1a(const uint8_t *data, size_t len);

#endif
