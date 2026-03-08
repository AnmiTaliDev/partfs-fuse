# On-Disk Format Reference

All multi-byte integers are little-endian. The block size is 4096 bytes (fixed).

## Volume layout

```
LBA 0        Superblock (primary)
LBA 1        Superblock (backup, identical)
LBA 2        Journal header A
LBA 3        Journal header B (identical content, block_no differs)
LBA 4 .. 4+J-1   Journal data blocks  (J = journal_blocks, default 1024)
LBA 4+J ..        Block groups (G = 32768 blocks each)
```

Groups start at LBA `journal_start + journal_blocks` (always `4 + J`).
The last group may be smaller than G.

Minimum volume size: `J + 11` blocks. For the default journal: 1035 blocks (~4 MiB).

## Block header (16 bytes)

Every metadata block begins with this header. Raw file data blocks do not carry it.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0x00 | 4 | `magic` | Block-type magic (see table below) |
| 0x04 | 4 | `crc32c` | CRC32C of full 4096-byte block; zeroed during calculation |
| 0x08 | 8 | `block_no` | Self-referencing LBA; detects misplaced blocks |

Magic values:

| Value | Block type |
|---|---|
| `0x42545245` | B-tree internal node (`BTRE`) |
| `0x42544C46` | B-tree leaf node (`BTLF`) |
| `0x4A524E4C` | Journal header (`JRNL`) |
| `0x47525044` | Group descriptor (`GRPD`) |
| `0x434F4D54` | Journal commit block (`COMT`) |

## Superblock (128 bytes, LBA 0 and 1)

The CRC32C covers bytes `0x00`–`0x7B` (124 bytes). The `crc32c` field at `0x7C`
is outside this range and does not need to be zeroed before calculation.

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 4 | u32 | `magic` | `0x53465450` (reads `PTFS` in a LE hexdump) |
| 0x04 | 2 | u16 | `version_major` | `1` |
| 0x06 | 2 | u16 | `version_minor` | `0` |
| 0x08 | 8 | u64 | `block_count` | Total 4096-byte blocks on volume |
| 0x10 | 8 | u64 | `free_blocks` | Free block count |
| 0x18 | 8 | u64 | `journal_start` | LBA of first journal block (`2`) |
| 0x20 | 4 | u32 | `journal_blocks` | Journal size in blocks (default 1024) |
| 0x24 | 4 | u32 | `group_size` | Blocks per group (`32768`) |
| 0x28 | 8 | u64 | `root_inode` | Inode number of root directory (`1`) |
| 0x30 | 8 | u64 | `inode_count` | Total allocated inodes |
| 0x38 | 16 | u8[16] | `uuid` | Volume UUID, RFC 4122 version 4 |
| 0x48 | 32 | u8[32] | `label` | Volume label, UTF-8, null-padded |
| 0x68 | 8 | u64 | `mkfs_time` | Creation timestamp, Unix nanoseconds |
| 0x70 | 8 | u64 | `mount_count` | Total mount count |
| 0x78 | 4 | u32 | `flags` | Feature flags (see below) |
| 0x7C | 4 | u32 | `crc32c` | CRC32C of bytes 0x00–0x7B |

Superblock flags (`flags` field):

| Bit | Name | Description |
|---|---|---|
| 0 | `FLAG_CLEAN` | Volume was unmounted cleanly |
| 1 | `FLAG_JOURNAL` | Journal is present and active |
| 2 | `FLAG_CHECKSUMS` | Per-block CRC32C enabled |
| 3 | `FLAG_EXTENTS` | Extent-based allocation (always set) |
| 4 | `FLAG_XATTR` | Extended attributes enabled |
| 5 | `FLAG_ACL` | POSIX ACL via xattr |

`mkfs.part` sets bits 0–3 (`0x0F`).

## Journal header (LBA 2 and 3)

