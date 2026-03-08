# Driver Internals

This document describes the implementation of each module in detail, covering
data flows, design choices, and edge cases.

## crc32c — checksums and hashing

### CRC32C

Polynomial `0x82F63B78` (Castagnoli, reflected). Implemented with a 256-entry
lookup table built at startup by `crc32c_init()`. The initial CRC value passed
to `crc32c_compute` is always `0`; the function handles the `~crc` / `~result`
inversion internally.

Block CRC workflow:

1. Zero the `hdr.crc32c` field.
2. Call `crc32c_compute(0, block, 4096)`.
3. Store the result in `hdr.crc32c`.

Validation reverses this: zero the field, compute, compare with stored value.

Superblock CRC covers only bytes 0x00–0x7B (124 bytes) — the `crc32c` field at
0x7C is outside the covered range and does not need to be zeroed.

Directory record CRC covers the full variable-length record (including the
padding bytes at the end).

### FNV-1a 64-bit

```
offset_basis = 14695981039346656037ULL
prime        = 1099511628211ULL
```

No initialization. Hash collisions in the directory B-tree are resolved by
comparing the full filename after a hash match.

## io — block I/O layer

All I/O uses `pread`/`pwrite` at offset `lba × 4096`. Block-layer errors are
mapped to `-EIO`.

`sb_read` tries block 0 first. If the magic is wrong or the CRC fails, it falls
back to block 1. Both copies are always written together by `sb_write`.

`gd_read` validates both the magic and the CRC. A mismatch aborts the mount.

`inode_to_stat` fills all `struct stat` fields. It does not set `st_atime`
separately — all three time fields are set to `mtime`. `st_blocks` is in
512-byte units (POSIX convention), computed as `blocks_used × 8`.

## alloc — block allocation

The allocator scans bitmaps starting from the preferred group. The preferred
group for new blocks defaults to 0; file write passes the group of the last
existing extent to improve locality.

Each call to `block_alloc` or `block_free` writes back the group descriptor and
superblock to keep `free_blocks` counts consistent on disk. This is one write per
allocation — not batched.

The bitmap layout is 4 blocks per group = 131072 addressable bits, enough for
groups up to 131072 blocks even though the current group size is 32768.

## btree — inode B-tree

### Node format

Internal node interleaves children and keys:

```
child[0]  key[0]  child[1]  key[1]  ...  key[n-1]  child[n]
```

Each child and key is 8 bytes, so entry `i` occupies:
- `child[i]` at offset `24 + i×16`
- `key[i]` at offset `24 + i×16 + 8`
- `child[n]` at offset `24 + n×16`

`PARTFS_BTLEAF_HDR_SIZE` (24) is reused for internal nodes since both headers
have the same size.

### Lookup

Recursive descent: at an internal node, find the first key greater than
`target_ino` to select the child index; at a leaf, linear scan for the matching
key. Returns `-ENOENT` if not found.

### Insert

`ibtree_insert_r` recurses. At a leaf, `ibtree_leaf_insert` either inserts
in-place (if space allows) or splits:

1. Allocate a new leaf block.
2. Merge all existing entries with the new one into a temporary buffer sorted
   by key.
3. Write the first `PARTFS_IENTRY_SPLIT` (13) entries to the existing leaf.
4. Write the remaining entries to the new leaf.
5. Return the new leaf's first key and LBA as the split separator.

On split, the caller (internal node handler) inserts the separator key and new
child pointer into the internal node. If the internal node is also full, it
splits too, propagating the separator upward.

At the top level (`ibtree_insert`), if the root splits, a new root internal node
is created and `groups[0].inode_tree_root` is updated.

### Update

`ibtree_update` traverses to the leaf containing the inode and overwrites the
128-byte data region in place. The key does not change (inode numbers are
immutable). Returns `-ENOENT` if the inode is not found.

### B-tree and deleted inodes

Deleted inodes are marked with `inode_type = PARTFS_ITYPE_DELETED` and updated
in place. Their B-tree entries are never removed. This avoids the complexity of
B-tree deletion and rebalancing at the cost of accumulating tombstones.

## inode — inode management

`inode_alloc` does not insert a B-tree entry — it only allocates a number
(incrementing `inode_count`). The caller must call `ibtree_insert` to add the
actual inode data. This separation allows the caller to fill in the inode fields
before inserting.

`inode_map_block` does a linear scan of up to 4 extents. For each extent it
checks whether `logical_blk` falls in `[lb, lb+len)`. Returns 0 if no extent
covers the block (sparse hole).

## dir — directory operations

### Block format

Directory blocks share the `BTLF` header format to allow future upgrade to a
proper directory B-tree with internal nodes. Currently only leaf blocks are used.

### Hash collisions

`dir_lookup` checks hash first, then does an exact `memcmp` on the name. Two
different filenames with the same FNV-1a hash will coexist correctly in the same
directory block, both found by their respective lookups.

### Adding entries

`dir_add` checks the last directory block of the inode for space. Space is
measured by `dir_block_used`, which sums `rec_len` of all entries from the
block header. If the new entry fits, it is appended and the `entry_count` is
incremented. If not, a new block is allocated and added as a new or extended
extent.

### Tombstoning

`dir_remove` sets `inode_no = 0` and recomputes the record CRC. The record
remains in place, occupying its original `rec_len` bytes. Iteration skips
entries with `inode_no == 0`.

### Dot entries

