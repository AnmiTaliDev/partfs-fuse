# PartFS FUSE Driver — Roadmap

## v0.1 (current)

- [x] Superblock read/write with CRC32C validation and backup fallback
- [x] Group descriptor read/write
- [x] Allocation bitmap: get, set
- [x] Block allocator with group-preference and round-robin fallback
- [x] Inode B-tree: lookup, sorted insert, in-place update
- [x] B-tree leaf splitting with internal node creation
- [x] Internal node splitting (recursive)
- [x] Directory: lookup by FNV-1a hash, add, remove (tombstone), iteration
- [x] Extent-based file read (including inline data and sparse holes)
- [x] Extent-based file write with contiguous extent extension
- [x] FUSE operations: `getattr`, `readdir`, `open`, `read`, `write`,
      `create`, `mkdir`, `unlink`, `rmdir`, `rename`, `truncate`,
      `utimens`, `readlink`, `symlink`, `statfs`
- [x] Hard links (`link` operation)
- [x] `chmod`, `chown` support
- [x] `fsync` / `fdatasync` support
- [x] `flush` on file close
- [x] Extended attributes inline in inode tail (`getxattr`, `setxattr`,
      `listxattr`, `removexattr`)
- [x] `IFLAG_IMMUTABLE` and `IFLAG_APPEND` enforcement
- [x] Clean flag and mount count on mount/unmount
- [x] Journal header read with redundant copy fallback (blocks 2 and 3)
- [x] Journal replay on unclean mount (WAL, commit-block delimited)

## v0.2

- [ ] Per-group inode B-tree creation when group 0's tree is full
- [ ] Beyond-4-extents files via indirect extent block
- [ ] Directory B-tree internal nodes for large directories
- [ ] Inode B-tree deletion with rebalancing (replace tombstone approach)

## v0.3

- [ ] POSIX ACL support via xattr (`FLAG_ACL`)
- [ ] Write-ahead journal for metadata operations (driver-side journalling)
- [ ] Block group creation on demand (grow filesystem)

## v0.4

- [ ] `fallocate` support
- [ ] Multi-threaded I/O (per-inode locking instead of global mutex)
