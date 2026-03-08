# Building and Installing partfs-fuse

## Requirements

| Component | Version | Notes |
|---|---|---|
| GCC | any with C11 | `gcc -std=c11` |
| libfuse3 | 3.x | headers and shared library |
| pkg-config | any | used by Makefile to locate fuse3 |
| GNU make | any | |

### Install dependencies

Arch Linux:
```sh
pacman -S fuse3 base-devel
```

Debian / Ubuntu:
```sh
apt install libfuse3-dev build-essential pkg-config
```

Fedora / RHEL:
```sh
dnf install fuse3-devel gcc make pkg-config
```

## Build

```sh
git clone --recurse-submodules <repo-url>
cd partfs-fuse
make
```

The build produces two artifacts in the project root:

- `libpartfs.so` — position-independent shared library containing all filesystem
  logic: I/O, B-tree, directory, extent, FUSE operation handlers.
- `partfs` — thin executable. Links against `libpartfs.so` and calls `fuse_main`.

All source files under `src/` except `main.c` are compiled into `libpartfs.so`
with `-fPIC -shared`. `main.c` is compiled separately into `partfs` and linked
with `-L. -lpartfs`.

## Clean

```sh
make clean
```

Removes `libpartfs.so`, `partfs`, and all `*.o` files.

## Install

```sh
sudo make install
```

Installs:
- `libpartfs.so` → `/usr/lib/libpartfs.so`
- `partfs` → `/usr/bin/partfs`

Then runs `ldconfig` to update the dynamic linker cache.

To install under a custom prefix:
```sh
sudo make install DESTDIR=/usr/local
```

## Running without installation

Set `LD_LIBRARY_PATH` so the dynamic linker finds `libpartfs.so` in the build
directory:

```sh
export LD_LIBRARY_PATH=.
./partfs disk.img mnt
```

## Submodule

The `partfs_progs/` directory is a git submodule containing `mkfs.part`.
Initialize it with:

```sh
git submodule update --init
```

Then build separately inside `partfs_progs/` according to its own instructions.
