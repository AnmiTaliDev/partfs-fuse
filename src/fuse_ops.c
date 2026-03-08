/* fuse_ops.c - FUSE operation implementations for PartFS
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "fuse_ops.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <endian.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <linux/fs.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE  (1 << 1)
#endif

#include "partfs.h"
#include "crc32c.h"
#include "io.h"
#include "alloc.h"
#include "btree.h"
#include "inode.h"
#include "dir.h"
#include "file.h"

static int path_walk(struct partfs_state *fs, const char *path,
                      struct partfs_inode *out, uint64_t *out_ino)
{
    uint64_t root_ino = le64toh(fs->sb.root_inode);

    if (strcmp(path, "/") == 0) {
        *out_ino = root_ino;
        return inode_lookup(fs, root_ino, out);
    }

    struct partfs_inode cur;
    uint64_t cur_ino = root_ino;
    int r = inode_lookup(fs, cur_ino, &cur);
    if (r < 0)
        return r;

    char pathbuf[4096];
    strncpy(pathbuf, path + 1, sizeof(pathbuf) - 1);
    pathbuf[sizeof(pathbuf) - 1] = '\0';

    char *saveptr  = NULL;
    char *component = strtok_r(pathbuf, "/", &saveptr);

    while (component != NULL) {
        if (le16toh(cur.inode_type) != PARTFS_ITYPE_DIR)
            return -ENOTDIR;

        uint64_t child_ino;
        uint16_t child_type;
        r = dir_lookup(fs, &cur, component, &child_ino, &child_type);
        if (r < 0)
            return r;

        cur_ino = child_ino;
        r = inode_lookup(fs, cur_ino, &cur);
        if (r < 0)
            return r;

        component = strtok_r(NULL, "/", &saveptr);
    }

    *out_ino = cur_ino;
    *out = cur;
    return 0;
}

static int path_walk_parent(struct partfs_state *fs, const char *path,
                             struct partfs_inode *out_parent, uint64_t *out_parent_ino,
                             const char **out_name)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash)
        return -EINVAL;

    *out_name = last_slash + 1;

    if (last_slash == path) {
        uint64_t root_ino = le64toh(fs->sb.root_inode);
        *out_parent_ino = root_ino;
        return inode_lookup(fs, root_ino, out_parent);
    }

    char parent_path[4096];
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len >= sizeof(parent_path))
        return -ENAMETOOLONG;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    return path_walk(fs, parent_path, out_parent, out_parent_ino);
}

struct readdir_ctx {
    void            *fuse_buf;
    fuse_fill_dir_t  filler;
};

static int readdir_cb(void *arg, const char *name, size_t name_len,
                       uint64_t ino, uint16_t itype)
{
    struct readdir_ctx *ctx = (struct readdir_ctx *)arg;
    char name_buf[256];
    if (name_len >= sizeof(name_buf))
        return 0;
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';
    (void)ino;
    (void)itype;
    ctx->filler(ctx->fuse_buf, name_buf, NULL, 0, 0);
    return 0;
}

/* xattr helpers — records are packed in inode.tail after extents:
 * each record: name_len(u8) value_len(u16le) name[name_len] value[value_len] */

static uint8_t *xattr_base(struct partfs_inode *inode)
{
    uint16_t ec = le16toh(inode->extent_count);
    return inode->tail + (size_t)ec * sizeof(struct partfs_extent);
}

static const uint8_t *xattr_base_c(const struct partfs_inode *inode)
{
    uint16_t ec = le16toh(inode->extent_count);
    return inode->tail + (size_t)ec * sizeof(struct partfs_extent);
}

/* Find xattr record by name. Returns pointer to record start, sets *out_end
 * to one byte past the record. Returns NULL if not found. */
static uint8_t *xattr_find(struct partfs_inode *inode, const char *name,
                             size_t name_len, uint8_t **out_end)
{
    uint16_t xlen = le16toh(inode->xattr_len);
    uint8_t *base = xattr_base(inode);
    uint8_t *p    = base;
    uint8_t *end  = base + xlen;

    while (p + 3 <= end) {
        uint8_t  nlen = p[0];
        uint16_t vlen;
        memcpy(&vlen, p + 1, 2);
        vlen = le16toh(vlen);
        uint8_t *rec_end = p + 3 + nlen + vlen;
        if (rec_end > end)
            break;
        if (nlen == (uint8_t)name_len && memcmp(p + 3, name, name_len) == 0) {
            if (out_end)
                *out_end = rec_end;
            return p;
        }
        p = rec_end;
    }
    return NULL;
}

