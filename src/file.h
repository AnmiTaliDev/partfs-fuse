/* file.h - File data read/write using extents
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdint.h>
#include <sys/types.h>
#include "partfs.h"

ssize_t file_read_data(struct partfs_state *fs, const struct partfs_inode *inode,
                        void *buf, size_t size, off_t offset);

ssize_t file_write_data(struct partfs_state *fs, struct partfs_inode *inode,
                         const void *buf, size_t size, off_t offset);
