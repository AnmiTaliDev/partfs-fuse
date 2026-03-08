/* btree.h - Inode B-tree operations
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "partfs.h"

int ibtree_lookup(struct partfs_state *fs, uint64_t node_lba,
                  uint64_t target_ino, struct partfs_inode *out);
int ibtree_insert(struct partfs_state *fs, uint64_t ino,
                  const struct partfs_inode *inode);
int ibtree_update(struct partfs_state *fs, uint64_t node_lba,
                  const struct partfs_inode *inode);