static void *partfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 0;
    cfg->direct_io    = 1;

    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    uint32_t flags = le32toh(fs->sb.flags);
    flags &= ~PARTFS_FLAG_CLEAN;
    fs->sb.flags       = htole32(flags);
    fs->sb.mount_count = htole64(le64toh(fs->sb.mount_count) + 1);
    sb_write(fs);

    pthread_mutex_unlock(&fs->lock);
    return fs;
}

static void partfs_destroy(void *data)
{
    struct partfs_state *fs = (struct partfs_state *)data;
    pthread_mutex_lock(&fs->lock);

    uint32_t flags = le32toh(fs->sb.flags);
    flags |= PARTFS_FLAG_CLEAN;
    fs->sb.flags = htole32(flags);
    sb_write(fs);

    pthread_mutex_unlock(&fs->lock);
}

static int partfs_getattr(const char *path, struct stat *st,
                           struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    int r;

    if (fi && fi->fh != 0)
        r = inode_lookup(fs, fi->fh, &inode);
    else {
        uint64_t ino;
        r = path_walk(fs, path, &inode, &ino);
    }

    if (r == 0) {
        if (le16toh(inode.inode_type) == PARTFS_ITYPE_DELETED)
            r = -ENOENT;
        else
            inode_to_stat(&inode, st);
    }

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)flags;
    (void)fi;

    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode dir_inode;
    uint64_t dir_ino;
    int r = path_walk(fs, path, &dir_inode, &dir_ino);
    if (r < 0) {
        pthread_mutex_unlock(&fs->lock);
        return r;
    }
    if (le16toh(dir_inode.inode_type) != PARTFS_ITYPE_DIR) {
        pthread_mutex_unlock(&fs->lock);
        return -ENOTDIR;
    }

    struct readdir_ctx ctx = { .fuse_buf = buf, .filler = filler };
    r = dir_iter(fs, &dir_inode, readdir_cb, &ctx);

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_open(const char *path, struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r == 0) {
        if (le16toh(inode.inode_type) == PARTFS_ITYPE_DELETED)
            r = -ENOENT;
        else if (le16toh(inode.inode_type) == PARTFS_ITYPE_DIR)
            r = -EISDIR;
        else {
            uint16_t iflags = le16toh(inode.flags);
            int writing = (fi->flags & O_ACCMODE) == O_WRONLY
                       || (fi->flags & O_ACCMODE) == O_RDWR;
            if (writing && (iflags & PARTFS_IFLAG_IMMUTABLE))
                r = -EPERM;
            else if ((fi->flags & O_TRUNC) && (iflags & (PARTFS_IFLAG_IMMUTABLE | PARTFS_IFLAG_APPEND)))
                r = -EPERM;
            else
                fi->fh = ino;
        }
    }

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi)
{
    (void)path;
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    int r = inode_lookup(fs, fi->fh, &inode);
    if (r < 0) {
        pthread_mutex_unlock(&fs->lock);
        return r;
    }

    ssize_t n = file_read_data(fs, &inode, buf, size, offset);
    pthread_mutex_unlock(&fs->lock);
    return (int)n;
}

static int partfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    int r = inode_lookup(fs, fi->fh, &inode);
    if (r < 0) {
        pthread_mutex_unlock(&fs->lock);
        return r;
    }

    uint16_t iflags = le16toh(inode.flags);
    if (iflags & PARTFS_IFLAG_IMMUTABLE) {
        pthread_mutex_unlock(&fs->lock);
        return -EPERM;
    }
    if ((iflags & PARTFS_IFLAG_APPEND) && (uint64_t)offset < le64toh(inode.size)) {
        pthread_mutex_unlock(&fs->lock);
        return -EPERM;
    }

    ssize_t n = file_write_data(fs, &inode, buf, size, offset);
    if (n > 0)
        inode_write(fs, &inode);

    pthread_mutex_unlock(&fs->lock);
    return (int)n;
}

