# libpartfs API Reference

`libpartfs.so` exposes the complete PartFS filesystem driver as a shared library.
The intended consumers are `partfs` (the FUSE mountpoint binary), `mkfs.part`,
`fsck.part`, and any other userspace tool that needs to read or write PartFS volumes.

## Using the library

### pkg-config

```sh
gcc $(pkg-config --cflags libpartfs) myprogram.c \
    $(pkg-config --libs libpartfs) -o myprogram
```

### Headers

After installation, all headers live under `<partfs/>`:

```c
#include <partfs/partfs.h>   /* on-disk structs and constants */
#include <partfs/crc32c.h>   /* CRC32C and FNV-1a */
#include <partfs/io.h>       /* block I/O, superblock, group descriptors */
#include <partfs/alloc.h>    /* block allocation */
#include <partfs/btree.h>    /* inode B-tree */
#include <partfs/inode.h>    /* inode management */
#include <partfs/dir.h>      /* directory operations */
#include <partfs/file.h>     /* file data I/O */
#include <partfs/fuse_ops.h> /* FUSE operations table */
```

When building from source without installation, add `-Iinclude -Isrc` to CFLAGS
and include the headers without the `partfs/` prefix.

### Initialization sequence

```c
crc32c_init();                  /* build CRC table — call once */

struct partfs_state fs = {0};
fs.fd = open("disk.img", O_RDWR);
pthread_mutex_init(&fs.lock, NULL);

sb_read(&fs);                   /* load superblock */
/* compute groups_start, then: */
fs.groups = calloc(fs.num_groups, sizeof(struct partfs_group_desc));
for (uint64_t i = 0; i < fs.num_groups; i++)
    gd_read(&fs, i);

/* use the API ... */

free(fs.groups);
close(fs.fd);
pthread_mutex_destroy(&fs.lock);
```

### Thread safety

No function is thread-safe on its own. All callers must hold `fs->lock` around
every call. The FUSE driver acquires this mutex in each FUSE operation handler.

### Error codes

All functions that can fail return a negative `errno` value (`-EIO`, `-ENOENT`,
`-ENOSPC`, etc.) or a negative value from the standard C error table on failure,
and `0` (or a positive value) on success. Return values specific to each function
are listed in the sections below.

---

## `partfs_state` — driver state

Defined in `include/partfs.h`. Must be initialized to zero before use.

```c
struct partfs_state {
    int                      fd;           /* open file descriptor (O_RDWR) */
    struct partfs_superblock sb;           /* in-memory superblock copy */
    uint64_t                 groups_start; /* LBA of first block group */
    uint64_t                 num_groups;   /* number of block groups */
    struct partfs_group_desc *groups;      /* heap-allocated array, num_groups entries */
    pthread_mutex_t          lock;         /* caller must hold for every API call */
};
```

`groups_start` is computed from the superblock:
```c
fs.groups_start = le64toh(fs.sb.journal_start) + le32toh(fs.sb.journal_blocks);
```

`num_groups` is typically computed as:
```c
uint64_t data_blocks = le64toh(fs.sb.block_count) - fs.groups_start;
uint32_t group_size  = le32toh(fs.sb.group_size);
fs.num_groups = (data_blocks + group_size - 1) / group_size;
```

---

## Module: crc32c

Header: `<partfs/crc32c.h>`

### `crc32c_init`

```c
void crc32c_init(void);
```

Builds the 256-entry CRC32C lookup table. Must be called once before any call
to `crc32c_compute`. Subsequent calls are safe but redundant. Not thread-safe
if called concurrently with `crc32c_compute`.

### `crc32c_compute`

```c
uint32_t crc32c_compute(uint32_t crc, const void *buf, size_t len);
```

Computes CRC32C (Castagnoli, polynomial `0x82F63B78`) over `len` bytes of `buf`,
starting from the running value `crc`.

**Parameters:**
- `crc` — initial value; pass `0` for a fresh computation.
- `buf` — pointer to data; must not be NULL.
- `len` — byte count; `0` is valid and returns `crc` unchanged.

**Returns:** the updated CRC32C value.

