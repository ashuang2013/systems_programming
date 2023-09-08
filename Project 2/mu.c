#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "mu.h"

/* 
 * On success, return 0 and set val to the parsed value.
 * On failure, return a negative errno value.
 */
int
mu_str_to_long(const char *s, int base, long *val)
{
    char *endptr;

    errno = 0;
    *val = strtol(s, &endptr, base);
    if (errno != 0) {
        /* EINVAL for bad base, or ERANGE for value to big or small */
        return -errno;
    }

    if (endptr == s) {
        /* no digits at all -- not a number */
        return -EINVAL;
    }

    if (*endptr != '\0') {
        /* trailing garbage */
        return -EINVAL;
    }

    return 0;
}


/* 
 * On success, return 0 and set val to the parsed value.
 * On failure, return a negative errno value.
 */
int
mu_str_to_int(const char *s, int base, int *val)
{
    int ret;
    long tmp;

    ret = mu_str_to_long(s, base, &tmp); 
    if (ret < 0)
        return ret;

    if (tmp < INT_MIN || tmp > INT_MAX)
        return -ERANGE;

    *val = (int)tmp;
    return 0;
}


/*
 * Read `count` bytes from fd.
 *
 * On success, return 0.  On failure, return a negative errno.  In either case,
 * if the `total` argument is non-NULL, store the total number of bytes read.
 * Thus, if the function returns 0 and total == count, then all bytes were
 * read, but if total < count, then EOF was reached before reading all
 * requested bytes.
 *
 * The read is restarted in the event of interruption.
 */
int
mu_read_n(int fd, void *data, size_t count, size_t *total)
{
    int err = 0;
    ssize_t n;
    size_t avail = count;
    size_t tot = 0;

    do {
retry:
        n = read(fd, (uint8_t *)data + tot, avail);
        if (n == -1) {
            if (errno == EINTR) {
                goto retry;
            } else {
                err = -errno;
                goto out;
            }
        } else if (n == 0) {
            goto out;
        } else {
            avail -= (size_t)n;
            tot += (size_t)n;
        }
    } while (avail);

out:
    if (total != NULL)
        *total = tot;
    return err;
}


/*
 * Read `count` bytes from fd without changing the file's offset.
 *
 * On success, return 0.  On failure, return a negative errno.  In either case,
 * if the `total` argument is non-NULL, store the total number of bytes read.
 * Thus, if the function returns 0 and total == count, then all bytes were
 * read, but if total < count, then EOF was reached before reading all
 * requested bytes.
 *
 * The read is restarted in the event of interruption.
 */
int
mu_pread_n(int fd, void *data, size_t count, off_t offset, size_t *total)
{
    int err = 0;
    ssize_t n;
    size_t avail = count;
    size_t tot = 0;

    do {
retry:
        n = pread(fd, (uint8_t *)data + tot, avail, offset + (ssize_t)tot);
        if (n == -1) {
            if (errno == EINTR) {
                goto retry;
            } else {
                err = -errno;
                goto out;
            }
        } else if (n == 0) {
            goto out;
        } else {
            avail -= (size_t)n;
            tot += (size_t)n;
        }
    } while (avail);

out:
    if (total != NULL)
        *total = tot;
    return err;
}


/*
 * Write `count` bytes to fd.
 *
 * On success, return 0.  On failure, return a negative errno.  In either case,
 * if the `total` argument is non-NULL, store the total number of bytes written.
 *
 * The write is restarted in the event of interruption.
 */
int
mu_write_n(int fd, const void *data, size_t count, size_t *total)
{
    int err = 0;
    ssize_t n = 0;
    size_t left = count;
    size_t tot = 0;

    do {
retry:
        n = write(fd, (uint8_t *)data + tot, left);
        if (n == -1) {
            if (errno == EINTR) {
                goto retry;
            } else {
                err = -errno;
                goto out;
            }
        } else {
            left -= (size_t)n;
            tot += (size_t)n;
        }
    } while (left);

out:
    if (total != NULL)
        *total = tot;
    return err;
}


/*
 * Write `count` bytes to fd without changing file offset.
 *
 * On success, return 0.  On failure, return a negative errno.  In either case,
 * if the `total` argument is non-NULL, store the total number of bytes written.
 *
 * The write is restarted in the event of interruption.
 */
int
mu_pwrite_n(int fd, const void *data, size_t count, off_t offset, size_t *total)
{
    int err = 0;
    ssize_t n = 0;
    size_t left = count;
    size_t tot = 0;

    do {
retry:
        n = pwrite(fd, (uint8_t *)data + tot, left, offset + (ssize_t)tot);
        if (n == -1) {
            if (errno == EINTR) {
                goto retry;
            } else {
                err = -errno;
                goto out;
            }
        } else {
            left -= (size_t)n;
            tot += (size_t)n;
        }
    } while (left);

out:
    if (total != NULL)
        *total = tot;
    return err;
}
