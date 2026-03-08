/* crc32c.h - CRC32C (Castagnoli) and FNV-1a hashing
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

void     crc32c_init(void);
uint32_t crc32c_compute(uint32_t crc, const void *buf, size_t len);
uint64_t fnv1a_64(const uint8_t *data, size_t len);