`mkdir` pre-populates the new directory block with `.` (pointing to itself) and
`..` (pointing to the parent). These are written directly without going through
`dir_add`.

## file — file data I/O

### Read path

1. If `IFLAG_INLINE`: read from `inode.tail` offset (after extents and xattr).
2. For each byte range: compute logical block number, call `inode_map_block`.
3. If physical LBA is 0 (sparse hole): zero-fill the output buffer.
4. Otherwise: read the physical block and copy the relevant slice.
5. Stop at `file_size` even if `size` requests more.

### Write path

1. For each byte range: compute logical block number, call `inode_map_block`.
2. If the logical block is not mapped: call `block_alloc`.
   - Try to extend the last extent: if the new LBA equals `last_phys + last_len`
     and the logical block equals `last_logical + last_len`, increment
     `last->length`.
   - Otherwise: add a new extent entry. If all 4 extent slots are used and
     extension is not possible, free the newly allocated block and return
     `ENOSPC`.
3. If the block is already mapped: read it first (read-modify-write).
4. Copy the data slice into the block buffer and write.
5. Update `inode.size` if the write extends past the current EOF.
6. Update `inode.mtime_ns`.

The caller (`partfs_write` in `fuse_ops.c`) writes the updated inode back after
`file_write_data` returns.

## fuse_ops — FUSE dispatch

### Global lock

All FUSE operations acquire `fs->lock` at entry and release at exit. The mutex
is a plain `pthread_mutex_t` (non-recursive). Operations do not call each other
while holding the lock; they use the lower-level functions directly.

### Path resolution

`path_walk` splits the path on `/` with `strtok_r` and calls `dir_lookup` at
each component, following the inode chain. It does not resolve symbolic links —
FUSE handles symlink resolution in the kernel before calling the driver for
most operations.

`path_walk_parent` extracts the last `/`-separated component as the name and
resolves everything before it with `path_walk`.

### `partfs_open`

Stores the inode number in `fi->fh`. Subsequent `read`/`write`/`truncate` calls
use `fi->fh` directly, bypassing another path walk. Checks:
- Deleted inodes → `ENOENT`
- Directories → `EISDIR`
- `IFLAG_IMMUTABLE` with write access → `EPERM`
- `O_TRUNC` with `IFLAG_IMMUTABLE` or `IFLAG_APPEND` → `EPERM`

### `partfs_create`

Allocates a new inode number, fills the inode struct, inserts it into the B-tree,
then calls `dir_add` to add the directory entry. Sets `fi->fh` to the new inode
number so the file is open immediately.

### `partfs_unlink`

Decrements `refcount`. If it reaches zero, all extent blocks are freed and
`inode_type` is set to `PARTFS_ITYPE_DELETED`. The directory entry is
tombstoned with `dir_remove`.

### `partfs_link`

Hard links: increments the target inode's `refcount`, then calls `dir_add` in
the destination directory. Refuses to link directories (`EPERM`).

### `partfs_rename`

Handles `RENAME_NOREPLACE` and `RENAME_EXCHANGE` flags. For a standard rename:
1. If destination exists, free it (decrement refcount or delete).
2. `dir_add` the source inode under the new name.
3. `dir_remove` the source entry.

### `partfs_mkdir`

Allocates a data block, writes it as a `BTLF` block with `.` and `..` entries,
allocates an inode, inserts it, adds a directory entry in the parent, increments
the parent's `refcount`.

### `partfs_truncate`

Shrink: iterates extents, frees blocks beyond the new EOF, compacts the extent
array (removes zero-length extents).

Extend: calls `file_write_data` with a zero buffer in `PARTFS_BLOCK_SIZE`
chunks to allocate and zero-fill blocks up to the new size.

### `partfs_fsync`

Calls `fdatasync(fs->fd)` if `datasync != 0`, otherwise `fsync(fs->fd)`.
The file descriptor points to the image file or block device.

### `partfs_init` / `partfs_destroy`

`init`: clears `FLAG_CLEAN`, increments `mount_count`, writes the superblock.
`destroy`: sets `FLAG_CLEAN`, writes the superblock. If the process is killed
unexpectedly, `destroy` does not run and the clean flag remains unset, causing
journal replay on the next mount.

## main — startup and journal replay

### Startup sequence

1. `crc32c_init()` — build lookup table.
2. `fuse_opt_parse` — extract device path.
3. `open(device, O_RDWR)`.
4. `sb_read` — read and validate superblock.
5. `journal_hdr_read` — try block 2, fall back to block 3.
6. Journal replay if dirty (see below).
7. Compute `groups_start = journal_start + journal_blocks`.
8. Compute `num_groups`.
9. `gd_read` for each group.
10. `fuse_main`.

### Journal replay

The journal is a ring buffer of `journal_size` data blocks starting at LBA
`journal_start`. Sequence numbers index positions: block at sequence `S` occupies
ring position `S % journal_size`.

Replay iterates sequence numbers from `seq_tail` to `seq_head`:

- For each slot, read the journal block.
- If the block magic is `COMT`: write all blocks collected since the last commit
  (or since `seq_tail`) to their original LBAs (given by `hdr.block_no`), then
  clear the pending list.
- If the block magic is any other non-zero value: it is a logged data block;
  add it to the pending list.
- If the block is zeroed or unreadable: discard pending and stop.

After successful replay, the journal state is reset to `PARTFS_JOURNAL_CLEAN`
and `seq_tail` is advanced to `seq_head`. Both journal header copies (blocks 2
and 3) are written.