Both copies are identical; only `hdr.block_no` differs (2 vs 3).

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 16 | — | `hdr` | Block header; magic = `0x4A524E4C` |
| 0x10 | 8 | u64 | `seq_head` | Next write sequence number |
| 0x18 | 8 | u64 | `seq_tail` | Oldest uncommitted sequence |
| 0x20 | 8 | u64 | `journal_start` | LBA of first journal data block (`4`) |
| 0x28 | 4 | u32 | `journal_size` | Journal data area size in blocks |
| 0x2C | 4 | u32 | `state` | `0` = clean, `1` = dirty, `2` = replaying |

Journal data blocks (LBA 4 … 4+J−1) are zeroed at `mkfs` time.

## Block group layout

Group 0:

```
offset 0      Group descriptor   (1 block)
offset 1–4    Allocation bitmap  (4 blocks)
offset 5      Inode B-tree root leaf
offset 6      Root directory data block
offset 7 …    Free data blocks
```

Groups 1+:

```
offset 0      Group descriptor
offset 1–4    Allocation bitmap
offset 5 …    Free data blocks  (no inode B-tree at mkfs time)
```

Metadata block counts: group 0 = 7, groups 1+ = 5.

## Group descriptor (LBA = group_base)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 16 | — | `hdr` | Block header; magic = `0x47525044` |
| 0x10 | 8 | u64 | `group_no` | Zero-based group index |
| 0x18 | 8 | u64 | `bitmap_start` | LBA of first allocation bitmap block |
| 0x20 | 8 | u64 | `inode_tree_root` | LBA of inode B-tree root; `0` for groups 1+ |
| 0x28 | 8 | u64 | `data_start` | LBA of first free data block |
| 0x30 | 8 | u64 | `free_blocks` | Free block count in this group |
| 0x38 | 8 | u64 | `total_blocks` | Total blocks in this group |
| 0x40 | 4 | u32 | `flags` | Group flags (`0` after mkfs) |

## Allocation bitmap

Each group has 4 bitmap blocks (16384 bytes = 131072 bits).
One bit per block: `1` = allocated, `0` = free.
Bit `i` is in byte `i/8`, bit position `i mod 8` (LSB-first).

After mkfs: group 0 has bits 0–6 set; groups 1+ have bits 0–4 set.

## Inode (128 bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 2 | u16 | `inode_type` | `0x0001` file, `0x0002` dir, `0x0003` symlink, `0x0004` deleted |
| 0x02 | 2 | u16 | `mode` | Unix permissions + special bits (12 bits) |
| 0x04 | 4 | u32 | `uid` | Owner user ID |
| 0x08 | 4 | u32 | `gid` | Owner group ID |
| 0x0C | 4 | u32 | `refcount` | Hard-link count |
| 0x10 | 8 | u64 | `size` | Logical size in bytes |
| 0x18 | 8 | u64 | `blocks_used` | Allocated physical blocks |
| 0x20 | 8 | u64 | `crtime_ns` | Creation timestamp, Unix nanoseconds |
| 0x28 | 8 | u64 | `mtime_ns` | Last modification timestamp, Unix nanoseconds |
| 0x30 | 8 | u64 | `inode_no` | Self-referencing inode number |
| 0x38 | 2 | u16 | `extent_count` | Number of extents in `tail` |
| 0x3A | 2 | u16 | `xattr_len` | Inline xattr byte length |
| 0x3C | 2 | u16 | `inline_len` | Inline data byte length |
| 0x3E | 2 | u16 | `flags` | Inode flags (see below) |
| 0x40 | 64 | u8[64] | `tail` | Extents, then xattr, then inline data |

Inode flags:

| Bit | Name | Description |
|---|---|---|
| 0 | `IFLAG_INLINE` | Data is stored inline in `tail`; no extents |
| 1 | `IFLAG_SPARSE` | File has holes |
| 2 | `IFLAG_IMMUTABLE` | Cannot be modified or deleted |
| 3 | `IFLAG_APPEND` | Append-only |

