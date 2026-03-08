/* io.h - Block I/O, superblock, group descriptors, and misc helpers
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include <sys/stat.h>
#include "partfs.h"

int      block_read(int fd, uint64_t lba, void *buf);
int      block_write(int fd, uint64_t lba, const void *buf);
void     block_crc_set(void *buf);
int      block_hdr_validate(const void *buf, uint32_t expected_magic, uint64_t expected_lba);

uint64_t extent_phys(const struct partfs_extent *e);
void     extent_set_phys(struct partfs_extent *e, uint64_t lba);
uint16_t dirent_rec_len(uint16_t name_len);
uint64_t time_now_ns(void);
mode_t   itype_to_mode(uint16_t itype);
void     inode_to_stat(const struct partfs_inode *inode, struct stat *st);

uint64_t group_base_lba(const struct partfs_state *fs, uint64_t group_no);
int      sb_read(struct partfs_state *fs);
int      sb_write(struct partfs_state *fs);
int      gd_read(struct partfs_state *fs, uint64_t group_no);
int      gd_write(struct partfs_state *fs, uint64_t group_no);
