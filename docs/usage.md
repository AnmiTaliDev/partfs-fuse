# Usage

## Synopsis

```
partfs [FUSE options] <device|image> <mountpoint>
```

The first non-option argument is the block device or image file to mount.
The second non-option argument is the mountpoint directory (must already exist).

## Creating a volume

Use `mkfs.part` from the `partfs_progs` submodule:

```sh
# Format a regular file
dd if=/dev/zero of=disk.img bs=4096 count=4096   # 16 MiB
partfs_progs/mkfs.part disk.img

# Format with a label and custom journal size
partfs_progs/mkfs.part -L myvolume -j 512 disk.img

# Format a block device (careful — destroys data)
sudo partfs_progs/mkfs.part /dev/sdX
```

## Mounting

```sh
mkdir mnt

# Basic mount (background)
./partfs disk.img mnt

# Foreground (stays attached to terminal, Ctrl-C to unmount)
./partfs -f disk.img mnt

# Debug mode (foreground + verbose FUSE trace)
./partfs -d disk.img mnt

# Read-only (FUSE option)
./partfs -o ro disk.img mnt

# Allow other users to access the mount
./partfs -o allow_other disk.img mnt

# Mount a block device
sudo ./partfs /dev/sdX mnt
```

## Unmounting

```sh
fusermount3 -u mnt

# If the process is stuck (kernel will force-unmount)
fusermount3 -uz mnt
```

## Supported operations

| Operation | FUSE op | Notes |
|---|---|---|
| List directory | `readdir` | |
| Stat file | `getattr` | |
| Read file | `read` | extent-mapped, sparse holes return zeros |
| Write file | `write` | allocates blocks on demand |
| Create file | `create` | |
| Make directory | `mkdir` | allocates a directory data block |
| Remove file | `unlink` | decrements refcount; frees blocks at zero |
| Remove directory | `rmdir` | must be empty |
| Rename / move | `rename` | supports `RENAME_NOREPLACE`, `RENAME_EXCHANGE` |
| Hard link | `link` | increments refcount |
| Symbolic link | `symlink` | short targets stored inline |
| Read symlink | `readlink` | |
| Change size | `truncate` | extends with zeros or frees tail blocks |
| Change times | `utimens` | stores mtime (crtime unchanged after creation) |
| Change permissions | `chmod` | updates `inode.mode` (full 12 bits) |
| Change owner | `chown` | updates `inode.uid` / `inode.gid` |
| Filesystem info | `statfs` | reports block/inode counts |
| Sync to disk | `fsync` | calls `fsync(2)` or `fdatasync(2)` on the image fd |
| Get xattr | `getxattr` | inline in inode tail |
| Set xattr | `setxattr` | inline in inode tail |
| List xattr | `listxattr` | |
| Remove xattr | `removexattr` | |

## Inode flags

Two inode flags affect permitted operations:

**`IFLAG_IMMUTABLE`** — the file cannot be modified or deleted:
- `write`, `truncate` → `EPERM`
- `unlink`, `rmdir` → `EPERM`
- `rename` (as source) → `EPERM`
- `chmod`, `chown` → `EPERM`
- `open` with write access → `EPERM`
- `setxattr`, `removexattr` → `EPERM`

**`IFLAG_APPEND`** — the file is append-only:
- `write` at offset < file size → `EPERM`
- `truncate` to a smaller size → `EPERM`
- `open` with `O_TRUNC` → `EPERM`

These flags are stored in `inode.flags` and are set by external tools (not yet
exposed via `ioctl` in the driver).

## Extended attributes

Extended attributes are stored inline in the inode tail (see [xattr.md](xattr.md)).
The available space depends on how many extents the file uses; for a typical
regular file with 1–2 extents there are 32–48 bytes available.

```sh
# Set a user xattr
setfattr -n user.comment -v "hello" mnt/file.txt

# Read it back
getfattr -n user.comment mnt/file.txt

# List all xattrs
getfattr -d mnt/file.txt

# Remove
setfattr -x user.comment mnt/file.txt
```

## Journal behaviour

On each mount the driver reads the journal header from blocks 2 and 3:

- **Clean** (`state=0`): normal mount, no action taken.
- **Dirty** (`state=1`) or **Replaying** (`state=2`): the driver replays all
  committed journal transactions before mounting and resets the state to clean.
  A message is printed to stderr.

The driver itself does not currently write to the journal during operation —
all metadata writes go directly to disk. Journal replay handles images that
were written by a future journalling-capable version of the driver.

## Common errors

| Error | Likely cause |
|---|---|
| `invalid or corrupt PartFS superblock` | Not a PartFS volume, or superblock CRC mismatch |
| `no block groups found` | Volume too small or corrupt group layout |
| `failed to read group descriptor N` | Disk I/O error or corrupt group descriptor CRC |
| `ENOSPC` on write | No free blocks; or file already has 4 extents and new blocks are not contiguous |
| `ENOSPC` on setxattr | Not enough space in inode tail for the xattr record |
| `EPERM` on write/unlink | File has `IFLAG_IMMUTABLE` set |