**Usage — computing a block checksum:**
```c
/* Zero the crc32c field first, then compute over the full block */
struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)block;
hdr->crc32c = 0;
uint32_t crc = crc32c_compute(0, block, PARTFS_BLOCK_SIZE);
hdr->crc32c = htole32(crc);
```

**Usage — superblock CRC (covers only bytes 0x00–0x7B):**
```c
uint32_t crc = crc32c_compute(0, sb, 124);
```

### `fnv1a_64`

```c
uint64_t fnv1a_64(const uint8_t *data, size_t len);
```

Computes the FNV-1a 64-bit hash of `len` bytes of `data`.
Uses offset basis `14695981039346656037` and prime `1099511628211`.

**Parameters:**
- `data` — pointer to bytes; must not be NULL even if `len` is 0.
- `len` — byte count.

**Returns:** 64-bit hash value.

**Usage — directory entry key:**
```c
uint64_t hash = fnv1a_64((const uint8_t *)name, strlen(name));
```

---

## Module: io

Header: `<partfs/io.h>`

### `block_read`

```c
int block_read(int fd, uint64_t lba, void *buf);
```

Reads one 4096-byte block from `fd` at logical block address `lba` into `buf`.
Uses `pread(2)` internally; the call is atomic and restartable.

**Parameters:**
- `fd` — file descriptor opened with `O_RDWR` or `O_RDONLY`.
- `lba` — zero-based block number.
- `buf` — output buffer; must be at least `PARTFS_BLOCK_SIZE` bytes.

**Returns:** `0` on success, `-EIO` if `pread` returns a short read or error.

### `block_write`

```c
int block_write(int fd, uint64_t lba, const void *buf);
```

Writes one 4096-byte block from `buf` to `fd` at logical block address `lba`.

**Parameters:**
- `fd` — file descriptor opened with `O_RDWR`.
- `lba` — zero-based block number.
- `buf` — source buffer; must be exactly `PARTFS_BLOCK_SIZE` bytes.

**Returns:** `0` on success, `-EIO` on short write or error.

### `block_crc_set`

```c
void block_crc_set(void *buf);
```

Computes the CRC32C of the full 4096-byte block at `buf` (after zeroing
`hdr.crc32c`) and stores the result in `hdr.crc32c` in little-endian order.

The block must begin with a `struct partfs_block_hdr`.

**Usage:**
```c
lhdr->hdr.magic    = htole32(PARTFS_MAGIC_BTLF);
lhdr->hdr.block_no = htole64(lba);
/* ... fill payload ... */
block_crc_set(buf);
block_write(fd, lba, buf);
```

### `block_hdr_validate`

```c
int block_hdr_validate(const void *buf, uint32_t expected_magic, uint64_t expected_lba);
```

Validates a metadata block: checks that `hdr.magic == expected_magic`,
`hdr.block_no == expected_lba`, and that the CRC32C of the full block matches
`hdr.crc32c`.

**Parameters:**
- `buf` — pointer to a 4096-byte block.
- `expected_magic` — one of `PARTFS_MAGIC_*`.
- `expected_lba` — the LBA at which this block should reside.

**Returns:** `0` on success, `-EIO` if any check fails.

### `extent_phys`

```c
uint64_t extent_phys(const struct partfs_extent *e);
```

Decodes the 48-bit little-endian physical LBA stored in `e->phys_block[6]`
and returns it as a `uint64_t`.

### `extent_set_phys`

```c
void extent_set_phys(struct partfs_extent *e, uint64_t lba);
```

Encodes `lba` as a 48-bit little-endian value into `e->phys_block[6]`.
The upper 16 bits of `lba` are silently discarded; volumes larger than 256 TiB
are not supported.

### `dirent_rec_len`

```c
uint16_t dirent_rec_len(uint16_t name_len);
```

Computes the total record length for a directory entry with the given filename
length, rounded up to a 4-byte boundary:

```
rec_len = ((PARTFS_DIRENT_HDR_SIZE + name_len + 3) / 4) * 4
```

**Returns:** record length in bytes (minimum 36 for a 1-byte name).

### `time_now_ns`

```c
uint64_t time_now_ns(void);
```

Returns the current wall-clock time as nanoseconds since the Unix epoch,
obtained via `clock_gettime(CLOCK_REALTIME)`.

### `itype_to_mode`

