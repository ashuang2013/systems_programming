#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "mu.h"

#define USAGE \
    "Usage: fed [-s START] [-e END] [-r] [-k] FILE \n" \
    "\n" \
    "The fed editor either prints or modifies FILE according to an operation, which the user gives as an operation.\n" \
    "\n" \
    "Optional Arguments:\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit.\n" \
    "\n" \
    "   -s NUM, --start NUM\n" \
    "       The start index for an operation. If not specified, START defaults to 0. START must be in the range [0, FSIZE]. (N.B., FSIZE is a valid value so that the insert command can append data.\n" \
    "   -e NUM, --end NUM\n" \
    "       The end index for an operation. If not specified, END defaults to the file's size. END must be in the range [0, FSIZE]. It is an error if START > END.\n" \
    "\n" \
    "   -r, --remove\n" \
    "       Remove the bytes in the file from inides [START, END). Any remaining bytes from [END, FSIZE) are shifted down to START. The file's new size is (FSIZE - (END - START)).\n" \
    "   -k, --keep\n" \
    "       Keep the bytes in the file from indices [START, END), and removes all others. These kept bytes are shifted down to index 0. The file's new size is (END - START).\n" \
    "   -x, --expunge\n" \
    "       Overwrite the bytes in the file ffrom indices [START, END) with * characters. The file size does not change.\n" \
    "   -i, --insert\n" \
    "       Insert the STR into the file at index START, shifting the existing bytes up. The file's new size is (FSIZE + strlen(STR)).\n" \

#define die(fmt, ...) \
    do { \
        fprintf(stderr, "[die] %s:%d " fmt "\n", \
                __func__, __LINE__,##__VA_ARGS__); \
        exit(1); \
    } while (0)

#define die_errno(errnum, fmt, ...) \
    do { \
        fprintf(stderr, "[die] %s:%d " fmt ": %s\n", \
                __func__, __LINE__,##__VA_ARGS__, strerror(errnum)); \
        exit(1); \
    } while (0)

#define CMD_PRINT   0
#define CMD_REMOVE  1U<<0
#define CMD_KEEP    1U<<1
#define CMD_EXPUNGE 1U<<2
#define CMD_INSERT  1U<<3

static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}

/*
 * On success, return the file size.
 * On error, return a negative errno value.
 */
static ssize_t
file_size(const char *path)
{
    int ret;
    struct stat statbuf;

    ret = stat(path, &statbuf);
    return ret == -1 ? -errno : statbuf.st_size;
}

/*
 * The print method corresponding to -p or --print 
 * output: returns 0 on success, -1 on failure
 */
static int
fprint(const char *path, long start, long end) 
{
    int ret = 0;
    int fd;
    off_t off;
    char buf[1] = {0};
    long left = end - start;
    size_t want = 0;
    ssize_t n = 0;

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        ret = -1;
        mu_stderr_errno(errno, "%s", path);
        goto out;
    }

    off = lseek(fd, start, SEEK_SET);
    if (off == -1) {
        ret = -1;
        mu_stderr_errno(errno, "lseek");
        goto out;
    }

    want = MU_MIN(sizeof(buf), (size_t)left);
    while (want) {
        n = read(fd, buf, want);
        if (n == -1) {
            ret = -1;
            mu_stderr_errno(errno, "error reading %zu bytes from \"%s\"", want, path);
            goto out;
        }
        if (n == 0)
            break;

        ret = mu_write_n(STDOUT_FILENO, buf, (size_t)n, NULL);
        if (ret != 0) {
            mu_stderr_errno(-ret, "error writing to stdout");
            ret = -1;
            goto out;
        }

        left -= n;
        want = MU_MIN((size_t)left, sizeof(buf));
    }

out:
    if (fd != -1)
        close(fd);

    return ret;
}

/*
 * The remove method corresponding to -r or --remove
 * output: returns 0 on success, -1 on failure
 */
static int
fremove(const char *path, long start, long end, long size, int state) 
{
    int ret = 0;
    int rfd, wfd;
    off_t roff, woff;
    char buf[1] = {0};
    long left = size - end;
    size_t want = 0;
    ssize_t n = 0;

    rfd = open(path, O_RDONLY);
    if (rfd == -1) {
        ret = -1;
        mu_stderr_errno(errno, "%s", path);
        goto out;
    }

    wfd = open(path, O_WRONLY);
    if (wfd == -1) {
        ret = -1;
        mu_stderr_errno(errno, "%s", path);
        goto out;
    }

    roff = lseek(rfd, end, SEEK_SET);
    if (roff == -1) {
        ret = -1;
        mu_stderr_errno(errno, "lseek");
        goto out;
    }

    woff = lseek(wfd, start, SEEK_SET);
    if (woff == -1) {
        ret = -1;
        mu_stderr_errno(errno, "lseek");
        goto out;
    }
    
    /*
     * CASE 1: Removing the head (start = 0) or center
     * CASE 2: Removing the tail (end = FILE_SIZE) or entirety
     */
    switch(state) {
    case 1:
        want = MU_MIN(sizeof(buf), (size_t)left);
        while (left) {
            n = read(rfd, buf, want);
            if (n == -1) {
                ret = -1;
                mu_stderr_errno(errno, "error reading %zu bytes from \"%s\"", want, path);
                goto out;
            }

            if (n == 0)
                break;

            ret = mu_write_n(wfd, buf, (size_t)n, NULL);

            if (ret != 0) {
                mu_stderr_errno(-ret, "error writing to stdout");
                ret = -1;
                goto out;
            }
            
            left -= n;
            want = MU_MIN((size_t)left, sizeof(buf));
        }
        truncate(path, (size-(end-start)));
        break;
    case 2:
        truncate(path, start);         
        goto out;
    default:
        ret = -1;
        break;
    }

out:
    if (rfd != -1)
        close(rfd);

    if (wfd != -1)
        close(wfd);

    return ret;
}

