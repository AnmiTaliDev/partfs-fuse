/* fuse_ops.h - FUSE operation table for PartFS
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

extern const struct fuse_operations partfs_ops;