```c
mode_t itype_to_mode(uint16_t itype);
```

Converts a PartFS inode type code to the corresponding POSIX `S_IF*` constant:

| `itype` | Returns |
|---|---|
| `PARTFS_ITYPE_FILE` | `S_IFREG` |
| `PARTFS_ITYPE_DIR` | `S_IFDIR` |
| `PARTFS_ITYPE_SYMLINK` | `S_IFLNK` |
| anything else | `0` |

### `inode_to_stat`

```c
void inode_to_stat(const struct partfs_inode *inode, struct stat *st);
```

Fills a `struct stat` from an in-memory inode. Zeroes `st` first. Sets:
`st_ino`, `st_mode` (type + permission bits including setuid/setgid/sticky),
`st_nlink`, `st_uid`, `st_gid`, `st_size`, `st_blocks` (512-byte units),
`st_blksize` (`PARTFS_BLOCK_SIZE`), `st_atim`, `st_mtim`, `st_ctim`
(all set to `mtime_ns`; atime and ctime are not stored separately).

### `group_base_lba`

```c
uint64_t group_base_lba(const struct partfs_state *fs, uint64_t group_no);
```

Returns the LBA of the first block (group descriptor) of block group `group_no`.

```
base = fs->groups_start + group_no * group_size
```

### `sb_read`

```c
int sb_read(struct partfs_state *fs);
```

Reads the superblock from the volume and validates it. Tries block 0 first
(magic check + CRC32C). Falls back to block 1 if block 0 fails either check.
Stores the validated superblock in `fs->sb`.

**Returns:** `0` on success, `-EIO` if CRC fails on both copies, `-EINVAL`
if magic is missing on both copies.

### `sb_write`

```c
int sb_write(struct partfs_state *fs);
```

Recomputes the superblock CRC and writes `fs->sb` to both blocks 0 and 1
(padded with zeros to `PARTFS_BLOCK_SIZE`).

**Returns:** `0` on success, `-EIO` if either write fails.

### `gd_read`

```c
int gd_read(struct partfs_state *fs, uint64_t group_no);
```

Reads and validates the group descriptor for `group_no` from disk. Stores the
result in `fs->groups[group_no]`. Validates magic and CRC via `block_hdr_validate`.

**Parameters:**
- `group_no` — zero-based group index; must be `< fs->num_groups`.

**Returns:** `0` on success, `-EIO` on read error or validation failure.

### `gd_write`

```c
int gd_write(struct partfs_state *fs, uint64_t group_no);
```

Writes `fs->groups[group_no]` to disk, setting the block header magic
(`PARTFS_MAGIC_GRPD`) and `block_no`, then computing CRC.

**Returns:** `0` on success, `-EIO` on write failure.

---

## Module: alloc

Header: `<partfs/alloc.h>`

### `bitmap_get`

```c
int bitmap_get(struct partfs_state *fs, uint64_t group_no, uint64_t bit);
```

Reads the allocation bitmap for `group_no` and returns the state of `bit`.
Bit `i` is in byte `i/8`, LSB-first.

**Parameters:**
- `group_no` — zero-based group index.
- `bit` — block offset within the group (0-based).

**Returns:** `1` if allocated, `0` if free, `-EIO` on read error.

### `bitmap_set`

```c
int bitmap_set(struct partfs_state *fs, uint64_t group_no, uint64_t bit, int val);
```

Sets or clears `bit` in the allocation bitmap for `group_no` and writes the
bitmap block back to disk.

**Parameters:**
- `val` — `1` to mark allocated, `0` to mark free.

**Returns:** `0` on success, `-EIO` on I/O error.

**Note:** Does not update `free_blocks` counters. Use `block_alloc` / `block_free`
for full allocation/free including counter updates.

### `block_alloc`

```c
int64_t block_alloc(struct partfs_state *fs, uint64_t preferred_group);
```

Allocates one free block. Scans from `preferred_group`, then round-robins
across all groups until a free block is found. Sets the bitmap bit, decrements
`group_desc.free_blocks` and `superblock.free_blocks`, and writes both back to
disk via `gd_write` and `sb_write`.

**Parameters:**
- `preferred_group` — hint for locality; allocation will still succeed in any
  other group if the preferred one is full.