static int partfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    int r = path_walk_parent(fs, path, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(parent.inode_type) != PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -ENOTDIR; }

    uint64_t exist_ino;
    uint16_t exist_type;
    if (dir_lookup(fs, &parent, name, &exist_ino, &exist_type) == 0)
        { pthread_mutex_unlock(&fs->lock); return -EEXIST; }

    uint64_t new_ino;
    r = inode_alloc(fs, &new_ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint64_t now = time_now_ns();
    struct partfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_type = htole16(PARTFS_ITYPE_FILE);
    new_inode.mode       = htole16((uint16_t)(mode & 07777));
    new_inode.uid        = htole32(fuse_get_context()->uid);
    new_inode.gid        = htole32(fuse_get_context()->gid);
    new_inode.refcount   = htole32(1);
    new_inode.inode_no   = htole64(new_ino);
    new_inode.crtime_ns  = htole64(now);
    new_inode.mtime_ns   = htole64(now);

    r = ibtree_insert(fs, new_ino, &new_inode);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    r = dir_add(fs, &parent, name, new_ino, PARTFS_ITYPE_FILE);
    if (r == 0) inode_write(fs, &parent);
    fi->fh = new_ino;

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_mkdir(const char *path, mode_t mode)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    int r = path_walk_parent(fs, path, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(parent.inode_type) != PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -ENOTDIR; }

    int64_t dir_blk = block_alloc(fs, 0);
    if (dir_blk < 0) { pthread_mutex_unlock(&fs->lock); return (int)dir_blk; }

    uint64_t new_ino;
    r = inode_alloc(fs, &new_ino);
    if (r < 0) { block_free(fs, (uint64_t)dir_blk); pthread_mutex_unlock(&fs->lock); return r; }

    uint8_t buf[PARTFS_BLOCK_SIZE];
    memset(buf, 0, sizeof(buf));
    struct partfs_btree_leaf_hdr *lhdr = (struct partfs_btree_leaf_hdr *)buf;
    lhdr->hdr.magic    = htole32(PARTFS_MAGIC_BTLF);
    lhdr->hdr.block_no = htole64((uint64_t)dir_blk);
    lhdr->entry_count  = htole32(2);

    const char *dot_names[2] = {".", ".."};
    uint64_t    dot_inos[2]  = {new_ino, parent_ino};
    uint32_t pos = PARTFS_BTLEAF_HDR_SIZE;

    for (int di = 0; di < 2; di++) {
        size_t   nlen = strlen(dot_names[di]);
        uint16_t rlen = dirent_rec_len((uint16_t)nlen);
        struct partfs_dirent *de = (struct partfs_dirent *)(buf + pos);
        memset(de, 0, rlen);
        de->inode_no   = htole64(dot_inos[di]);
        de->name_hash  = htole64(fnv1a_64((const uint8_t *)dot_names[di], nlen));
        de->name_len   = htole16((uint16_t)nlen);
        de->rec_len    = htole16(rlen);
        de->inode_type = htole16(PARTFS_ITYPE_DIR);
        memcpy(buf + pos + sizeof(struct partfs_dirent), dot_names[di], nlen);
        de->crc32c = htole32(crc32c_compute(0, de, rlen));
        pos += rlen;
    }

    block_crc_set(buf);
    if (block_write(fs->fd, (uint64_t)dir_blk, buf) < 0)
        { pthread_mutex_unlock(&fs->lock); return -EIO; }

    uint64_t now = time_now_ns();
    struct partfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_type   = htole16(PARTFS_ITYPE_DIR);
    new_inode.mode         = htole16((uint16_t)(mode & 07777));
    new_inode.uid          = htole32(fuse_get_context()->uid);
    new_inode.gid          = htole32(fuse_get_context()->gid);
    new_inode.refcount     = htole32(2);
    new_inode.inode_no     = htole64(new_ino);
    new_inode.crtime_ns    = htole64(now);
    new_inode.mtime_ns     = htole64(now);
    new_inode.blocks_used  = htole64(1);
    new_inode.size         = htole64(PARTFS_BLOCK_SIZE);
    new_inode.extent_count = htole16(1);

    struct partfs_extent *e = (struct partfs_extent *)new_inode.tail;
    e->logical_block = htole64(0);
    extent_set_phys(e, (uint64_t)dir_blk);
    e->length = htole16(1);

    r = ibtree_insert(fs, new_ino, &new_inode);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    r = dir_add(fs, &parent, name, new_ino, PARTFS_ITYPE_DIR);
    if (r == 0) {
        parent.refcount = htole32(le32toh(parent.refcount) + 1);
        inode_write(fs, &parent);
    }

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_unlink(const char *path)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    int r = path_walk_parent(fs, path, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint64_t target_ino;
    uint16_t target_type;
    r = dir_lookup(fs, &parent, name, &target_ino, &target_type);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (target_type == PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -EISDIR; }

    struct partfs_inode target;
    r = inode_lookup(fs, target_ino, &target);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(target.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    uint32_t refcount = le32toh(target.refcount);
    if (refcount > 1) {
        target.refcount = htole32(refcount - 1);
        inode_write(fs, &target);
    } else {
        uint16_t ec = le16toh(target.extent_count);
        struct partfs_extent *ext = (struct partfs_extent *)target.tail;
        for (uint16_t i = 0; i < ec; i++) {
            uint64_t phys = extent_phys(&ext[i]);
            uint16_t len  = le16toh(ext[i].length);
            for (uint16_t b = 0; b < len; b++)
                block_free(fs, phys + b);
        }
        target.inode_type = htole16(PARTFS_ITYPE_DELETED);
        inode_write(fs, &target);
    }

    r = dir_remove(fs, &parent, name);
    if (r == 0)
        inode_write(fs, &parent);

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_rmdir(const char *path)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    int r = path_walk_parent(fs, path, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        { pthread_mutex_unlock(&fs->lock); return -EINVAL; }

    uint64_t target_ino;
    uint16_t target_type;
    r = dir_lookup(fs, &parent, name, &target_ino, &target_type);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (target_type != PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -ENOTDIR; }

    struct partfs_inode target;
    r = inode_lookup(fs, target_ino, &target);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(target.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    /* Check directory is empty (only . and ..) */
    uint16_t ec = le16toh(target.extent_count);
    const struct partfs_extent *extents = (const struct partfs_extent *)target.tail;
    uint8_t dbuf[PARTFS_BLOCK_SIZE];
    int non_dot = 0;

    for (uint16_t ei = 0; ei < ec && !non_dot; ei++) {
        uint16_t len  = le16toh(extents[ei].length);
        uint64_t phys = extent_phys(&extents[ei]);
        for (uint16_t bi = 0; bi < len && !non_dot; bi++) {
            if (block_read(fs->fd, phys + bi, dbuf) < 0)
                continue;
            struct partfs_btree_leaf_hdr *lh = (struct partfs_btree_leaf_hdr *)dbuf;
            uint32_t count = le32toh(lh->entry_count);
            uint32_t dpos  = PARTFS_BTLEAF_HDR_SIZE;
            for (uint32_t i = 0; i < count; i++) {
                if (dpos + sizeof(struct partfs_dirent) > PARTFS_BLOCK_SIZE) break;
                struct partfs_dirent *de = (struct partfs_dirent *)(dbuf + dpos);
                uint16_t rlen = le16toh(de->rec_len);
                if (rlen == 0) break;
                if (le64toh(de->inode_no) != 0) {
                    uint16_t nlen  = le16toh(de->name_len);
                    const char *dn = (const char *)(dbuf + dpos + sizeof(struct partfs_dirent));
                    if (!((nlen == 1 && dn[0] == '.')
                          || (nlen == 2 && dn[0] == '.' && dn[1] == '.'))) {
                        non_dot = 1;
                        break;
                    }
                }
                dpos += rlen;
            }
        }
    }

    if (non_dot) { pthread_mutex_unlock(&fs->lock); return -ENOTEMPTY; }

    for (uint16_t ei = 0; ei < ec; ei++) {
        uint64_t phys = extent_phys(&extents[ei]);
        uint16_t len  = le16toh(extents[ei].length);
        for (uint16_t b = 0; b < len; b++)
            block_free(fs, phys + b);
    }

    target.inode_type = htole16(PARTFS_ITYPE_DELETED);
    inode_write(fs, &target);

    r = dir_remove(fs, &parent, name);
    if (r == 0) {
        parent.refcount = htole32(le32toh(parent.refcount) - 1);
        inode_write(fs, &parent);
    }

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_rename(const char *from, const char *to, unsigned int flags)
{
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;

    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode src_parent, dst_parent;
    uint64_t src_parent_ino, dst_parent_ino;
    const char *src_name, *dst_name;

    int r = path_walk_parent(fs, from, &src_parent, &src_parent_ino, &src_name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    r = path_walk_parent(fs, to, &dst_parent, &dst_parent_ino, &dst_name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint64_t src_ino;
    uint16_t src_type;
    r = dir_lookup(fs, &src_parent, src_name, &src_ino, &src_type);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    /* Immutability check on source */
    struct partfs_inode src_inode;
    if (inode_lookup(fs, src_ino, &src_inode) == 0
        && (le16toh(src_inode.flags) & PARTFS_IFLAG_IMMUTABLE))
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    uint64_t dst_ino;
    uint16_t dst_type;
    int dst_exists = (dir_lookup(fs, &dst_parent, dst_name, &dst_ino, &dst_type) == 0);

    if (dst_exists && (flags & RENAME_NOREPLACE))
        { pthread_mutex_unlock(&fs->lock); return -EEXIST; }

    if (flags & RENAME_EXCHANGE) {
        if (!dst_exists) { pthread_mutex_unlock(&fs->lock); return -ENOENT; }
        dir_remove(fs, &src_parent, src_name);
        dir_remove(fs, &dst_parent, dst_name);
        dir_add(fs, &src_parent, src_name, dst_ino, dst_type);
        dir_add(fs, &dst_parent, dst_name, src_ino, src_type);
        inode_write(fs, &src_parent);
        inode_write(fs, &dst_parent);
        pthread_mutex_unlock(&fs->lock);
        return 0;
    }

    if (dst_exists) {
        struct partfs_inode dst_inode;
        if (inode_lookup(fs, dst_ino, &dst_inode) == 0) {
            uint32_t ref = le32toh(dst_inode.refcount);
            if (ref <= 1) {
                uint16_t dec = le16toh(dst_inode.extent_count);
                struct partfs_extent *ext = (struct partfs_extent *)dst_inode.tail;
                for (uint16_t i = 0; i < dec; i++) {
                    uint64_t phys = extent_phys(&ext[i]);
                    uint16_t len  = le16toh(ext[i].length);
                    for (uint16_t b = 0; b < len; b++)
                        block_free(fs, phys + b);
                }
                dst_inode.inode_type = htole16(PARTFS_ITYPE_DELETED);
                inode_write(fs, &dst_inode);
            } else {
                dst_inode.refcount = htole32(ref - 1);
                inode_write(fs, &dst_inode);
            }
        }
        dir_remove(fs, &dst_parent, dst_name);
    }

    dir_add(fs, &dst_parent, dst_name, src_ino, src_type);
    dir_remove(fs, &src_parent, src_name);
    inode_write(fs, &src_parent);
    inode_write(fs, &dst_parent);

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

static int partfs_link(const char *from, const char *to)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode target;
    uint64_t target_ino;
    int r = path_walk(fs, from, &target, &target_ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(target.inode_type) == PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }
    if (le16toh(target.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    r = path_walk_parent(fs, to, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(parent.inode_type) != PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -ENOTDIR; }

    uint64_t exist_ino;
    uint16_t exist_type;
    if (dir_lookup(fs, &parent, name, &exist_ino, &exist_type) == 0)
        { pthread_mutex_unlock(&fs->lock); return -EEXIST; }

    target.refcount = htole32(le32toh(target.refcount) + 1);
    inode_write(fs, &target);

    r = dir_add(fs, &parent, name, target_ino, le16toh(target.inode_type));
    if (r == 0)
        inode_write(fs, &parent);

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r;
    if (fi && fi->fh != 0) {
        ino = fi->fh;
        r = inode_lookup(fs, ino, &inode);
    } else {
        r = path_walk(fs, path, &inode, &ino);
    }
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(inode.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    inode.mode = htole16((uint16_t)(mode & 07777));
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_chown(const char *path, uid_t uid, gid_t gid,
                         struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r;
    if (fi && fi->fh != 0) {
        ino = fi->fh;
        r = inode_lookup(fs, ino, &inode);
    } else {
        r = path_walk(fs, path, &inode, &ino);
    }
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(inode.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    if (uid != (uid_t)-1)
        inode.uid = htole32((uint32_t)uid);
    if (gid != (gid_t)-1)
        inode.gid = htole32((uint32_t)gid);
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r;
    if (fi && fi->fh != 0) {
        ino = fi->fh;
        r = inode_lookup(fs, ino, &inode);
    } else {
        r = path_walk(fs, path, &inode, &ino);
    }
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint16_t iflags = le16toh(inode.flags);
    if (iflags & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }
    if ((iflags & PARTFS_IFLAG_APPEND) && (uint64_t)size < le64toh(inode.size))
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    uint64_t current_size = le64toh(inode.size);
    uint64_t new_size     = (uint64_t)size;

    if (new_size == current_size) { pthread_mutex_unlock(&fs->lock); return 0; }

    if (new_size < current_size) {
        uint64_t last_logical = (new_size + PARTFS_BLOCK_SIZE - 1) / PARTFS_BLOCK_SIZE;
        uint16_t extent_count = le16toh(inode.extent_count);
        struct partfs_extent *extents = (struct partfs_extent *)inode.tail;
        uint64_t blocks_freed = 0;

        for (uint16_t i = 0; i < extent_count; i++) {
            uint64_t lb  = le64toh(extents[i].logical_block);
            uint16_t len = le16toh(extents[i].length);
            uint64_t phys = extent_phys(&extents[i]);

            if (lb >= last_logical) {
                for (uint16_t b = 0; b < len; b++)
                    block_free(fs, phys + b);
                blocks_freed += len;
                extents[i].length = htole16(0);
            } else if (lb + len > last_logical) {
                uint64_t keep = last_logical - lb;
                for (uint64_t b = keep; b < len; b++)
                    block_free(fs, phys + b);
                blocks_freed += len - keep;
                extents[i].length = htole16((uint16_t)keep);
            }
        }

        uint16_t new_ec = 0;
        for (uint16_t i = 0; i < extent_count; i++) {
            if (le16toh(extents[i].length) > 0) {
                if (new_ec != i) extents[new_ec] = extents[i];
                new_ec++;
            }
        }
        inode.extent_count = htole16(new_ec);
        inode.blocks_used  = htole64(le64toh(inode.blocks_used) - blocks_freed);
    } else {
        uint8_t zeros[PARTFS_BLOCK_SIZE];
        memset(zeros, 0, sizeof(zeros));
        uint64_t to_write = new_size - current_size;
        off_t write_off   = (off_t)current_size;
        while (to_write > 0) {
            size_t chunk = to_write > PARTFS_BLOCK_SIZE ? PARTFS_BLOCK_SIZE : (size_t)to_write;
            ssize_t n = file_write_data(fs, &inode, zeros, chunk, write_off);
            if (n <= 0) break;
            write_off += n;
            to_write  -= (uint64_t)n;
        }
    }

    inode.size     = htole64(new_size);
    inode.mtime_ns = htole64(time_now_ns());
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_utimens(const char *path, const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r;
    if (fi && fi->fh != 0) {
        ino = fi->fh;
        r = inode_lookup(fs, ino, &inode);
    } else {
        r = path_walk(fs, path, &inode, &ino);
    }
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint64_t mtime_ns;
    if (tv[1].tv_nsec == UTIME_NOW)
        mtime_ns = time_now_ns();
    else if (tv[1].tv_nsec == UTIME_OMIT)
        mtime_ns = le64toh(inode.mtime_ns);
    else
        mtime_ns = (uint64_t)tv[1].tv_sec * 1000000000ULL + (uint64_t)tv[1].tv_nsec;

    inode.mtime_ns = htole64(mtime_ns);
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_readlink(const char *path, char *buf, size_t size)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(inode.inode_type) != PARTFS_ITYPE_SYMLINK)
        { pthread_mutex_unlock(&fs->lock); return -EINVAL; }

    uint64_t link_size = le64toh(inode.size);
    if (link_size >= size)
        link_size = size - 1;

    ssize_t n = file_read_data(fs, &inode, buf, (size_t)link_size, 0);
    if (n >= 0) buf[n] = '\0';

    pthread_mutex_unlock(&fs->lock);
    return (n < 0) ? (int)n : 0;
}

static int partfs_symlink(const char *target, const char *linkpath)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode parent;
    uint64_t parent_ino;
    const char *name;
    int r = path_walk_parent(fs, linkpath, &parent, &parent_ino, &name);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }
    if (le16toh(parent.inode_type) != PARTFS_ITYPE_DIR)
        { pthread_mutex_unlock(&fs->lock); return -ENOTDIR; }

    uint64_t new_ino;
    r = inode_alloc(fs, &new_ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    size_t target_len = strlen(target);
    uint64_t now = time_now_ns();
    struct partfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_type = htole16(PARTFS_ITYPE_SYMLINK);
    new_inode.mode       = htole16(0777);
    new_inode.uid        = htole32(fuse_get_context()->uid);
    new_inode.gid        = htole32(fuse_get_context()->gid);
    new_inode.refcount   = htole32(1);
    new_inode.inode_no   = htole64(new_ino);
    new_inode.crtime_ns  = htole64(now);
    new_inode.mtime_ns   = htole64(now);
    new_inode.size       = htole64(target_len);

    /* Store short targets inline in tail */
    size_t inline_room = sizeof(new_inode.tail)
        - (size_t)PARTFS_MAX_EXTENTS * sizeof(struct partfs_extent);
    if (target_len <= inline_room) {
        new_inode.flags      = htole16(PARTFS_IFLAG_INLINE);
        new_inode.inline_len = htole16((uint16_t)target_len);
        memcpy(new_inode.tail, target, target_len);
        r = ibtree_insert(fs, new_ino, &new_inode);
    } else {
        r = ibtree_insert(fs, new_ino, &new_inode);
        if (r == 0) {
            ssize_t n = file_write_data(fs, &new_inode, target, target_len, 0);
            if (n < 0)
                r = (int)n;
            else
                inode_write(fs, &new_inode);
        }
    }
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    r = dir_add(fs, &parent, name, new_ino, PARTFS_ITYPE_SYMLINK);
    if (r == 0) inode_write(fs, &parent);

    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    int ret = datasync ? fdatasync(fs->fd) : fsync(fs->fd);
    return ret < 0 ? -errno : 0;
}

static int partfs_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;
    return 0;
}

static int partfs_getxattr(const char *path, const char *name,
                            char *value, size_t size)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint16_t xlen = le16toh(inode.xattr_len);
    if (xlen == 0) { pthread_mutex_unlock(&fs->lock); return -ENODATA; }

    size_t name_len = strlen(name);
    uint8_t *rec_end;
    uint8_t *rec = xattr_find(&inode, name, name_len, &rec_end);
    if (!rec) { pthread_mutex_unlock(&fs->lock); return -ENODATA; }

    uint16_t vlen;
    memcpy(&vlen, rec + 1, 2);
    vlen = le16toh(vlen);

    if (size == 0) { pthread_mutex_unlock(&fs->lock); return (int)vlen; }
    if (size < vlen) { pthread_mutex_unlock(&fs->lock); return -ERANGE; }

    memcpy(value, rec + 3 + rec[0], vlen);
    pthread_mutex_unlock(&fs->lock);
    return (int)vlen;
}

static int partfs_setxattr(const char *path, const char *name,
                            const char *value, size_t size, int flags)
{
    if (size > UINT16_MAX)
        return -E2BIG;

    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(inode.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    size_t name_len = strlen(name);
    if (name_len > UINT8_MAX) { pthread_mutex_unlock(&fs->lock); return -E2BIG; }

    uint8_t *rec_end;
    uint8_t *existing = xattr_find(&inode, name, name_len, &rec_end);

    if (flags == XATTR_CREATE && existing)
        { pthread_mutex_unlock(&fs->lock); return -EEXIST; }
    if (flags == XATTR_REPLACE && !existing)
        { pthread_mutex_unlock(&fs->lock); return -ENODATA; }

    uint16_t xlen = le16toh(inode.xattr_len);
    uint8_t *base = xattr_base(&inode);

    if (existing) {
        uint8_t  onlen = existing[0];
        uint16_t ovlen;
        memcpy(&ovlen, existing + 1, 2);
        ovlen = le16toh(ovlen);
        size_t old_size   = 1 + 2 + onlen + ovlen;
        size_t tail_after = (size_t)(base + xlen - rec_end);
        memmove(existing, rec_end, tail_after);
        xlen -= (uint16_t)old_size;
    }

    size_t new_rec_size = 1 + 2 + name_len + size;
    uint16_t inline_len = le16toh(inode.inline_len);
    uint16_t ec         = le16toh(inode.extent_count);
    size_t tail_used    = (size_t)ec * sizeof(struct partfs_extent) + xlen + inline_len;
    if (tail_used + new_rec_size > sizeof(inode.tail))
        { pthread_mutex_unlock(&fs->lock); return -ENOSPC; }

    if (inline_len > 0)
        memmove(base + xlen + new_rec_size, base + xlen, inline_len);

    uint8_t *p    = base + xlen;
    p[0]          = (uint8_t)name_len;
    uint16_t vlen_le = htole16((uint16_t)size);
    memcpy(p + 1, &vlen_le, 2);
    memcpy(p + 3, name, name_len);
    memcpy(p + 3 + name_len, value, size);
    xlen += (uint16_t)new_rec_size;

    inode.xattr_len = htole16(xlen);
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_listxattr(const char *path, char *list, size_t size)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    uint16_t xlen         = le16toh(inode.xattr_len);
    const uint8_t *base   = xattr_base_c(&inode);
    const uint8_t *p      = base;
    const uint8_t *end    = base + xlen;
    size_t total          = 0;

    while (p + 3 <= end) {
        uint8_t  nlen = p[0];
        uint16_t vlen;
        memcpy(&vlen, p + 1, 2);
        vlen = le16toh(vlen);
        const uint8_t *rec_end = p + 3 + nlen + vlen;
        if (rec_end > end)
            break;
        if (size > 0) {
            if (total + nlen + 1 > size) {
                pthread_mutex_unlock(&fs->lock);
                return -ERANGE;
            }
            memcpy(list + total, p + 3, nlen);
            list[total + nlen] = '\0';
        }
        total += nlen + 1;
        p = rec_end;
    }

    pthread_mutex_unlock(&fs->lock);
    return (int)total;
}

static int partfs_removexattr(const char *path, const char *name)
{
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    struct partfs_inode inode;
    uint64_t ino;
    int r = path_walk(fs, path, &inode, &ino);
    if (r < 0) { pthread_mutex_unlock(&fs->lock); return r; }

    if (le16toh(inode.flags) & PARTFS_IFLAG_IMMUTABLE)
        { pthread_mutex_unlock(&fs->lock); return -EPERM; }

    size_t name_len = strlen(name);
    uint16_t xlen   = le16toh(inode.xattr_len);
    uint8_t *base   = xattr_base(&inode);
    uint8_t *rec_end;
    uint8_t *rec = xattr_find(&inode, name, name_len, &rec_end);
    if (!rec) { pthread_mutex_unlock(&fs->lock); return -ENODATA; }

    uint8_t  nlen = rec[0];
    uint16_t vlen;
    memcpy(&vlen, rec + 1, 2);
    vlen = le16toh(vlen);
    size_t rec_size   = 1 + 2 + nlen + vlen;
    size_t tail_after = (size_t)(base + xlen - rec_end);
    memmove(rec, rec_end, tail_after);
    xlen -= (uint16_t)rec_size;

    inode.xattr_len = htole16(xlen);
    r = inode_write(fs, &inode);
    pthread_mutex_unlock(&fs->lock);
    return r;
}

static int partfs_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    struct partfs_state *fs = (struct partfs_state *)fuse_get_context()->private_data;
    pthread_mutex_lock(&fs->lock);

    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = PARTFS_BLOCK_SIZE;
    stbuf->f_frsize  = PARTFS_BLOCK_SIZE;
    stbuf->f_blocks  = le64toh(fs->sb.block_count);
    stbuf->f_bfree   = le64toh(fs->sb.free_blocks);
    stbuf->f_bavail  = stbuf->f_bfree;
    stbuf->f_files   = le64toh(fs->sb.inode_count);
    stbuf->f_ffree   = stbuf->f_bfree;
    stbuf->f_namemax = 255;

    pthread_mutex_unlock(&fs->lock);
    return 0;
}

const struct fuse_operations partfs_ops = {
    .init        = partfs_init,
    .destroy     = partfs_destroy,
    .getattr     = partfs_getattr,
    .readdir     = partfs_readdir,
    .open        = partfs_open,
    .read        = partfs_read,
    .write       = partfs_write,
    .flush       = partfs_flush,
    .fsync       = partfs_fsync,
    .create      = partfs_create,
    .mkdir       = partfs_mkdir,
    .unlink      = partfs_unlink,
    .rmdir       = partfs_rmdir,
    .rename      = partfs_rename,
    .link        = partfs_link,
    .chmod       = partfs_chmod,
    .chown       = partfs_chown,
    .truncate    = partfs_truncate,
    .utimens     = partfs_utimens,
    .readlink    = partfs_readlink,
    .symlink     = partfs_symlink,
    .statfs      = partfs_statfs,
    .getxattr    = partfs_getxattr,
    .setxattr    = partfs_setxattr,
    .listxattr   = partfs_listxattr,
    .removexattr = partfs_removexattr,
};
