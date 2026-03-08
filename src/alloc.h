/* alloc.h - Block allocation and bitmap management
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "partfs.h"

int   bitmap_get(struct partfs_state *fs, uint64_t group_no, uint64_t bit);
int   bitmap_set(struct partfs_state *fs, uint64_t group_no, uint64_t bit, int val);
int64_t block_alloc(struct partfs_state *fs, uint64_t preferred_group);
int   block_free(struct partfs_state *fs, uint64_t lba);
