/* dir.c - Directory operations
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <errno.h>
#include <endian.h>

#include "partfs.h"
#include "crc32c.h"
#include "io.h"
#include "alloc.h"
#include "dir.h"

static uint32_t dir_block_used(const uint8_t *buf)
{
    const struct partfs_btree_leaf_hdr *lhdr = (const struct partfs_btree_leaf_hdr *)buf;
    uint32_t count = le32toh(lhdr->entry_count);
    uint32_t used  = PARTFS_BTLEAF_HDR_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (used + sizeof(struct partfs_dirent) > PARTFS_BLOCK_SIZE)
            break;
        const struct partfs_dirent *de = (const struct partfs_dirent *)(buf + used);
        uint16_t rec_len = le16toh(de->rec_len);
        if (rec_len == 0)
            break;
        used += rec_len;
    }
    return used;
}

int dir_iter(struct partfs_state *fs, const struct partfs_inode *dir,
             dir_iter_fn fn, void *arg)
{
    uint16_t extent_count = le16toh(dir->extent_count);
    const struct partfs_extent *extents = (const struct partfs_extent *)dir->tail;
    uint8_t buf[PARTFS_BLOCK_SIZE];

    for (uint16_t ei = 0; ei < extent_count; ei++) {
        uint16_t len  = le16toh(extents[ei].length);
        uint64_t phys = extent_phys(&extents[ei]);

        for (uint16_t bi = 0; bi < len; bi++) {
            if (block_read(fs->fd, phys + bi, buf) < 0)
                continue;

            struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
            uint32_t count = le32toh(lhdr->entry_count);
            uint32_t pos   = PARTFS_BTLEAF_HDR_SIZE;

            for (uint32_t i = 0; i < count; i++) {
                if (pos + sizeof(struct partfs_dirent) > PARTFS_BLOCK_SIZE)
                    break;
                struct partfs_dirent *de = (struct partfs_dirent *)(buf + pos);
                uint16_t rec_len = le16toh(de->rec_len);
                if (rec_len == 0)
                    break;

                uint64_t dino = le64toh(de->inode_no);
                if (dino != 0) {
                    uint16_t nlen = le16toh(de->name_len);
                    const char *dname = (const char *)(buf + pos + sizeof(struct partfs_dirent));
                    int r = fn(arg, dname, nlen, dino, le16toh(de->inode_type));
                    if (r != 0)
                        return r;
                }
                pos += rec_len;
            }
        }
    }
    return 0;
}

int dir_lookup(struct partfs_state *fs, const struct partfs_inode *dir,
               const char *name, uint64_t *out_ino, uint16_t *out_type)
{
    size_t name_len   = strlen(name);
    uint64_t name_hash = fnv1a_64((const uint8_t *)name, name_len);
    uint16_t extent_count = le16toh(dir->extent_count);
    const struct partfs_extent *extents = (const struct partfs_extent *)dir->tail;
    uint8_t buf[PARTFS_BLOCK_SIZE];

    for (uint16_t ei = 0; ei < extent_count; ei++) {
        uint16_t len  = le16toh(extents[ei].length);
        uint64_t phys = extent_phys(&extents[ei]);

        for (uint16_t bi = 0; bi < len; bi++) {
            if (block_read(fs->fd, phys + bi, buf) < 0)
                continue;

            struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
            uint32_t count = le32toh(lhdr->entry_count);
            uint32_t pos   = PARTFS_BTLEAF_HDR_SIZE;

            for (uint32_t i = 0; i < count; i++) {
                if (pos + sizeof(struct partfs_dirent) > PARTFS_BLOCK_SIZE)
                    break;
                struct partfs_dirent *de = (struct partfs_dirent *)(buf + pos);
                uint16_t rec_len = le16toh(de->rec_len);
                if (rec_len == 0)
                    break;

                uint64_t dino = le64toh(de->inode_no);
                if (dino != 0 && le64toh(de->name_hash) == name_hash) {
                    uint16_t dname_len = le16toh(de->name_len);
                    const char *dname  = (const char *)(buf + pos + sizeof(struct partfs_dirent));
                    if (dname_len == (uint16_t)name_len
                        && memcmp(dname, name, name_len) == 0) {
                        *out_ino  = dino;
                        *out_type = le16toh(de->inode_type);
                        return 0;
                    }
                }
                pos += rec_len;
            }
        }
    }
    return -ENOENT;
}

int dir_add(struct partfs_state *fs, struct partfs_inode *dir,
            const char *name, uint64_t ino, uint16_t itype)
{
    size_t name_len = strlen(name);
    if (name_len > 255)
        return -ENAMETOOLONG;

    uint64_t name_hash = fnv1a_64((const uint8_t *)name, name_len);
    uint16_t rec_len   = dirent_rec_len((uint16_t)name_len);
    uint16_t extent_count = le16toh(dir->extent_count);
    struct partfs_extent *extents = (struct partfs_extent *)dir->tail;
    uint8_t buf[PARTFS_BLOCK_SIZE];

    if (extent_count > 0) {
        struct partfs_extent *last = &extents[extent_count - 1];
        uint64_t last_blk = extent_phys(last) + le16toh(last->length) - 1;

        if (block_read(fs->fd, last_blk, buf) < 0)
            return -EIO;

        uint32_t used = dir_block_used(buf);
        if (used + rec_len <= PARTFS_BLOCK_SIZE) {
            struct partfs_dirent *de = (struct partfs_dirent *)(buf + used);
            memset(de, 0, rec_len);
            de->inode_no   = htole64(ino);
            de->name_hash  = htole64(name_hash);
            de->name_len   = htole16((uint16_t)name_len);
            de->rec_len    = htole16(rec_len);
            de->inode_type = htole16(itype);
            memcpy(buf + used + sizeof(struct partfs_dirent), name, name_len);
            de->crc32c = htole32(crc32c_compute(0, de, rec_len));

            struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
            lhdr->entry_count = htole32(le32toh(lhdr->entry_count) + 1);
            if (block_write(fs->fd, last_blk, buf) < 0)
                return -EIO;

            dir->size     = htole64(le64toh(dir->size) + rec_len);
            dir->mtime_ns = htole64(time_now_ns());
            return 0;
        }
    }

    /* Allocate a new directory block */
    uint64_t pref = 0;
    if (extent_count > 0) {
        uint64_t last_phys  = extent_phys(&extents[extent_count - 1]);
        uint32_t group_size = le32toh(fs->sb.group_size);
        if (last_phys >= fs->groups_start)
            pref = (last_phys - fs->groups_start) / group_size;
    }

    int64_t new_blk = block_alloc(fs, pref);
    if (new_blk < 0)
        return (int)new_blk;

    memset(buf, 0, sizeof(buf));
    struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
    lhdr->hdr.magic    = htole32(PARTFS_MAGIC_BTLF);
    lhdr->hdr.block_no = htole64((uint64_t)new_blk);
    lhdr->entry_count  = htole32(1);

    struct partfs_dirent *de = (struct partfs_dirent *)(buf + PARTFS_BTLEAF_HDR_SIZE);
    memset(de, 0, rec_len);
    de->inode_no   = htole64(ino);
    de->name_hash  = htole64(name_hash);
    de->name_len   = htole16((uint16_t)name_len);
    de->rec_len    = htole16(rec_len);
    de->inode_type = htole16(itype);
    memcpy(buf + PARTFS_BTLEAF_HDR_SIZE + sizeof(struct partfs_dirent), name, name_len);
    de->crc32c = htole32(crc32c_compute(0, de, rec_len));

    block_crc_set(buf);
    if (block_write(fs->fd, (uint64_t)new_blk, buf) < 0)
        return -EIO;

    /* Add extent to directory inode */
    if (extent_count > 0) {
        struct partfs_extent *last = &extents[extent_count - 1];
        uint64_t last_logical = le64toh(last->logical_block);
        uint16_t last_len     = le16toh(last->length);
        uint64_t last_phys    = extent_phys(last);

        if ((uint64_t)new_blk == last_phys + last_len) {
            last->length = htole16(last_len + 1);
        } else {
            if (extent_count >= PARTFS_MAX_EXTENTS) {
                block_free(fs, (uint64_t)new_blk);
                return -ENOSPC;
            }
            extents[extent_count].logical_block = htole64(last_logical + last_len);
            extent_set_phys(&extents[extent_count], (uint64_t)new_blk);
            extents[extent_count].length = htole16(1);
            dir->extent_count = htole16(extent_count + 1);
        }
    } else {
        extents[0].logical_block = htole64(0);
        extent_set_phys(&extents[0], (uint64_t)new_blk);
        extents[0].length = htole16(1);
        dir->extent_count = htole16(1);
    }

    dir->size        = htole64(le64toh(dir->size) + PARTFS_BLOCK_SIZE);
    dir->blocks_used = htole64(le64toh(dir->blocks_used) + 1);
    dir->mtime_ns    = htole64(time_now_ns());
    return 0;
}

