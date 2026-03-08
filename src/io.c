/* io.c - Block I/O, superblock, group descriptors, and misc helpers
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "partfs.h"
#include "crc32c.h"
#include "io.h"

int block_read(int fd, uint64_t lba, void *buf)
{
    ssize_t n = pread(fd, buf, PARTFS_BLOCK_SIZE, (off_t)(lba * PARTFS_BLOCK_SIZE));
    if (n != PARTFS_BLOCK_SIZE)
        return -EIO;
    return 0;
}

int block_write(int fd, uint64_t lba, const void *buf)
{
    ssize_t n = pwrite(fd, buf, PARTFS_BLOCK_SIZE, (off_t)(lba * PARTFS_BLOCK_SIZE));
    if (n != PARTFS_BLOCK_SIZE)
        return -EIO;
    return 0;
}

void block_crc_set(void *buf)
{
    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    hdr->crc32c = 0;
    uint32_t crc = crc32c_compute(0, buf, PARTFS_BLOCK_SIZE);
    hdr->crc32c = htole32(crc);
}

int block_hdr_validate(const void *buf, uint32_t expected_magic, uint64_t expected_lba)
{
    const struct partfs_block_hdr *hdr = (const struct partfs_block_hdr *)buf;
    if (le32toh(hdr->magic) != expected_magic)
        return -EIO;
    if (le64toh(hdr->block_no) != expected_lba)
        return -EIO;
    uint8_t tmp[PARTFS_BLOCK_SIZE];
    memcpy(tmp, buf, PARTFS_BLOCK_SIZE);
    ((struct partfs_block_hdr *)tmp)->crc32c = 0;
    uint32_t computed = crc32c_compute(0, tmp, PARTFS_BLOCK_SIZE);
    if (computed != le32toh(hdr->crc32c))
        return -EIO;
    return 0;
}

uint64_t extent_phys(const struct partfs_extent *e)
{
    return (uint64_t)e->phys_block[0]
         | ((uint64_t)e->phys_block[1] << 8)
         | ((uint64_t)e->phys_block[2] << 16)
         | ((uint64_t)e->phys_block[3] << 24)
         | ((uint64_t)e->phys_block[4] << 32)
         | ((uint64_t)e->phys_block[5] << 40);
}

void extent_set_phys(struct partfs_extent *e, uint64_t lba)
{
    e->phys_block[0] = (uint8_t)(lba >> 0);
    e->phys_block[1] = (uint8_t)(lba >> 8);
    e->phys_block[2] = (uint8_t)(lba >> 16);
    e->phys_block[3] = (uint8_t)(lba >> 24);
    e->phys_block[4] = (uint8_t)(lba >> 32);
    e->phys_block[5] = (uint8_t)(lba >> 40);
}

uint16_t dirent_rec_len(uint16_t name_len)
{
    return (uint16_t)(((PARTFS_DIRENT_HDR_SIZE + name_len + 3) / 4) * 4);
}

uint64_t time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

mode_t itype_to_mode(uint16_t itype)
{
    switch (itype) {
    case PARTFS_ITYPE_FILE:    return S_IFREG;
    case PARTFS_ITYPE_DIR:     return S_IFDIR;
    case PARTFS_ITYPE_SYMLINK: return S_IFLNK;
    default:                   return 0;
    }
}

void inode_to_stat(const struct partfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino     = (ino_t)le64toh(inode->inode_no);
    st->st_mode    = itype_to_mode(le16toh(inode->inode_type))
                   | (le16toh(inode->mode) & 07777);
    st->st_nlink   = (nlink_t)le32toh(inode->refcount);
    st->st_uid     = (uid_t)le32toh(inode->uid);
    st->st_gid     = (gid_t)le32toh(inode->gid);
    st->st_size    = (off_t)le64toh(inode->size);
    st->st_blocks  = (blkcnt_t)(le64toh(inode->blocks_used) * PARTFS_BLOCK_SIZE / 512);
    st->st_blksize = PARTFS_BLOCK_SIZE;

    uint64_t mtime_ns = le64toh(inode->mtime_ns);
    st->st_mtim.tv_sec  = (time_t)(mtime_ns / 1000000000ULL);
    st->st_mtim.tv_nsec = (long)(mtime_ns % 1000000000ULL);
    st->st_ctim = st->st_mtim;
    st->st_atim = st->st_mtim;
}

uint64_t group_base_lba(const struct partfs_state *fs, uint64_t group_no)
{
    return fs->groups_start + group_no * le32toh(fs->sb.group_size);
}

static int sb_crc_valid(const struct partfs_superblock *sb)
{
    return crc32c_compute(0, sb, 124) == le32toh(sb->crc32c);
}

static void sb_crc_update(struct partfs_superblock *sb)
{
    sb->crc32c = htole32(crc32c_compute(0, sb, 124));
}

int sb_read(struct partfs_state *fs)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];

    if (block_read(fs->fd, 0, buf) < 0)
        return -EIO;

    struct partfs_superblock *sb = (struct partfs_superblock *)buf;
    if (le32toh(sb->magic) != PARTFS_MAGIC_SB) {
        if (block_read(fs->fd, 1, buf) < 0)
            return -EIO;
        if (le32toh(sb->magic) != PARTFS_MAGIC_SB)
            return -EINVAL;
    }

    if (!sb_crc_valid(sb)) {
        if (block_read(fs->fd, 1, buf) < 0)
            return -EIO;
        if (!sb_crc_valid(sb))
            return -EIO;
    }

    memcpy(&fs->sb, buf, sizeof(struct partfs_superblock));
    return 0;
}

int sb_write(struct partfs_state *fs)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    sb_crc_update(&fs->sb);
    memcpy(buf, &fs->sb, sizeof(struct partfs_superblock));
    if (block_write(fs->fd, 0, buf) < 0)
        return -EIO;
    if (block_write(fs->fd, 1, buf) < 0)
        return -EIO;
    return 0;
}

int gd_read(struct partfs_state *fs, uint64_t group_no)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    uint64_t lba = group_base_lba(fs, group_no);
    if (block_read(fs->fd, lba, buf) < 0)
        return -EIO;
    if (block_hdr_validate(buf, PARTFS_MAGIC_GRPD, lba) < 0)
        return -EIO;
    memcpy(&fs->groups[group_no], buf, sizeof(struct partfs_group_desc));
    return 0;
}

int gd_write(struct partfs_state *fs, uint64_t group_no)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];
    uint64_t lba = group_base_lba(fs, group_no);
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &fs->groups[group_no], sizeof(struct partfs_group_desc));
    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    hdr->magic    = htole32(PARTFS_MAGIC_GRPD);
    hdr->block_no = htole64(lba);
    block_crc_set(buf);
    return block_write(fs->fd, lba, buf);
}