**Returns:** the allocated block's absolute LBA as `int64_t` (always positive),
or `-ENOSPC` if no free blocks exist anywhere, or `-EIO` on I/O error.

**Usage:**
```c
int64_t lba = block_alloc(fs, 0);
if (lba < 0) { /* handle -ENOSPC or -EIO */ }
/* use (uint64_t)lba as the physical block address */
```

### `block_free`

```c
int block_free(struct partfs_state *fs, uint64_t lba);
```

Frees the block at absolute LBA `lba`. Computes the group number and bit
index from the LBA, clears the bitmap bit, increments `free_blocks` counters,
and writes back group descriptor and superblock.

**Parameters:**
- `lba` — absolute LBA; must be `>= fs->groups_start`.

**Returns:** `0` on success, `-EINVAL` if `lba` maps to a non-existent group,
`-EIO` on I/O error.

---

## Module: btree

Header: `<partfs/btree.h>`

The inode B-tree stores `struct partfs_inode` records keyed by inode number.
The tree root LBA is held in `fs->groups[0].inode_tree_root`. Internal nodes
have magic `PARTFS_MAGIC_BTRE`; leaf nodes have magic `PARTFS_MAGIC_BTLF`.

### `ibtree_lookup`

```c
int ibtree_lookup(struct partfs_state *fs, uint64_t node_lba,
                  uint64_t target_ino, struct partfs_inode *out);
```

Recursively searches the B-tree rooted at `node_lba` for inode number
`target_ino`. Descends through internal nodes by key comparison; performs a
linear scan at leaf nodes.

**Parameters:**
- `node_lba` — LBA of the root node (typically `fs->groups[0].inode_tree_root`).
- `target_ino` — inode number to find.
- `out` — on success, the inode data is copied here.

**Returns:** `0` on success, `-ENOENT` if not found, `-EIO` on I/O error or
corrupt block magic.

**Note:** Callers should use `inode_lookup` instead of calling this directly.
`inode_lookup` automatically passes the correct root LBA.

### `ibtree_insert`

```c
int ibtree_insert(struct partfs_state *fs, uint64_t ino,
                  const struct partfs_inode *inode);
```

Inserts a new inode entry into the B-tree. The tree root is taken from
`fs->groups[0].inode_tree_root`.

If a leaf is full (`PARTFS_IENTRY_MAX` = 27 entries), it is split: entries are
divided at the split point (`PARTFS_IENTRY_SPLIT` = 13), a new leaf is
allocated, and the separator key propagates up to the parent internal node.
If the internal node is also full (`PARTFS_BTRE_MAX_KEYS` = 253 keys), it
splits too. If the root splits, a new root internal node is allocated and
`fs->groups[0].inode_tree_root` is updated; the group descriptor is written
back to disk.

**Parameters:**
- `ino` — inode number (the B-tree key); must not already exist in the tree.
- `inode` — inode data to store; `inode->inode_no` must equal `ino`.

**Returns:** `0` on success, `-ENOSPC` if block allocation fails, `-EIO` on I/O error.

**Warning:** Inserting a duplicate key produces undefined behaviour. Check with
`ibtree_lookup` first if existence is uncertain.

### `ibtree_update`

```c
int ibtree_update(struct partfs_state *fs, uint64_t node_lba,
                  const struct partfs_inode *inode);
```

Finds the B-tree entry whose key equals `inode->inode_no` (starting from
`node_lba`) and overwrites the 128-byte inode data in-place. Does not modify
the key or the B-tree structure.

**Parameters:**
- `node_lba` — LBA of the root node.
- `inode` — new inode data; `inode->inode_no` is used as the search key.

**Returns:** `0` on success, `-ENOENT` if the inode is not found, `-EIO` on error.

**Note:** Callers should use `inode_write` instead of calling this directly.

---

## Module: inode

Header: `<partfs/inode.h>`

High-level wrappers over `btree` that always operate on group 0's tree.

### `inode_lookup`

```c
int inode_lookup(struct partfs_state *fs, uint64_t ino, struct partfs_inode *out);
```

Finds inode `ino` in the B-tree and copies it to `out`.

**Returns:** `0` on success, `-ENOENT` if not found, `-EIO` on error.

