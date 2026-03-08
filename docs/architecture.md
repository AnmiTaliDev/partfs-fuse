# Architecture

## Overview

partfs-fuse is split into two build artifacts:

- **`libpartfs.so`** — shared library that contains all filesystem logic.
  Compiled with `-fPIC -shared` from everything in `src/` except `main.c`.
- **`partfs`** — thin executable. Parses arguments, opens the device, performs
  startup checks (journal, group descriptors), then calls `fuse_main` with the
  `partfs_ops` table and a pointer to the driver state.

The separation lets `libpartfs.so` be loaded by other tools (test harnesses,
`fsck`, `debugfs`) without pulling in FUSE or the argument parser.

## Layer diagram

```
  userspace process (e.g. cp, vim, shell)
          │ VFS syscalls
  ┌───────▼───────┐
  │  Linux kernel │
  │  FUSE module  │
  └───────┬───────┘
          │ /dev/fuse  ←→  uAPI messages
  ┌───────▼─────────────────────────────┐
  │  partfs (executable)                │
  │  main.c: fuse_main, opt_proc,       │
  │          journal_replay, startup    │
  ├─────────────────────────────────────┤
  │  libpartfs.so                       │
  │                                     │
  │  fuse_ops.c   FUSE dispatch layer   │
  │  ┌──────────────────────────────┐   │
  │  │  dir.c     directory ops     │   │
  │  │  file.c    file data I/O     │   │
  │  │  inode.c   inode management  │   │
  │  │  btree.c   B-tree operations │   │
  │  │  alloc.c   block allocator   │   │
  │  │  io.c      block I/O layer   │   │
  │  │  crc32c.c  checksums / hash  │   │
  │  └──────────────────────────────┘   │
  └─────────────────────────────────────┘
          │ pread / pwrite
  ┌───────▼───────┐
  │  image file   │
  │  or block dev │
  └───────────────┘
```

## Module responsibilities

### `crc32c` — checksums and hashing

Two algorithms, no external dependencies:

- **CRC32C** (Castagnoli, polynomial `0x82F63B78`): computed over full 4096-byte
  blocks (with the `crc32c` field zeroed), and over individual directory records.
  A 256-entry lookup table is built once at startup by `crc32c_init()`.
- **FNV-1a 64-bit**: used as the B-tree key for directory entries. Pure function,
  no initialization required.

### `io` — block I/O and metadata helpers

Thin wrappers over `pread`/`pwrite` that operate in 4096-byte units (one block
per call). Also contains:

- `block_crc_set` / `block_hdr_validate` — CRC management for metadata blocks.
- `sb_read` / `sb_write` — superblock with backup fallback (blocks 0 and 1).
- `gd_read` / `gd_write` — group descriptor with CRC.
- `inode_to_stat` — convert an on-disk inode to a POSIX `struct stat`.
- `extent_phys` / `extent_set_phys` — encode/decode the 48-bit physical LBA
  stored in `partfs_extent.phys_block[6]`.
- `time_now_ns` — current time as nanoseconds since epoch.

### `alloc` — block allocation

Scans the per-group allocation bitmaps to find free blocks. One bit per block,
LSB-first within each byte. Tries the preferred group first, then round-robins
across all groups. Updates `group_desc.free_blocks` and `superblock.free_blocks`
after each allocation or free.

### `btree` — inode B-tree

A classic B-tree stored in 4096-byte blocks:

- **Leaf nodes** (`BTLF`): packed array of 144-byte entries
  (`key(8) + val_size(4) + reserved(4) + inode(128)`), sorted by inode number.
  Up to 27 entries per leaf (`PARTFS_IENTRY_MAX`).
- **Internal nodes** (`BTRE`): interleaved `child(8), key(8)` pairs.
  Up to 253 keys per node (`PARTFS_BTRE_MAX_KEYS`).

Insert is recursive with bottom-up splitting: a full leaf splits into two
leaves; the separator key propagates up to the parent internal node; a full
internal node splits too; if the root splits, a new root is allocated and the
group descriptor's `inode_tree_root` pointer is updated.

Update (`ibtree_update`) traverses the tree to find the leaf containing the
inode and overwrites the 128-byte inode data in place.

### `inode` — inode management

Thin wrappers over `btree`:

- `inode_lookup` — calls `ibtree_lookup` on group 0's tree root.
- `inode_write` — calls `ibtree_update`.
- `inode_alloc` — increments `superblock.inode_count` and returns the new number
  as the inode number. Does not write a B-tree entry; that is done by the caller
  with `ibtree_insert`.
- `inode_map_block` — linear scan of the inline extent array to translate a
  logical block number to a physical LBA.

### `dir` — directory operations

A directory is stored as one or more data blocks, each formatted as a B-tree
leaf block (`BTLF`) containing packed `partfs_dirent` records sorted by FNV-1a
hash. Deletion uses tombstoning (`inode_no = 0`). CRC32C is computed over each
individual directory record.

- `dir_lookup` — scans all directory blocks for a matching hash + exact name.
- `dir_iter` — iterates all live entries, calling a callback.
- `dir_add` — appends to the last block if space allows; allocates a new block
  otherwise.
- `dir_remove` — finds the entry and zeros its `inode_no` (tombstone).

The callback type `dir_iter_fn` is defined in `dir.h` to avoid pulling FUSE
headers into the directory module. The FUSE-specific `readdir_cb` wrapper lives
in `fuse_ops.c`.

### `file` — file data I/O

`file_read_data` and `file_write_data` handle:

- **Inline data** (`IFLAG_INLINE`): data is stored directly in `inode.tail`
  after extents and xattr. Used for short symlink targets.
- **Sparse holes**: logical blocks not covered by any extent return zeroes on
  read.
- **Extent-based data**: maps each logical block through `inode_map_block`,
  reads/writes the corresponding physical block.
- **Allocation on write**: if the target logical block has no physical mapping,
  `block_alloc` is called. If the new block is physically contiguous with the
  last extent, it is merged (extent length incremented). Otherwise a new extent
  entry is appended. Returns `ENOSPC` if the inode already has 4 extents and the
  new block is not contiguous.

### `fuse_ops` — FUSE dispatch

Implements all entries in `struct fuse_operations`. Each handler:

1. Acquires the global mutex (`fs->lock`).
2. Resolves the path to an inode using `path_walk` or `path_walk_parent`.
3. Calls the appropriate lower-level functions.
4. Releases the mutex and returns.

`path_walk` splits the path on `/` and calls `dir_lookup` at each component.
`path_walk_parent` extracts the final component name and resolves the parent.

The `partfs_ops` table is declared `const` and exported from `libpartfs.so`.
`main.c` passes it directly to `fuse_main`.

### `main` — entry point

Responsibilities:
1. Initialize CRC32C table.
2. Parse arguments with `fuse_opt_parse`, extracting the device path.
3. Open the device file with `O_RDWR`.
4. Read the superblock.
5. Read journal headers from blocks 2 and 3 (with fallback).
6. If journal state is not clean, replay committed transactions and reset state.
7. Calculate `groups_start` and load all group descriptors.
8. Call `fuse_main`.
9. Close the device and free resources.

## Concurrency model

A single `pthread_mutex_t` in `partfs_state` serializes all FUSE operations.
This is safe but limits throughput to one operation at a time. Per-inode locking
is planned for a future version.

## State lifetime

`partfs_state` is stack-allocated in `main` and passed as FUSE private data.
Its address remains valid for the entire lifetime of the mount because `main`
blocks in `fuse_main`. The group descriptor array (`fs->groups`) is heap-allocated
and freed after `fuse_main` returns.
