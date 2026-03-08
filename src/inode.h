/* inode.h - Inode lookup, write, allocation, and extent mapping
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include "partfs.h"

int      inode_lookup(struct partfs_state *fs, uint64_t ino, struct partfs_inode *out);
int      inode_write(struct partfs_state *fs, const struct partfs_inode *inode);
int      inode_alloc(struct partfs_state *fs, uint64_t *out_ino);
uint64_t inode_map_block(const struct partfs_inode *inode, uint64_t logical_blk);