**Note:** A returned inode with `inode_type == PARTFS_ITYPE_DELETED` is a
tombstone; callers should treat it as `ENOENT`.

### `inode_write`

```c
int inode_write(struct partfs_state *fs, const struct partfs_inode *inode);
```

Updates an existing inode in the B-tree. `inode->inode_no` identifies the entry.
The block containing the inode is read, modified, and written back with an
updated CRC.

**Returns:** `0` on success, `-ENOENT` if the inode does not exist in the tree,
`-EIO` on error.

### `inode_alloc`

```c
int inode_alloc(struct partfs_state *fs, uint64_t *out_ino);
```

Allocates a new inode number by incrementing `fs->sb.inode_count` and writing
the superblock. Does **not** insert a B-tree entry.

After calling `inode_alloc`, the caller must fill a `struct partfs_inode` and
call `ibtree_insert` to actually add the inode to the tree.

**Parameters:**
- `out_ino` — on success, receives the newly allocated inode number.

**Returns:** `0` on success, `-EIO` if the superblock write fails.

**Example:**
```c
uint64_t ino;
inode_alloc(fs, &ino);

struct partfs_inode new_inode = {0};
new_inode.inode_type = htole16(PARTFS_ITYPE_FILE);
new_inode.mode       = htole16(0644);
new_inode.inode_no   = htole64(ino);
new_inode.refcount   = htole32(1);
/* ... fill other fields ... */

ibtree_insert(fs, ino, &new_inode);
```

### `inode_map_block`

```c
uint64_t inode_map_block(const struct partfs_inode *inode, uint64_t logical_blk);
```

Translates a logical block number to a physical LBA by scanning the inode's
extent array. Does not perform I/O.

**Parameters:**
- `inode` — in-memory inode; scans `inode->tail` for extents.
- `logical_blk` — logical block number (0-based from file start).

**Returns:** physical LBA if the block is covered by an extent, `0` if the
block is a sparse hole (not covered by any extent).

---

## Module: dir

Header: `<partfs/dir.h>`

Directories are stored as one or more 4096-byte data blocks, each formatted as
a `BTLF` leaf block containing packed `struct partfs_dirent` records.

### `dir_lookup`

```c
int dir_lookup(struct partfs_state *fs, const struct partfs_inode *dir,
               const char *name, uint64_t *out_ino, uint16_t *out_type);
```

Searches all data blocks of `dir` for a live directory entry matching `name`.
Computes the FNV-1a 64-bit hash of `name` and scans blocks for matching hash,
then verifies with `memcmp`.

**Parameters:**
- `dir` — inode of the directory to search; must have `inode_type == PARTFS_ITYPE_DIR`.
- `name` — null-terminated filename to search for.
- `out_ino` — on success, receives the target inode number.
- `out_type` — on success, receives the target inode type (`PARTFS_ITYPE_*`).

**Returns:** `0` on success, `-ENOENT` if not found, `-EIO` on read error.

### `dir_add`

```c
int dir_add(struct partfs_state *fs, struct partfs_inode *dir,
            const char *name, uint64_t ino, uint16_t itype);
```

Appends a new directory entry for `name → ino`. Tries to fit the new record
into the last directory block. If the block is full, allocates a new data block
and adds it as an extent (or extends the last extent if physically contiguous).

Computes FNV-1a hash and CRC32C for the new record. Updates `dir->size`,
`dir->blocks_used` (if a new block is allocated), and `dir->mtime_ns`.
Does **not** call `inode_write` — the caller must persist the modified `dir` inode.

**Parameters:**
- `dir` — directory inode to modify (in-memory; caller writes it back).
- `name` — null-terminated filename; maximum 255 bytes.
- `ino` — inode number of the target.
- `itype` — inode type of the target (`PARTFS_ITYPE_FILE`, `PARTFS_ITYPE_DIR`, etc.).

**Returns:** `0` on success, `-ENAMETOOLONG` if `name` exceeds 255 bytes,
`-ENOSPC` if block allocation fails, `-EIO` on I/O error.

### `dir_remove`

```c
int dir_remove(struct partfs_state *fs, struct partfs_inode *dir,
               const char *name);
```

Marks the directory entry for `name` as a tombstone by setting `inode_no = 0`
and rewriting the record with an updated CRC32C. The record bytes remain on disk
and continue to occupy space.