int dir_remove(struct partfs_state *fs, struct partfs_inode *dir, const char *name)
{
    size_t name_len    = strlen(name);
    uint64_t name_hash = fnv1a_64((const uint8_t *)name, name_len);
    uint16_t extent_count = le16toh(dir->extent_count);
    const struct partfs_extent *extents = (const struct partfs_extent *)dir->tail;
    uint8_t buf[PARTFS_BLOCK_SIZE];

    for (uint16_t ei = 0; ei < extent_count; ei++) {
        uint16_t len  = le16toh(extents[ei].length);
        uint64_t phys = extent_phys(&extents[ei]);

        for (uint16_t bi = 0; bi < len; bi++) {
            uint64_t blk_lba = phys + bi;
            if (block_read(fs->fd, blk_lba, buf) < 0)
                continue;

            struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
            uint32_t count = le32toh(lhdr->entry_count);
            uint32_t pos   = PARTFS_BTLEAF_HDR_SIZE;

            for (uint32_t i = 0; i < count; i++) {
                if (pos + sizeof(struct partfs_dirent) > PARTFS_BLOCK_SIZE)
                    break;
                struct partfs_dirent *de = (struct partfs_dirent *)(buf + pos);
                uint16_t rec_len = le16toh(de->rec_len);
                if (rec_len == 0)
                    break;

                if (le64toh(de->inode_no) != 0
                    && le64toh(de->name_hash) == name_hash) {
                    uint16_t dname_len = le16toh(de->name_len);
                    const char *dname  = (const char *)(buf + pos + sizeof(struct partfs_dirent));
                    if (dname_len == (uint16_t)name_len
                        && memcmp(dname, name, name_len) == 0) {
                        de->inode_no = htole64(0);
                        de->crc32c   = 0;
                        de->crc32c   = htole32(crc32c_compute(0, de, rec_len));
                        if (block_write(fs->fd, blk_lba, buf) < 0)
                            return -EIO;
                        dir->mtime_ns = htole64(time_now_ns());
                        return 0;
                    }
                }
                pos += rec_len;
            }
        }
    }
    return -ENOENT;
}
