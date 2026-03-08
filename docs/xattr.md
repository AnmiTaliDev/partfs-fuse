# Extended Attributes

## Overview

Extended attributes (xattrs) are stored inline in the inode's 64-byte `tail`
field, after the extent array and before any inline file data.

The layout of `inode.tail`:

```
[extents: extent_count × 16 bytes]
[xattr area: xattr_len bytes]
[inline data: inline_len bytes]
```

The three regions are packed with no gaps. Their sizes are stored in the inode:

| Field | Meaning |
|---|---|
| `extent_count` | Number of extents (each 16 bytes) |
| `xattr_len` | Byte length of the xattr area |
| `inline_len` | Byte length of inline data (`IFLAG_INLINE` files) |

The invariant `extent_count×16 + xattr_len + inline_len ≤ 64` must always hold.

## Xattr record format

The xattr area contains zero or more variable-length records packed consecutively
with no alignment padding:

```
[name_len : u8        ]
[value_len: u16 LE    ]
[name     : name_len bytes  ]
[value    : value_len bytes ]
```

Records are not individually checksummed; the inode itself is protected by the
B-tree leaf's block CRC32C.

Example — storing `user.comment = "hello"`:

```
05          name_len  = 5
05 00       value_len = 5
75 73 65 72 2e 63 6f 6d 6d 65 6e 74   "user.comment"
68 65 6c 6c 6f                         "hello"
```

Total: 1 + 2 + 12 + 5 = 20 bytes.

## Available space

The space available for xattrs depends on how many extents the inode has and
whether it stores inline data:

| extent_count | inline_len | Max xattr bytes |
|---|---|---|
| 0 | 0 | 64 |
| 1 | 0 | 48 |
| 2 | 0 | 32 |
| 3 | 0 | 16 |
| 4 | 0 | 0 |

For symlinks with `IFLAG_INLINE` the inline data occupies the tail from the end
of xattrs onward; adding xattrs to such an inode is only possible if the total
fits in 64 bytes.

When `setxattr` is called and there is not enough space, `ENOSPC` is returned.

## Namespace handling

The full xattr name including its namespace prefix (e.g. `user.comment`,
`security.capability`, `trusted.overlay.opaque`) is stored verbatim in the
record's `name` field. The driver does not enforce or interpret namespaces.
Name length is limited to 255 bytes (`UINT8_MAX`). Value length is limited to
65535 bytes (`UINT16_MAX`), though in practice the 64-byte tail cap makes
values larger than ~60 bytes impossible.

## POSIX semantics

The driver implements the standard POSIX xattr semantics:

- `setxattr` with `XATTR_CREATE`: fails with `EEXIST` if the attribute already exists.
- `setxattr` with `XATTR_REPLACE`: fails with `ENODATA` if the attribute does not exist.
- `setxattr` with `flags = 0`: creates or replaces unconditionally.
- `getxattr` with `size = 0`: returns the value size without copying; use this
  to query the size before allocating a buffer.
- `listxattr` with `size = 0`: returns the total byte length of the name list.
- Attempting any xattr mutation on an `IFLAG_IMMUTABLE` inode returns `EPERM`.

## Replace implementation detail

When replacing an existing xattr, the old record is removed by shifting the
remaining xattr bytes left (memmove), and the new record is appended at the
end of the xattr area. This keeps the area compact but means the order of
attributes may change across set operations.

## Superblock flag

`FLAG_XATTR` (bit 4 of `superblock.flags`) indicates that the volume supports
extended attributes. The driver reads and writes xattrs regardless of this flag;
the flag is informational for other tools.