Updates `dir->mtime_ns`. Does not call `inode_write`.

**Returns:** `0` on success, `-ENOENT` if `name` is not found, `-EIO` on error.

### `dir_iter_fn`

```c
typedef int (*dir_iter_fn)(void *arg, const char *name, size_t name_len,
                            uint64_t ino, uint16_t itype);
```

Callback type for `dir_iter`. Called once per live (non-tombstone) directory
entry.

**Parameters delivered to callback:**
- `arg` — the opaque pointer passed to `dir_iter`.
- `name` — filename bytes (not null-terminated); valid only for the duration
  of the callback.
- `name_len` — byte length of `name`.
- `ino` — target inode number.
- `itype` — target inode type.

**Return value from callback:**
- `0` — continue iteration.
- Any non-zero value — stop iteration; `dir_iter` returns this value.

### `dir_iter`

```c
int dir_iter(struct partfs_state *fs, const struct partfs_inode *dir,
             dir_iter_fn fn, void *arg);
```

Iterates all live (non-tombstone) entries in `dir`, calling `fn` for each.
Tombstones (`inode_no == 0`) are skipped. Iteration order matches the physical
layout of records across directory blocks and is not sorted.

**Parameters:**
- `dir` — directory inode; must have `inode_type == PARTFS_ITYPE_DIR`.
- `fn` — callback function.
- `arg` — opaque argument forwarded to every `fn` call.

**Returns:** `0` if all entries were iterated, the non-zero value returned by
`fn` if iteration was stopped early, or `-EIO` on read error.

**Example — collecting entry names:**
```c
struct collect_ctx { char **names; int count; };

static int collect_cb(void *arg, const char *name, size_t nlen,
                      uint64_t ino, uint16_t itype)
{
    struct collect_ctx *ctx = arg;
    ctx->names[ctx->count] = strndup(name, nlen);
    ctx->count++;
    return 0;
}

struct collect_ctx ctx = { .names = buf, .count = 0 };
dir_iter(fs, &dir_inode, collect_cb, &ctx);
```

---

## Module: file

Header: `<partfs/file.h>`

### `file_read_data`

```c
ssize_t file_read_data(struct partfs_state *fs,
                        const struct partfs_inode *inode,
                        void *buf, size_t size, off_t offset);
```

Reads up to `size` bytes from the file described by `inode`, starting at byte
position `offset`, into `buf`.

Handles three storage modes transparently:
- **Inline** (`IFLAG_INLINE` set): data is read from `inode->tail` (after
  extents and xattr). Used for short symlink targets.
- **Sparse holes**: logical blocks with no physical mapping return zeroed bytes.
- **Extent-mapped**: uses `inode_map_block` to find physical blocks and calls
  `block_read`.

The read is clamped to `inode->size`; bytes beyond EOF are not returned.

**Parameters:**
- `inode` — in-memory inode; not modified.
- `buf` — output buffer; must be at least `size` bytes.
- `size` — maximum bytes to read.
- `offset` — byte offset from the start of the file.

**Returns:** number of bytes read (0 at EOF, less than `size` at EOF),
or `-EIO` on read error. Does not return `-EINVAL` for out-of-bounds `offset`
— returns `0` instead.

### `file_write_data`

```c
ssize_t file_write_data(struct partfs_state *fs,
                         struct partfs_inode *inode,
                         const void *buf, size_t size, off_t offset);
```

Writes `size` bytes from `buf` to the file at byte position `offset`.
Allocates physical blocks as needed. May extend the file.

For each logical block touched:
1. If the block is already mapped, read it first (read-modify-write).
2. If not mapped, call `block_alloc`. If the new physical block is contiguous
   with the last extent, extend that extent's length; otherwise append a new
   extent. Returns `-ENOSPC` if all 4 extent slots are used and the new block
   is not contiguous.

Updates `inode->size` if the write extends past the current EOF.
Updates `inode->mtime_ns`. Does **not** call `inode_write` — the caller must
persist the modified inode.

**Parameters:**
- `inode` — in-memory inode; modified in-place (extent array, size, mtime).
- `buf` — source buffer; must be at least `size` bytes.
- `size` — byte count to write.
- `offset` — byte offset from the start of the file.

