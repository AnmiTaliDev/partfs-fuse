/* crc32c.c - CRC32C (Castagnoli) and FNV-1a hashing
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "crc32c.h"

static uint32_t crc32c_table[256];

void crc32c_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x82F63B78U;
            else
                crc >>= 1;
        }
        crc32c_table[i] = crc;
    }
}

uint32_t crc32c_compute(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--)
        crc = (crc >> 8) ^ crc32c_table[(crc ^ *p++) & 0xFF];
    return ~crc;
}

uint64_t fnv1a_64(const uint8_t *data, size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= prime;
    }
    return hash;
}
