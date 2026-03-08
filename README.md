# partfs-fuse

FUSE 3 driver for the PartFS filesystem.

PartFS is a custom filesystem with extent-based file storage, per-group inode B-trees,
FNV-1a directory hashing, CRC32C checksums on all metadata, and a write-ahead journal.

## Features

- Read and write: regular files, directories, symbolic links, hard links
- Extent-based storage — up to 4 inline extents per inode
- Inode B-tree with recursive leaf and internal node splitting
- Directory B-tree keyed by FNV-1a 64-bit filename hash
- CRC32C (Castagnoli) checksums on every metadata block
- Inline extended attributes stored in the inode tail
- `IFLAG_IMMUTABLE` and `IFLAG_APPEND` flag enforcement
- Clean flag, mount count, and journal state management
- Journal replay on unclean mount (WAL, commit-block delimited)
- Full POSIX permission model: `chmod`, `chown`, setuid/setgid/sticky bits

## Dependencies

- `libfuse3` (FUSE 3.x)
- GCC with C11 support

```sh
# Arch Linux
pacman -S fuse3

# Debian / Ubuntu
apt install libfuse3-dev
```

## Building

```sh
make
```

Produces:
- `libpartfs.so` — shared library with the full driver implementation
- `partfs` — thin executable that loads `libpartfs.so` and calls `fuse_main`

To install system-wide:
```sh
sudo make install
```

## Documentation

| Document | Description |
|---|---|
| [docs/api.md](docs/api.md) | Complete library API reference |
| [docs/architecture.md](docs/architecture.md) | Project structure and layer design |
| [docs/on-disk-format.md](docs/on-disk-format.md) | On-disk format reference |
| [docs/internals.md](docs/internals.md) | Module-by-module internals |
| [docs/xattr.md](docs/xattr.md) | Extended attributes implementation |
| [docs/usage.md](docs/usage.md) | Mount options and usage examples |
| [docs/building.md](docs/building.md) | Build and install instructions |

## Known limitations

- Maximum 4 extents per inode (returns `ENOSPC` when exceeded)
- Inline xattr space is limited by the 64-byte inode tail
- Only group 0's inode B-tree is used; per-group trees are not yet created
- Driver-side journalling not yet implemented (replay is implemented)

## License

GNU General Public License v2.0 only. See LICENSE.

## Author

AnmiTaliDev <anmitalidev@nuros.org>