## Extent descriptor (16 bytes, packed in `inode.tail`)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| 0x00 | 8 | u64 | `logical_block` | First logical block this extent covers |
| 0x08 | 6 | u48 | `phys_block` | Physical LBA, 48-bit LE (6 bytes) |
| 0x0E | 2 | u16 | `length` | Length in blocks (1–65535) |

48-bit physical LBA encoding:
```c
field[0] = (uint8_t)(lba >>  0);
field[1] = (uint8_t)(lba >>  8);
field[2] = (uint8_t)(lba >> 16);
field[3] = (uint8_t)(lba >> 24);
field[4] = (uint8_t)(lba >> 32);
field[5] = (uint8_t)(lba >> 40);
```

Maximum 4 extents per inode (`PARTFS_MAX_EXTENTS`). Extents are stored packed
at the start of `tail`. The remainder of `tail` holds xattr data and/or inline
data (see [xattr.md](xattr.md)).

## Inode B-tree leaf block

```
offset 0x00  block header (16 bytes, magic = BTLF)
offset 0x10  entry_count : u32
offset 0x14  reserved    : u32
offset 0x18  entries[entry_count] (each 144 bytes)
```

Each entry:

| Offset | Size | Field | Description |
|---|---|---|---|
| +0 | 8 | `key` | Inode number (sort key) |
| +8 | 4 | `val_size` | Always 128 (`sizeof(partfs_inode)`) |
| +12 | 4 | reserved | Zero |
| +16 | 128 | `data` | `struct partfs_inode` |

Maximum entries per leaf: `(4096 − 24) / 144 = 27` (`PARTFS_IENTRY_MAX`).
Split point: 13 (`PARTFS_IENTRY_SPLIT`).

## Inode B-tree internal node

```
offset 0x00  block header (16 bytes, magic = BTRE)
offset 0x10  entry_count : u32
offset 0x14  reserved    : u32
offset 0x18  child0(8) key0(8) child1(8) key1(8) ... key(n-1)(8) child_n(8)
```

Maximum keys: `(4096 − 24 − 8) / 16 = 253` (`PARTFS_BTRE_MAX_KEYS`).

## Directory data block

Same layout as an inode B-tree leaf block (`BTLF`), but entries are directory
records. Each record:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0x00 | 8 | `inode_no` | Target inode number; `0` = tombstone |
| 0x08 | 8 | `name_hash` | FNV-1a 64-bit hash of filename |
| 0x10 | 2 | `name_len` | Filename byte length |
| 0x12 | 2 | `rec_len` | Total record length, 4-byte aligned: `⌈(32 + name_len) / 4⌉ × 4` |
| 0x14 | 2 | `inode_type` | Copy of `inode.inode_type` |
| 0x16 | 2 | reserved | Zero |
| 0x18 | 4 | `crc32c` | CRC32C of full record; zeroed during calculation |
| 0x1C | 4 | padding | Zero |
| 0x20 | name_len | `name` | Filename, UTF-8, not null-terminated |
| … | 0–3 | padding | Zero-pad to 4-byte boundary |

Record length examples: `"."` → 36 bytes, `".."` → 36 bytes.

## Checksums

| Structure | Field | Coverage |
|---|---|---|
| Superblock | `crc32c` at 0x7C | bytes 0x00–0x7B (124 bytes) |
| Any metadata block | `hdr.crc32c` at offset 4 | full 4096-byte block |
| Directory record | `crc32c` at offset 0x18 | full variable-length record |

CRC32C polynomial: `0x82F63B78` (Castagnoli, reflected form).

In all cases the `crc32c` field is zeroed before computing the sum.

## FNV-1a 64-bit hash

Used as the B-tree key for directory entries.

```
offset_basis = 14695981039346656037
prime        = 1099511628211

hash = offset_basis
for each byte b in name:
    hash ^= (uint64_t)b
    hash *= prime
```
