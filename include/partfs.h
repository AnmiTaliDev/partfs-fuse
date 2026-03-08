/* partfs.h - PartFS on-disk structures, constants, and driver state
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include <pthread.h>

#define PARTFS_BLOCK_SIZE       4096
#define PARTFS_GROUP_SIZE       32768
#define PARTFS_BITMAP_BLOCKS    4
#define PARTFS_JOURNAL_DEFAULT  1024

/* Superblock magic (reads PTFS in a little-endian hexdump: bytes 50 54 46 53) */
#define PARTFS_MAGIC_SB         0x53465450U

/* Block-type magic values */
#define PARTFS_MAGIC_BTRE       0x42545245U  /* B-tree internal node */
#define PARTFS_MAGIC_BTLF       0x42544C46U  /* B-tree leaf node */
#define PARTFS_MAGIC_JRNL       0x4A524E4CU  /* Journal header */
#define PARTFS_MAGIC_GRPD       0x47525044U  /* Group descriptor */
#define PARTFS_MAGIC_COMT       0x434F4D54U  /* Journal commit */

/* Superblock flags */
#define PARTFS_FLAG_CLEAN       (1U << 0)
#define PARTFS_FLAG_JOURNAL     (1U << 1)
#define PARTFS_FLAG_CHECKSUMS   (1U << 2)
#define PARTFS_FLAG_EXTENTS     (1U << 3)
#define PARTFS_FLAG_XATTR       (1U << 4)
#define PARTFS_FLAG_ACL         (1U << 5)

/* Inode type codes */
#define PARTFS_ITYPE_FILE       0x0001
#define PARTFS_ITYPE_DIR        0x0002
#define PARTFS_ITYPE_SYMLINK    0x0003
#define PARTFS_ITYPE_DELETED    0x0004

/* Inode flags */
#define PARTFS_IFLAG_INLINE     (1U << 0)
#define PARTFS_IFLAG_SPARSE     (1U << 1)
#define PARTFS_IFLAG_IMMUTABLE  (1U << 2)
#define PARTFS_IFLAG_APPEND     (1U << 3)

/* Metadata block counts per group after mkfs */
#define PARTFS_GRP_META_G0      7
#define PARTFS_GRP_META_GN      5

/* Max extents stored inline in inode tail (tail = 64 bytes, each extent = 16 bytes) */
#define PARTFS_MAX_EXTENTS      4

/* B-tree leaf header offset where entries begin */
#define PARTFS_BTLEAF_HDR_SIZE  24

/* Size of one inode B-tree leaf entry: key(8)+val_size(4)+reserved(4)+inode(128) */
#define PARTFS_IENTRY_SIZE      144

/* Max inode entries per leaf block */
#define PARTFS_IENTRY_MAX       ((PARTFS_BLOCK_SIZE - PARTFS_BTLEAF_HDR_SIZE) / PARTFS_IENTRY_SIZE)

/* Split point for leaf nodes */
#define PARTFS_IENTRY_SPLIT     (PARTFS_IENTRY_MAX / 2)

/* Max keys in a B-tree internal node:
 * layout: child0(8), key0(8), child1(8), ..., key(n-1)(8), child_n(8)
 * total data: (2*n+1)*8 bytes, must fit in BLOCK_SIZE - 24
 * max n = (4096-24-8)/16 = 253 */
#define PARTFS_BTRE_MAX_KEYS    253

/* Fixed-size part of a directory record */
#define PARTFS_DIRENT_HDR_SIZE  32

struct partfs_block_hdr {
    uint32_t magic;
    uint32_t crc32c;
    uint64_t block_no;
} __attribute__((packed));

struct partfs_superblock {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t block_count;
    uint64_t free_blocks;
    uint64_t journal_start;
    uint32_t journal_blocks;
    uint32_t group_size;
    uint64_t root_inode;
    uint64_t inode_count;
    uint8_t  uuid[16];
    uint8_t  label[32];
    uint64_t mkfs_time;
    uint64_t mount_count;
    uint32_t flags;
    uint32_t crc32c;
} __attribute__((packed));

_Static_assert(sizeof(struct partfs_superblock) == 128, "superblock must be 128 bytes");

struct partfs_group_desc {
    struct partfs_block_hdr hdr;
    uint64_t group_no;
    uint64_t bitmap_start;
    uint64_t inode_tree_root;
    uint64_t data_start;
    uint64_t free_blocks;
    uint64_t total_blocks;
    uint32_t flags;
} __attribute__((packed));

struct partfs_extent {
    uint64_t logical_block;
    uint8_t  phys_block[6];
    uint16_t length;
} __attribute__((packed));

_Static_assert(sizeof(struct partfs_extent) == 16, "extent must be 16 bytes");

struct partfs_inode {
    uint16_t inode_type;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t refcount;
    uint64_t size;
    uint64_t blocks_used;
    uint64_t crtime_ns;
    uint64_t mtime_ns;
    uint64_t inode_no;
    uint16_t extent_count;
    uint16_t xattr_len;
    uint16_t inline_len;
    uint16_t flags;
    uint8_t  tail[64];
} __attribute__((packed));

_Static_assert(sizeof(struct partfs_inode) == 128, "inode must be 128 bytes");

struct partfs_btree_leaf_hdr {
    struct partfs_block_hdr hdr;
    uint32_t entry_count;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct partfs_btree_leaf_hdr) == 24, "btree leaf hdr must be 24 bytes");

struct partfs_btree_internal_hdr {
    struct partfs_block_hdr hdr;
    uint32_t entry_count;
    uint32_t reserved;
} __attribute__((packed));

struct partfs_dirent {
    uint64_t inode_no;
    uint64_t name_hash;
    uint16_t name_len;
    uint16_t rec_len;
    uint16_t inode_type;
    uint16_t reserved;
    uint32_t crc32c;
    uint32_t padding;
} __attribute__((packed));

_Static_assert(sizeof(struct partfs_dirent) == 32, "dirent fixed part must be 32 bytes");

struct partfs_journal_hdr {
    struct partfs_block_hdr hdr;
    uint64_t seq_head;
    uint64_t seq_tail;
    uint64_t journal_start;
    uint32_t journal_size;
    uint32_t state;
} __attribute__((packed));

/* Journal header state values */
#define PARTFS_JOURNAL_CLEAN    0
#define PARTFS_JOURNAL_DIRTY    1
#define PARTFS_JOURNAL_REPLAY   2

struct partfs_state {
    int fd;
    struct partfs_superblock sb;
    uint64_t groups_start;
    uint64_t num_groups;
    struct partfs_group_desc *groups;
    pthread_mutex_t lock;
};