/*
 * The print method corresponding to -k or --keep
 * output: returns 0 on success, -1 on failure
 */
static int
fkeep(const char *path, long start, long end, long size) 
{
    int ret = 0;
    if(start == 0) { //keep head
        fremove(path, end, size, size, 2);
    }
    else if(end == size) { //keep tail
        fremove(path, 0, start, size, 1);
    }
    else { //keep center
        fremove(path, end, size, size, 2);
        fremove(path, 0, start, (size-(size-end)), 1);
    }

    return ret;
}

int
main(int argc,char *argv[])
{
    int opt;
    /*
     * An option that takes a required argument is followed by a ':'.
     * The leading ':' suppresses getopt_long's normal error handling.
     */
    const char *short_opts = ":hprkxi:s:e:";
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"print", no_argument, NULL, 'p'},
        {"remove", no_argument, NULL, 'r'},
        {"keep", no_argument, NULL, 'k'},
        {"expunge", no_argument, NULL, 'x'},
        {"insert", required_argument, NULL, 'i'},        
        {"start", required_argument, NULL, 's'},
        {"end", required_argument, NULL, 'e'},
        {NULL, 0, NULL, 0}
    };

    unsigned int cmd = 0;
    long start = -1, end = -1; //NULL values
    int size = -1, ret = 0;
    int EXIT_STATUS = 0;
    bool negative_ticked = false;

    long FILE_SIZE = (long)file_size(argv[argc-1]);

    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (opt == -1) {
            /* processed all command-line options */
            break;
        }

        switch (opt) {
        case 'h':
            usage(0);
            return 0;
        case 'r':
            cmd |= CMD_REMOVE;
            break;
        case 'k':
            cmd |= CMD_KEEP;
            break;
        case 'x':
            cmd |= CMD_EXPUNGE;
            break;
        case 'i':
            cmd |= CMD_INSERT;
            break;
        case 's':
            ret = mu_str_to_int(optarg, 10, &size);
            if(ret != 0)
                die_errno(-ret, "invalid value for --start: \"%s\"", optarg);

            if(size < 0 || size > FILE_SIZE) 
                negative_ticked = true;

            start = size;
            break;
        case 'e':
            ret = mu_str_to_int(optarg, 10, &size);
            if(ret != 0)
                die_errno(-ret, "invalid value for --end: \"%s\"", optarg);

            if(size < 0 || size > FILE_SIZE) 
                negative_ticked = true;

            end = size;
            break;
        case '?':
            mu_die("unknown option '%c' (decimal: %d)", optopt, optopt);
            break;
        case ':':
            mu_die("missing option argument for option %c", optopt);
            break;
        default:
            mu_die("unexpected getopt_long return value: %c", (char)opt);
        }
    }

    /*
     *  ERROR CHECKING FOR START AND END
     */
    if(negative_ticked) {
        if(start < 0)
            die_errno(-ret, "invalid negative value for --start: \"%ld\"", start);
        else
            die_errno(-ret, "invalid negative value for --end: \"%ld\"", end);
    }

    if(start < 0 || start >= FILE_SIZE)
        start = 0;
    
    if(end < 0 || end > FILE_SIZE)
        end = FILE_SIZE;

    if(start > end) {
        start = 0;
        end = 0;
    }

    switch (cmd) {
    case CMD_PRINT:
        EXIT_STATUS = fprint(argv[argc-1], start, end);
        break;
    case CMD_REMOVE:
        /*
         * CASE 1: Removing the head (start = 0) or center
         * CASE 2: Removing the tail (end = FILE_SIZE) or entirety
         */
        if(end == FILE_SIZE)                              //CASE 1
            EXIT_STATUS = fremove(argv[argc-1], start, end, FILE_SIZE, 2);
        else                                              //CASE 2
            EXIT_STATUS = fremove(argv[argc-1], start, end, FILE_SIZE, 1);

        break;
    case CMD_KEEP:
        EXIT_STATUS = fkeep(argv[argc-1], start, end, FILE_SIZE);
        break;
    case CMD_EXPUNGE:
        /* TODO: call expunge function */
        break;
    case CMD_INSERT:
        /* TODO: call insert function */
        break;
    default:
        mu_die("unexpected cmd: %u", cmd);
    }

    exit(EXIT_STATUS);
}
