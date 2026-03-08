/* alloc.c - Block allocation and bitmap management
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <endian.h>

#include "partfs.h"
#include "io.h"
#include "alloc.h"

static uint64_t bitmap_block_lba(const struct partfs_state *fs,
                                  uint64_t group_no, uint64_t bit)
{
    uint64_t bitmap_start = le64toh(fs->groups[group_no].bitmap_start);
    return bitmap_start + bit / (PARTFS_BLOCK_SIZE * 8);
}

int bitmap_get(struct partfs_state *fs, uint64_t group_no, uint64_t bit)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    uint64_t lba = bitmap_block_lba(fs, group_no, bit);
    if (block_read(fs->fd, lba, buf) < 0)
        return -EIO;
    uint64_t byte_idx = (bit / 8) % PARTFS_BLOCK_SIZE;
    uint64_t bit_idx  = bit % 8;
    return (buf[byte_idx] >> bit_idx) & 1;
}

int bitmap_set(struct partfs_state *fs, uint64_t group_no, uint64_t bit, int val)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    uint64_t lba = bitmap_block_lba(fs, group_no, bit);
    if (block_read(fs->fd, lba, buf) < 0)
        return -EIO;
    uint64_t byte_idx = (bit / 8) % PARTFS_BLOCK_SIZE;
    uint64_t bit_idx  = bit % 8;
    if (val)
        buf[byte_idx] |= (uint8_t)(1 << bit_idx);
    else
        buf[byte_idx] &= (uint8_t)~(1 << bit_idx);
    return block_write(fs->fd, lba, buf);
}

int64_t block_alloc(struct partfs_state *fs, uint64_t preferred_group)
{
    uint32_t group_size = le32toh(fs->sb.group_size);

    for (uint64_t gi = 0; gi < fs->num_groups; gi++) {
        uint64_t group_no = (preferred_group + gi) % fs->num_groups;
        if (le64toh(fs->groups[group_no].free_blocks) == 0)
            continue;

        uint64_t total = le64toh(fs->groups[group_no].total_blocks);
        for (uint64_t bit = 0; bit < total && bit < group_size; bit++) {
            int r = bitmap_get(fs, group_no, bit);
            if (r < 0)
                return r;
            if (r == 0) {
                if (bitmap_set(fs, group_no, bit, 1) < 0)
                    return -EIO;
                uint64_t lba = group_base_lba(fs, group_no) + bit;
                fs->groups[group_no].free_blocks =
                    htole64(le64toh(fs->groups[group_no].free_blocks) - 1);
                fs->sb.free_blocks =
                    htole64(le64toh(fs->sb.free_blocks) - 1);
                gd_write(fs, group_no);
                sb_write(fs);
                return (int64_t)lba;
            }
        }
    }
    return -ENOSPC;
}

int block_free(struct partfs_state *fs, uint64_t lba)
{
    uint32_t group_size = le32toh(fs->sb.group_size);
    uint64_t rel      = lba - fs->groups_start;
    uint64_t group_no = rel / group_size;
    uint64_t bit      = rel % group_size;

    if (group_no >= fs->num_groups)
        return -EINVAL;

    if (bitmap_set(fs, group_no, bit, 0) < 0)
        return -EIO;

    fs->groups[group_no].free_blocks =
        htole64(le64toh(fs->groups[group_no].free_blocks) + 1);
    fs->sb.free_blocks =
        htole64(le64toh(fs->sb.free_blocks) + 1);
    gd_write(fs, group_no);
    sb_write(fs);
    return 0;
}
