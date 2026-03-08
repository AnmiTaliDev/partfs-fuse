/* btree.c - Inode B-tree operations
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <errno.h>
#include <endian.h>

#include "partfs.h"
#include "io.h"
#include "alloc.h"
#include "btree.h"

static uint64_t btre_get_child(const uint8_t *buf, uint32_t idx)
{
    uint64_t val;
    memcpy(&val, buf + PARTFS_BTLEAF_HDR_SIZE + (size_t)idx * 16, 8);
    return le64toh(val);
}

static uint64_t btre_get_key(const uint8_t *buf, uint32_t idx)
{
    uint64_t val;
    memcpy(&val, buf + PARTFS_BTLEAF_HDR_SIZE + (size_t)idx * 16 + 8, 8);
    return le64toh(val);
}

static void btre_set_child(uint8_t *buf, uint32_t idx, uint64_t lba)
{
    uint64_t val = htole64(lba);
    memcpy(buf + PARTFS_BTLEAF_HDR_SIZE + (size_t)idx * 16, &val, 8);
}

static void btre_set_key(uint8_t *buf, uint32_t idx, uint64_t key)
{
    uint64_t val = htole64(key);
    memcpy(buf + PARTFS_BTLEAF_HDR_SIZE + (size_t)idx * 16 + 8, &val, 8);
}

int ibtree_lookup(struct partfs_state *fs, uint64_t node_lba,
                  uint64_t target_ino, struct partfs_inode *out)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    if (block_read(fs->fd, node_lba, buf) < 0)
        return -EIO;

    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    uint32_t magic = le32toh(hdr->magic);

    if (magic == PARTFS_MAGIC_BTRE) {
        struct partfs_btree_internal_hdr *ihdr = (struct partfs_btree_internal_hdr *)buf;
        uint32_t n = le32toh(ihdr->entry_count);
        uint32_t child_idx = n;
        for (uint32_t i = 0; i < n; i++) {
            if (target_ino < btre_get_key(buf, i)) {
                child_idx = i;
                break;
            }
        }
        return ibtree_lookup(fs, btre_get_child(buf, child_idx), target_ino, out);
    }

    if (magic == PARTFS_MAGIC_BTLF) {
        struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
        uint32_t count   = le32toh(lhdr->entry_count);
        uint8_t *entries = buf + PARTFS_BTLEAF_HDR_SIZE;

        for (uint32_t i = 0; i < count; i++) {
            uint64_t key;
            memcpy(&key, entries + (size_t)i * PARTFS_IENTRY_SIZE, 8);
            if (le64toh(key) == target_ino) {
                memcpy(out, entries + (size_t)i * PARTFS_IENTRY_SIZE + 16,
                       sizeof(struct partfs_inode));
                return 0;
            }
        }
        return -ENOENT;
    }

    return -EIO;
}

static void ientry_write(uint8_t *dst, uint64_t ino, const struct partfs_inode *inode)
{
    uint64_t key_le = htole64(ino);
    uint32_t vsz_le = htole32(sizeof(struct partfs_inode));
    uint32_t rsvd   = 0;
    memcpy(dst,      &key_le, 8);
    memcpy(dst + 8,  &vsz_le, 4);
    memcpy(dst + 12, &rsvd,   4);
    memcpy(dst + 16, inode,   sizeof(struct partfs_inode));
}

/* Returns 0 (inserted), 1 (leaf split), or -errno.
 * On split: *split_key and *split_lba receive the separator and new leaf LBA. */
