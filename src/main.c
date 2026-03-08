/* main.c - PartFS FUSE driver entry point
 * Copyright (C) 2026 AnmiTaliDev <anmitalidev@nuros.org>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "fuse_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "partfs.h"
#include "crc32c.h"
#include "io.h"

struct partfs_cli {
    char *device;
};

static int opt_proc(void *data, const char *arg, int key,
                    struct fuse_args *outargs)
{
    struct partfs_cli *cli = (struct partfs_cli *)data;
    (void)outargs;

    if (key == FUSE_OPT_KEY_NONOPT && cli->device == NULL) {
        cli->device = strdup(arg);
        return 0;
    }
    return 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [FUSE options] <device|image> <mountpoint>\n"
        "\n"
        "Mount a PartFS volume.\n"
        "\n"
        "FUSE options:\n"
        "  -o opt[,opt...]  mount options\n"
        "  -f               foreground operation\n"
        "  -d               debug mode (implies -f)\n"
        "  -h               print help\n",
        prog);
}

/* Journal replay: scan committed transactions from seq_tail to seq_head and
 * write each logged block back to its original location (block_hdr.block_no).
 * Transactions are delimited by COMT blocks; data blocks without a following
 * COMT are considered uncommitted and are skipped. */
static void journal_replay(int fd, const struct partfs_journal_hdr *jhdr)
{
    uint64_t jdata_start = le64toh(jhdr->journal_start);
    uint32_t jdata_size  = le32toh(jhdr->journal_size);
    uint64_t seq_tail    = le64toh(jhdr->seq_tail);
    uint64_t seq_head    = le64toh(jhdr->seq_head);

    if (seq_head == seq_tail || jdata_size == 0)
        return;

    /* Pending blocks for the current (not yet committed) transaction */
    uint8_t  *pending_data[4096];
    uint64_t  pending_lba[4096];
    int       pending_count = 0;

    fprintf(stderr, "partfs: replaying journal (seq %llu..%llu)\n",
            (unsigned long long)seq_tail, (unsigned long long)seq_head);

    uint8_t blk[PARTFS_BLOCK_SIZE];

    for (uint64_t seq = seq_tail; seq < seq_head; seq++) {
        uint64_t ring_pos = seq % jdata_size;
        uint64_t jlba    = jdata_start + ring_pos;

        if (block_read(fd, jlba, blk) < 0)
            goto discard_pending;

        struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)blk;
        uint32_t magic = le32toh(hdr->magic);

        if (magic == PARTFS_MAGIC_COMT) {
            /* Commit — write all pending blocks */
            for (int i = 0; i < pending_count; i++) {
                block_write(fd, pending_lba[i], pending_data[i]);
                free(pending_data[i]);
            }
            pending_count = 0;
            continue;
        }

        if (magic == 0)
            goto discard_pending;

        /* Data block — queue for the current transaction */
        uint64_t orig_lba = le64toh(hdr->block_no);
        if (orig_lba == jlba)
            continue; /* journal header itself — skip */

        if (pending_count < (int)(sizeof(pending_data) / sizeof(pending_data[0]))) {
            pending_data[pending_count] = malloc(PARTFS_BLOCK_SIZE);
            if (pending_data[pending_count]) {
                memcpy(pending_data[pending_count], blk, PARTFS_BLOCK_SIZE);
                pending_lba[pending_count] = orig_lba;
                pending_count++;
            }
        }
        continue;

discard_pending:
        for (int i = 0; i < pending_count; i++)
            free(pending_data[i]);
        pending_count = 0;
        break;
    }

    /* Free any uncommitted pending blocks */
    for (int i = 0; i < pending_count; i++)
        free(pending_data[i]);

    fsync(fd);
}

/* Read journal header, try block 2 first, fall back to block 3. */
static int journal_hdr_read(int fd, struct partfs_journal_hdr *out)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];

    if (block_read(fd, 2, buf) == 0) {
        struct partfs_journal_hdr *h = (struct partfs_journal_hdr *)buf;
        if (le32toh(h->hdr.magic) == PARTFS_MAGIC_JRNL) {
            memcpy(out, h, sizeof(*out));
            return 0;
        }
    }
    if (block_read(fd, 3, buf) == 0) {
        struct partfs_journal_hdr *h = (struct partfs_journal_hdr *)buf;
        if (le32toh(h->hdr.magic) == PARTFS_MAGIC_JRNL) {
            memcpy(out, h, sizeof(*out));
            return 0;
        }
    }
    return -1;
}

