#include "hash.h"

uint64_t bk_hash_fnv1a(const uint8_t *data, size_t len)
{
    const uint64_t offset_basis = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;

    uint64_t hash = offset_basis;

    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= prime;
    }

    return hash;
}