static int ibtree_leaf_insert(struct partfs_state *fs, uint64_t leaf_lba,
                               uint64_t ino, const struct partfs_inode *inode,
                               uint64_t *split_key, uint64_t *split_lba)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    if (block_read(fs->fd, leaf_lba, buf) < 0)
        return -EIO;

    struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
    uint32_t count   = le32toh(lhdr->entry_count);
    uint8_t *entries = buf + PARTFS_BTLEAF_HDR_SIZE;

    if (count < (uint32_t)PARTFS_IENTRY_MAX) {
        uint32_t pos = count;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t key;
            memcpy(&key, entries + (size_t)i * PARTFS_IENTRY_SIZE, 8);
            if (le64toh(key) > ino) {
                pos = i;
                break;
            }
        }
        if (pos < count) {
            memmove(entries + ((size_t)pos + 1) * PARTFS_IENTRY_SIZE,
                    entries + (size_t)pos * PARTFS_IENTRY_SIZE,
                    (count - pos) * PARTFS_IENTRY_SIZE);
        }
        ientry_write(entries + (size_t)pos * PARTFS_IENTRY_SIZE, ino, inode);
        lhdr->entry_count = htole32(count + 1);
        block_crc_set(buf);
        return block_write(fs->fd, leaf_lba, buf);
    }

    /* Leaf full: split */
    int64_t new_lba = block_alloc(fs, 0);
    if (new_lba < 0)
        return (int)new_lba;

    uint32_t split = PARTFS_IENTRY_SPLIT;
    uint8_t all[(PARTFS_IENTRY_MAX + 1) * PARTFS_IENTRY_SIZE];
    uint32_t total  = 0;
    int inserted    = 0;

    for (uint32_t i = 0; i <= count; i++) {
        if (!inserted) {
            uint64_t cur_key = UINT64_MAX;
            if (i < count) {
                memcpy(&cur_key, entries + (size_t)i * PARTFS_IENTRY_SIZE, 8);
                cur_key = le64toh(cur_key);
            }
            if (ino < cur_key) {
                ientry_write(all + (size_t)total * PARTFS_IENTRY_SIZE, ino, inode);
                total++;
                inserted = 1;
            }
        }
        if (i < count) {
            memcpy(all + (size_t)total * PARTFS_IENTRY_SIZE,
                   entries + (size_t)i * PARTFS_IENTRY_SIZE,
                   PARTFS_IENTRY_SIZE);
            total++;
        }
    }
    if (!inserted) {
        ientry_write(all + (size_t)total * PARTFS_IENTRY_SIZE, ino, inode);
        total++;
    }

    memset(entries, 0, (size_t)PARTFS_IENTRY_MAX * PARTFS_IENTRY_SIZE);
    memcpy(entries, all, (size_t)split * PARTFS_IENTRY_SIZE);
    lhdr->entry_count = htole32(split);
    block_crc_set(buf);
    if (block_write(fs->fd, leaf_lba, buf) < 0)
        return -EIO;

    uint32_t new_count = total - split;
    uint8_t new_buf[PARTFS_BLOCK_SIZE];
    memset(new_buf, 0, sizeof(new_buf));
    struct partfs_btree_leaf_hdr *new_lhdr = (struct partfs_btree_leaf_hdr *)new_buf;
    new_lhdr->hdr.magic    = htole32(PARTFS_MAGIC_BTLF);
    new_lhdr->hdr.block_no = htole64((uint64_t)new_lba);
    new_lhdr->entry_count  = htole32(new_count);
    memcpy(new_buf + PARTFS_BTLEAF_HDR_SIZE,
           all + (size_t)split * PARTFS_IENTRY_SIZE,
           (size_t)new_count * PARTFS_IENTRY_SIZE);
    block_crc_set(new_buf);
    if (block_write(fs->fd, (uint64_t)new_lba, new_buf) < 0)
        return -EIO;

    uint64_t sep;
    memcpy(&sep, new_buf + PARTFS_BTLEAF_HDR_SIZE, 8);
    *split_key = le64toh(sep);
    *split_lba = (uint64_t)new_lba;
    return 1;
}

static int ibtree_insert_r(struct partfs_state *fs, uint64_t node_lba,
                            uint64_t ino, const struct partfs_inode *inode,
                            uint64_t *split_key, uint64_t *split_lba)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    if (block_read(fs->fd, node_lba, buf) < 0)
        return -EIO;

    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    uint32_t magic = le32toh(hdr->magic);

    if (magic == PARTFS_MAGIC_BTLF)
        return ibtree_leaf_insert(fs, node_lba, ino, inode, split_key, split_lba);

    if (magic == PARTFS_MAGIC_BTRE) {
        struct partfs_btree_internal_hdr *ihdr = (struct partfs_btree_internal_hdr *)buf;
        uint32_t n = le32toh(ihdr->entry_count);

        uint32_t child_idx = n;
        for (uint32_t i = 0; i < n; i++) {
            if (ino < btre_get_key(buf, i)) {
                child_idx = i;
                break;
            }
        }

        uint64_t sk = 0, sl = 0;
        int r = ibtree_insert_r(fs, btre_get_child(buf, child_idx),
                                 ino, inode, &sk, &sl);
        if (r <= 0)
            return r;

        if (n < (uint32_t)PARTFS_BTRE_MAX_KEYS) {
            for (uint32_t i = n; i > child_idx; i--) {
                btre_set_key(buf, i, btre_get_key(buf, i - 1));
                btre_set_child(buf, i + 1, btre_get_child(buf, i));
            }
            btre_set_key(buf, child_idx, sk);
            btre_set_child(buf, child_idx + 1, sl);
            ihdr->entry_count = htole32(n + 1);
            block_crc_set(buf);
            return block_write(fs->fd, node_lba, buf);
        }

        /* Internal node full: split it */
        int64_t new_internal_lba = block_alloc(fs, 0);
        if (new_internal_lba < 0)
            return (int)new_internal_lba;

        uint32_t total_keys = n + 1;
        uint64_t keys[PARTFS_BTRE_MAX_KEYS + 1];
        uint64_t children[PARTFS_BTRE_MAX_KEYS + 2];

        for (uint32_t i = 0; i < n; i++) {
            keys[i < child_idx ? i : i + 1]         = btre_get_key(buf, i);
            children[i <= child_idx ? i : i + 1]    = btre_get_child(buf, i);
        }
        children[n]        = btre_get_child(buf, n);
        keys[child_idx]    = sk;
        children[child_idx + 1] = sl;

        uint32_t mid = total_keys / 2;
        *split_key = keys[mid];

        memset(buf + PARTFS_BTLEAF_HDR_SIZE, 0,
               PARTFS_BLOCK_SIZE - PARTFS_BTLEAF_HDR_SIZE);
        ihdr->entry_count = htole32(mid);
        for (uint32_t i = 0; i < mid; i++) {
            btre_set_child(buf, i, children[i]);
            btre_set_key(buf, i, keys[i]);
        }
        btre_set_child(buf, mid, children[mid]);
        block_crc_set(buf);
        if (block_write(fs->fd, node_lba, buf) < 0)
            return -EIO;

        uint8_t new_buf[PARTFS_BLOCK_SIZE];
        memset(new_buf, 0, sizeof(new_buf));
        struct partfs_btree_internal_hdr *new_ihdr = (struct partfs_btree_internal_hdr *)new_buf;
        new_ihdr->hdr.magic    = htole32(PARTFS_MAGIC_BTRE);
        new_ihdr->hdr.block_no = htole64((uint64_t)new_internal_lba);
        uint32_t new_n = total_keys - mid - 1;
        new_ihdr->entry_count = htole32(new_n);
        for (uint32_t i = 0; i < new_n; i++) {
            btre_set_child(new_buf, i, children[mid + 1 + i]);
            btre_set_key(new_buf, i, keys[mid + 1 + i]);
        }
        btre_set_child(new_buf, new_n, children[mid + 1 + new_n]);
        block_crc_set(new_buf);
        if (block_write(fs->fd, (uint64_t)new_internal_lba, new_buf) < 0)
            return -EIO;

        *split_lba = (uint64_t)new_internal_lba;
        return 1;
    }

    return -EIO;
}

