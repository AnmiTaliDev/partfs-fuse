/* inode.c - Inode lookup, write, allocation, and extent mapping
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <endian.h>

#include "partfs.h"
#include "io.h"
#include "btree.h"
#include "inode.h"

int inode_lookup(struct partfs_state *fs, uint64_t ino, struct partfs_inode *out)
{
    uint64_t root_lba = le64toh(fs->groups[0].inode_tree_root);
    if (root_lba == 0)
        return -EIO;
    return ibtree_lookup(fs, root_lba, ino, out);
}

int inode_write(struct partfs_state *fs, const struct partfs_inode *inode)
{
    uint64_t root_lba = le64toh(fs->groups[0].inode_tree_root);
    if (root_lba == 0)
        return -EIO;
    return ibtree_update(fs, root_lba, inode);
}

int inode_alloc(struct partfs_state *fs, uint64_t *out_ino)
{
    uint64_t new_ino = le64toh(fs->sb.inode_count) + 1;
    fs->sb.inode_count = htole64(new_ino);
    if (sb_write(fs) < 0)
        return -EIO;
    *out_ino = new_ino;
    return 0;
}

uint64_t inode_map_block(const struct partfs_inode *inode, uint64_t logical_blk)
{
    uint16_t extent_count = le16toh(inode->extent_count);
    const struct partfs_extent *extents = (const struct partfs_extent *)inode->tail;

    for (uint16_t i = 0; i < extent_count && i < PARTFS_MAX_EXTENTS; i++) {
        uint64_t lb  = le64toh(extents[i].logical_block);
        uint16_t len = le16toh(extents[i].length);
        if (logical_blk >= lb && logical_blk < lb + len)
            return extent_phys(&extents[i]) + (logical_blk - lb);
    }
    return 0;
}