/* Write journal header to both redundant copies (blocks 2 and 3). */
static void journal_hdr_write(int fd, struct partfs_journal_hdr *jhdr)
{
    uint8_t buf[PARTFS_BLOCK_SIZE];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, jhdr, sizeof(*jhdr));
    struct partfs_block_hdr *hdr = (struct partfs_block_hdr *)buf;
    hdr->magic    = htole32(PARTFS_MAGIC_JRNL);
    hdr->block_no = htole64(2);
    block_crc_set(buf);
    block_write(fd, 2, buf);

    hdr->block_no = htole64(3);
    block_crc_set(buf);
    block_write(fd, 3, buf);
}

int main(int argc, char *argv[])
{
    crc32c_init();

    struct partfs_cli cli = {0};
    struct fuse_args  args = FUSE_ARGS_INIT(argc, argv);

    static const struct fuse_opt opts[] = {
        FUSE_OPT_KEY("-h",     0),
        FUSE_OPT_KEY("--help", 0),
        FUSE_OPT_END
    };

    if (fuse_opt_parse(&args, &cli, opts, opt_proc) != 0) {
        usage(argv[0]);
        return 1;
    }

    if (!cli.device) {
        usage(argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }

    int fd = open(cli.device, O_RDWR);
    if (fd < 0) {
        perror(cli.device);
        fuse_opt_free_args(&args);
        free(cli.device);
        return 1;
    }

    struct partfs_state fs;
    memset(&fs, 0, sizeof(fs));
    fs.fd = fd;
    pthread_mutex_init(&fs.lock, NULL);

    if (sb_read(&fs) < 0) {
        fprintf(stderr, "%s: invalid or corrupt PartFS superblock\n", cli.device);
        close(fd);
        fuse_opt_free_args(&args);
        free(cli.device);
        return 1;
    }

    /* Read journal header from blocks 2/3 */
    struct partfs_journal_hdr jhdr;
    if (journal_hdr_read(fd, &jhdr) < 0) {
        fprintf(stderr, "%s: warning: cannot read journal header\n", cli.device);
        memset(&jhdr, 0, sizeof(jhdr));
    } else {
        uint32_t jstate = le32toh(jhdr.state);
        if (jstate == PARTFS_JOURNAL_CLEAN) {
            /* Nothing to do */
        } else if (jstate == PARTFS_JOURNAL_DIRTY || jstate == PARTFS_JOURNAL_REPLAY) {
            fprintf(stderr, "%s: journal not clean (state=%u), replaying\n",
                    cli.device, jstate);
            jhdr.state = htole32(PARTFS_JOURNAL_REPLAY);
            journal_hdr_write(fd, &jhdr);

            journal_replay(fd, &jhdr);

            /* Mark journal clean after successful replay */
            jhdr.state    = htole32(PARTFS_JOURNAL_CLEAN);
            jhdr.seq_tail = jhdr.seq_head;
            journal_hdr_write(fd, &jhdr);
            fprintf(stderr, "%s: journal replay complete\n", cli.device);
        } else {
            fprintf(stderr, "%s: warning: unknown journal state %u\n",
                    cli.device, jstate);
        }
    }

    uint32_t journal_blocks = le32toh(fs.sb.journal_blocks);
    uint64_t journal_start  = le64toh(fs.sb.journal_start);
    fs.groups_start = journal_start + journal_blocks;

    uint64_t block_count = le64toh(fs.sb.block_count);
    uint32_t group_size  = le32toh(fs.sb.group_size);
    fs.num_groups = (block_count - fs.groups_start + group_size - 1) / group_size;

    if (fs.num_groups == 0) {
        fprintf(stderr, "%s: no block groups found\n", cli.device);
        close(fd);
        fuse_opt_free_args(&args);
        free(cli.device);
        return 1;
    }

    fs.groups = calloc(fs.num_groups, sizeof(struct partfs_group_desc));
    if (!fs.groups) {
        perror("calloc");
        close(fd);
        fuse_opt_free_args(&args);
        free(cli.device);
        return 1;
    }

    for (uint64_t i = 0; i < fs.num_groups; i++) {
        if (gd_read(&fs, i) < 0) {
            fprintf(stderr, "%s: failed to read group descriptor %lu\n",
                    cli.device, (unsigned long)i);
            free(fs.groups);
            close(fd);
            fuse_opt_free_args(&args);
            free(cli.device);
            return 1;
        }
    }

    fprintf(stderr, "partfs: mounting %s, %lu groups, %lu free blocks\n",
            cli.device,
            (unsigned long)fs.num_groups,
            (unsigned long)le64toh(fs.sb.free_blocks));

    int ret = fuse_main(args.argc, args.argv, &partfs_ops, &fs);

    free(fs.groups);
    close(fd);
    fuse_opt_free_args(&args);
    free(cli.device);
    pthread_mutex_destroy(&fs.lock);
    return ret;
}