**Returns:** number of bytes written (always equals `size` on full success,
may be less if `-ENOSPC` is hit mid-write), or a negative errno on error
(`-ENOSPC`, `-EIO`). If some bytes were written before the error, returns the
positive count, not the error code.

**Caller must persist the inode:**
```c
ssize_t n = file_write_data(fs, &inode, buf, size, offset);
if (n > 0)
    inode_write(fs, &inode);
```

---

## Module: fuse_ops

Header: `<partfs/fuse_ops.h>`

Defines `FUSE_USE_VERSION 31` and declares:

```c
extern const struct fuse_operations partfs_ops;
```

`partfs_ops` is the complete `struct fuse_operations` table implementing the
FUSE 3 interface for PartFS. Pass it to `fuse_main`:

```c
#include <partfs/fuse_ops.h>
/* ... set up fs ... */
return fuse_main(args.argc, args.argv, &partfs_ops, &fs);
```

Implemented operations and their semantics:

| Field | Handler | Notes |
|---|---|---|
| `.init` | `partfs_init` | Clears `FLAG_CLEAN`, increments `mount_count` |
| `.destroy` | `partfs_destroy` | Sets `FLAG_CLEAN` |
| `.getattr` | `partfs_getattr` | Uses `fi->fh` if available |
| `.readdir` | `partfs_readdir` | Calls `dir_iter` with a FUSE-specific callback |
| `.open` | `partfs_open` | Stores inode number in `fi->fh`; checks immutability |
| `.read` | `partfs_read` | Uses `fi->fh`; delegates to `file_read_data` |
| `.write` | `partfs_write` | Enforces `IFLAG_IMMUTABLE` / `IFLAG_APPEND` |
| `.flush` | `partfs_flush` | No-op |
| `.fsync` | `partfs_fsync` | Calls `fdatasync` or `fsync` on the image fd |
| `.create` | `partfs_create` | Allocates inode + inserts into B-tree + adds dirent |
| `.mkdir` | `partfs_mkdir` | Allocates data block + inode; writes `.` and `..` |
| `.unlink` | `partfs_unlink` | Decrements refcount; frees blocks at zero |
| `.rmdir` | `partfs_rmdir` | Checks empty; frees data blocks; tombstones inode |
| `.rename` | `partfs_rename` | Handles `RENAME_NOREPLACE`, `RENAME_EXCHANGE` |
| `.link` | `partfs_link` | Increments refcount; adds dirent |
| `.chmod` | `partfs_chmod` | Updates `inode.mode` (12 permission bits) |
| `.chown` | `partfs_chown` | Updates `inode.uid` / `inode.gid` |
| `.truncate` | `partfs_truncate` | Shrinks or extends with zeros |
| `.utimens` | `partfs_utimens` | Updates `inode.mtime_ns`; honours `UTIME_NOW`/`UTIME_OMIT` |
| `.readlink` | `partfs_readlink` | Reads via `file_read_data` |
| `.symlink` | `partfs_symlink` | Short targets stored inline; long targets use extents |
| `.statfs` | `partfs_statfs` | Reports block/inode counts from superblock |
| `.getxattr` | `partfs_getxattr` | Inline xattr in inode tail |
| `.setxattr` | `partfs_setxattr` | Inline; enforces `XATTR_CREATE` / `XATTR_REPLACE` |
| `.listxattr` | `partfs_listxattr` | Returns null-terminated name list |
| `.removexattr` | `partfs_removexattr` | Tombstone-free removal via memmove |

All handlers acquire `fs->lock` on entry and release it before returning.

---

## Constants

All constants are defined in `include/partfs.h`.

### Block and group geometry

| Constant | Value | Description |
|---|---|---|
| `PARTFS_BLOCK_SIZE` | 4096 | Block size in bytes |
| `PARTFS_GROUP_SIZE` | 32768 | Blocks per group |
| `PARTFS_BITMAP_BLOCKS` | 4 | Bitmap blocks per group |
| `PARTFS_JOURNAL_DEFAULT` | 1024 | Default journal size in blocks |
| `PARTFS_GRP_META_G0` | 7 | Metadata blocks used in group 0 |
| `PARTFS_GRP_META_GN` | 5 | Metadata blocks used in groups 1+ |

