/* file.c - File data read/write using extents
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <errno.h>
#include <endian.h>

#include "partfs.h"
#include "io.h"
#include "alloc.h"
#include "inode.h"
#include "file.h"

ssize_t file_read_data(struct partfs_state *fs, const struct partfs_inode *inode,
                        void *buf, size_t size, off_t offset)
{
    uint64_t file_size = le64toh(inode->size);
    uint16_t iflags    = le16toh(inode->flags);

    if (iflags & PARTFS_IFLAG_INLINE) {
        uint16_t inline_len   = le16toh(inode->inline_len);
        uint16_t xattr_len    = le16toh(inode->xattr_len);
        uint16_t extent_count = le16toh(inode->extent_count);
        const uint8_t *data   = inode->tail
            + (size_t)extent_count * sizeof(struct partfs_extent)
            + xattr_len;
        if ((uint64_t)offset >= inline_len)
            return 0;
        size_t avail   = inline_len - (size_t)offset;
        size_t to_read = size < avail ? size : avail;
        memcpy(buf, data + offset, to_read);
        return (ssize_t)to_read;
    }

    if ((uint64_t)offset >= file_size)
        return 0;

    uint8_t block_buf[PARTFS_BLOCK_SIZE];
    size_t total = 0;

    while (size > 0 && (uint64_t)offset < file_size) {
        uint64_t logical_blk = (uint64_t)offset / PARTFS_BLOCK_SIZE;
        uint32_t blk_off     = (uint32_t)((uint64_t)offset % PARTFS_BLOCK_SIZE);
        uint64_t phys_lba    = inode_map_block(inode, logical_blk);

        size_t to_read = PARTFS_BLOCK_SIZE - blk_off;
        if (to_read > size)
            to_read = size;
        if ((uint64_t)(offset + (off_t)to_read) > file_size)
            to_read = (size_t)(file_size - (uint64_t)offset);

        if (phys_lba == 0) {
            memset((uint8_t *)buf + total, 0, to_read);
        } else {
            if (block_read(fs->fd, phys_lba, block_buf) < 0)
                return -EIO;
            memcpy((uint8_t *)buf + total, block_buf + blk_off, to_read);
        }

        total  += to_read;
        offset += (off_t)to_read;
        size   -= to_read;
    }
    return (ssize_t)total;
}

ssize_t file_write_data(struct partfs_state *fs, struct partfs_inode *inode,
                         const void *buf, size_t size, off_t offset)
{
    if (size == 0)
        return 0;

    struct partfs_extent *extents = (struct partfs_extent *)inode->tail;
    uint16_t extent_count = le16toh(inode->extent_count);
    uint64_t file_size    = le64toh(inode->size);
    uint64_t blocks_used  = le64toh(inode->blocks_used);

    uint64_t pref_group = 0;
    if (extent_count > 0) {
        uint64_t last_phys  = extent_phys(&extents[extent_count - 1]);
        uint32_t group_size = le32toh(fs->sb.group_size);
        if (last_phys >= fs->groups_start)
            pref_group = (last_phys - fs->groups_start) / group_size;
    }

    uint8_t block_buf[PARTFS_BLOCK_SIZE];
    size_t total     = 0;
    off_t cur_off    = offset;
    const uint8_t *src = (const uint8_t *)buf;

    while (size > 0) {
        uint64_t logical_blk = (uint64_t)cur_off / PARTFS_BLOCK_SIZE;
        uint32_t blk_off     = (uint32_t)((uint64_t)cur_off % PARTFS_BLOCK_SIZE);
        uint64_t phys_lba    = inode_map_block(inode, logical_blk);

        if (phys_lba == 0) {
            int64_t new_lba = block_alloc(fs, pref_group);
            if (new_lba < 0)
                return (total > 0) ? (ssize_t)total : new_lba;

            phys_lba = (uint64_t)new_lba;
            memset(block_buf, 0, sizeof(block_buf));

            int extended = 0;
            if (extent_count > 0) {
                struct partfs_extent *last = &extents[extent_count - 1];
                uint64_t last_logical = le64toh(last->logical_block);
                uint16_t last_len     = le16toh(last->length);
                uint64_t last_phys    = extent_phys(last);
                if (logical_blk == last_logical + last_len
                    && phys_lba == last_phys + last_len) {
                    last->length = htole16(last_len + 1);
                    extended = 1;
                }
            }
            if (!extended) {
                if (extent_count >= PARTFS_MAX_EXTENTS) {
                    block_free(fs, phys_lba);
                    return (total > 0) ? (ssize_t)total : -ENOSPC;
                }
                extents[extent_count].logical_block = htole64(logical_blk);
                extent_set_phys(&extents[extent_count], phys_lba);
                extents[extent_count].length = htole16(1);
                extent_count++;
                inode->extent_count = htole16(extent_count);
            }
            blocks_used++;
            inode->blocks_used = htole64(blocks_used);
        } else {
            if (block_read(fs->fd, phys_lba, block_buf) < 0)
                return (total > 0) ? (ssize_t)total : -EIO;
        }

        size_t to_write = PARTFS_BLOCK_SIZE - blk_off;
        if (to_write > size)
            to_write = size;

        memcpy(block_buf + blk_off, src, to_write);
        if (block_write(fs->fd, phys_lba, block_buf) < 0)
            return (total > 0) ? (ssize_t)total : -EIO;

        total   += to_write;
        cur_off += (off_t)to_write;
        src     += to_write;
        size    -= to_write;

        if ((uint64_t)cur_off > file_size)
            file_size = (uint64_t)cur_off;
    }

    inode->size     = htole64(file_size);
    inode->mtime_ns = htole64(time_now_ns());
    return (ssize_t)total;
}
