/* dir.h - Directory operations
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "partfs.h"

int dir_lookup(struct partfs_state *fs, const struct partfs_inode *dir,
               const char *name, uint64_t *out_ino, uint16_t *out_type);

int dir_add(struct partfs_state *fs, struct partfs_inode *dir,
            const char *name, uint64_t ino, uint16_t itype);

int dir_remove(struct partfs_state *fs, struct partfs_inode *dir,
               const char *name);

/* Iterate all valid directory entries. Callback returns 0 to continue, non-zero to stop. */
typedef int (*dir_iter_fn)(void *arg, const char *name, size_t name_len,
                            uint64_t ino, uint16_t itype);

int dir_iter(struct partfs_state *fs, const struct partfs_inode *dir,
             dir_iter_fn fn, void *arg);