### Magic values

| Constant | Value | Meaning |
|---|---|---|
| `PARTFS_MAGIC_SB` | `0x53465450` | Superblock |
| `PARTFS_MAGIC_BTRE` | `0x42545245` | B-tree internal node |
| `PARTFS_MAGIC_BTLF` | `0x42544C46` | B-tree leaf node |
| `PARTFS_MAGIC_JRNL` | `0x4A524E4C` | Journal header |
| `PARTFS_MAGIC_GRPD` | `0x47525044` | Group descriptor |
| `PARTFS_MAGIC_COMT` | `0x434F4D54` | Journal commit block |

### Superblock flags

| Constant | Bit | Meaning |
|---|---|---|
| `PARTFS_FLAG_CLEAN` | 0 | Cleanly unmounted |
| `PARTFS_FLAG_JOURNAL` | 1 | Journal present |
| `PARTFS_FLAG_CHECKSUMS` | 2 | CRC32C enabled |
| `PARTFS_FLAG_EXTENTS` | 3 | Extent storage (always set) |
| `PARTFS_FLAG_XATTR` | 4 | Extended attributes |
| `PARTFS_FLAG_ACL` | 5 | POSIX ACL via xattr |

### Inode type codes

| Constant | Value | Meaning |
|---|---|---|
| `PARTFS_ITYPE_FILE` | `0x0001` | Regular file |
| `PARTFS_ITYPE_DIR` | `0x0002` | Directory |
| `PARTFS_ITYPE_SYMLINK` | `0x0003` | Symbolic link |
| `PARTFS_ITYPE_DELETED` | `0x0004` | Tombstone |

### Inode flags

| Constant | Bit | Meaning |
|---|---|---|
| `PARTFS_IFLAG_INLINE` | 0 | Data stored inline in `tail` |
| `PARTFS_IFLAG_SPARSE` | 1 | File has sparse holes |
| `PARTFS_IFLAG_IMMUTABLE` | 2 | Cannot be modified or deleted |
| `PARTFS_IFLAG_APPEND` | 3 | Append-only |

### Journal state codes

| Constant | Value | Meaning |
|---|---|---|
| `PARTFS_JOURNAL_CLEAN` | 0 | Journal is clean |
| `PARTFS_JOURNAL_DIRTY` | 1 | Journal has uncommitted data |
| `PARTFS_JOURNAL_REPLAY` | 2 | Replay in progress |

### B-tree limits

| Constant | Value | Description |
|---|---|---|
| `PARTFS_BTLEAF_HDR_SIZE` | 24 | Leaf (and internal) block header size |
| `PARTFS_IENTRY_SIZE` | 144 | Size of one inode leaf entry |
| `PARTFS_IENTRY_MAX` | 27 | Maximum entries per leaf block |
| `PARTFS_IENTRY_SPLIT` | 13 | Split point for leaf nodes |
| `PARTFS_BTRE_MAX_KEYS` | 253 | Maximum keys per internal node |

### Inode limits

| Constant | Value | Description |
|---|---|---|
| `PARTFS_MAX_EXTENTS` | 4 | Maximum inline extents per inode |
| `PARTFS_DIRENT_HDR_SIZE` | 32 | Fixed part of a directory record |

---

## On-disk structures

All structures are defined in `include/partfs.h` and are packed
(`__attribute__((packed))`). All multi-byte integers are little-endian.

See [on-disk-format.md](on-disk-format.md) for field-level layout tables.

- `struct partfs_block_hdr` — 16 bytes
- `struct partfs_superblock` — 128 bytes (verified by `_Static_assert`)
- `struct partfs_group_desc` — variable padded to 4096 bytes on disk
- `struct partfs_extent` — 16 bytes (verified by `_Static_assert`)
- `struct partfs_inode` — 128 bytes (verified by `_Static_assert`)
- `struct partfs_btree_leaf_hdr` — 24 bytes (verified by `_Static_assert`)
- `struct partfs_btree_internal_hdr` — 24 bytes
- `struct partfs_dirent` — 32 bytes fixed header + variable name (verified by `_Static_assert`)
- `struct partfs_journal_hdr` — variable padded to 4096 bytes on disk