int ibtree_insert(struct partfs_state *fs, uint64_t ino,
                  const struct partfs_inode *inode)
{
    uint64_t root_lba = le64toh(fs->groups[0].inode_tree_root);
    uint64_t split_key = 0, split_lba = 0;
    int r = ibtree_insert_r(fs, root_lba, ino, inode, &split_key, &split_lba);
    if (r < 0)
        return r;
    if (r == 1) {
        int64_t new_root_lba = block_alloc(fs, 0);
        if (new_root_lba < 0)
            return (int)new_root_lba;

        uint8_t buf[PARTFS_BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        struct partfs_btree_internal_hdr *ihdr = (struct partfs_btree_internal_hdr *)buf;
        ihdr->hdr.magic    = htole32(PARTFS_MAGIC_BTRE);
        ihdr->hdr.block_no = htole64((uint64_t)new_root_lba);
        ihdr->entry_count  = htole32(1);
        btre_set_child(buf, 0, root_lba);
        btre_set_key(buf, 0, split_key);
        btre_set_child(buf, 1, split_lba);
        block_crc_set(buf);
        if (block_write(fs->fd, (uint64_t)new_root_lba, buf) < 0)
            return -EIO;

        fs->groups[0].inode_tree_root = htole64((uint64_t)new_root_lba);
        gd_write(fs, 0);
    }
    return 0;
}

int ibtree_update(struct partfs_state *fs, uint64_t node_lba,
                  const struct partfs_inode *inode)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    if (block_read(fs->fd, node_lba, buf) < 0)
        return -EIO;

    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    uint32_t magic = le32toh(hdr->magic);

    if (magic == PARTFS_MAGIC_BTRE) {
        struct partfs_btree_internal_hdr *ihdr = (struct partfs_btree_internal_hdr *)buf;
        uint32_t n      = le32toh(ihdr->entry_count);
        uint64_t target = le64toh(inode->inode_no);
        uint32_t child_idx = n;
        for (uint32_t i = 0; i < n; i++) {
            if (target < btre_get_key(buf, i)) {
                child_idx = i;
                break;
            }
        }
        return ibtree_update(fs, btre_get_child(buf, child_idx), inode);
    }

    if (magic == PARTFS_MAGIC_BTLF) {
        struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
        uint32_t count   = le32toh(lhdr->entry_count);
        uint8_t *entries = buf + PARTFS_BTLEAF_HDR_SIZE;
        uint64_t target  = le64toh(inode->inode_no);

        for (uint32_t i = 0; i < count; i++) {
            uint64_t key;
            memcpy(&key, entries + (size_t)i * PARTFS_IENTRY_SIZE, 8);
            if (le64toh(key) == target) {
                memcpy(entries + (size_t)i * PARTFS_IENTRY_SIZE + 16,
                       inode, sizeof(struct partfs_inode));
                block_crc_set(buf);
                return block_write(fs->fd, node_lba, buf);
            }
        }
        return -ENOENT;
    }

    return -EIO;
}
