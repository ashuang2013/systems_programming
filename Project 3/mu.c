#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "mu.h"


void *
mu_calloc(size_t nmemb, size_t size)
{
    void *p;

    p = calloc(nmemb, size);
    if (p == NULL)
        mu_die("out of memory");

    return p;
}


void *
mu_zalloc(size_t n)
{
    return mu_calloc(1, n);
}


char *
mu_strdup(const char *s)
{
    char *p;

    p = strdup(s);
    if (p == NULL)
        mu_die("out of memory");

    return p;
}


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


int
mu_str_to_uint(const char *s, int base, unsigned int *val)
{
    int ret;
    long tmp = 0;

    ret = mu_str_to_long(s, base, &tmp); 
    if (ret < 0)
        return ret;

    if (tmp < 0 || tmp > UINT_MAX)
        return -ERANGE;

    *val = (unsigned int)tmp;
    return 0;
}


/*
 * If the last char of `s` is a newline, remove it (overwrite it with a
 * nul-byte).
 *
 * Return 1 if a newline was removed, and 0 otherwise.
 */
size_t
mu_str_chomp(char *s)
{
    size_t len = strlen(s);

    if ((len > 0) && (s[len-1] == '\n')) {
        s[len-1] = '\0';
        return 1;
    } else {
        return 0;
    }
}


/*
 * mu_strlcpy and mu_strlcat are taken from OpenBSD 6.2's
 * lib/libc/string/strlcpy.c and lib/libc/string/strlcat.c, respectively.
 */

/*
 * Copy string src to buffer dst of size dsize.  At most dsize-1
 * chars will be copied.  Always NUL terminates (unless dsize == 0).
 * Returns strlen(src); if retval >= dsize, truncation occurred.
 */
size_t
mu_strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc = src;
	size_t nleft = dsize;

	/* Copy as many bytes as will fit. */
	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}

	/* Not enough room in dst, add NUL and traverse rest of src. */
	if (nleft == 0) {
		if (dsize != 0)
			*dst = '\0';		/* NUL-terminate dst */
		while (*src++)
			;
	}

	return(src - osrc - 1);	/* count does not include NUL */
}

/*
 * Appends src to string dst of size dsize (unlike strncat, dsize is the
 * full size of dst, not space left).  At most dsize-1 characters
 * will be copied.  Always NUL terminates (unless dsize <= strlen(dst)).
 * Returns strlen(src) + MIN(dsize, strlen(initial dst)).
 * If retval >= dsize, truncation occurred.
 *
 * d = "abcd" 0 0 0 0
 * strlcpy(d, 8, "ef")  This would return 6
 * 8 - 4 - 1 = 3
 */
size_t
mu_strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dsize;
	size_t dlen;

	/* Find the end of dst and adjust bytes left but don't go past end. */
	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = dst - odst;
	n = dsize - dlen;

	if (n-- == 0)
		return(dlen + strlen(src));
	while (*src != '\0') {
		if (n != 0) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return(dlen + (src - osrc));	/* count does not include NUL */
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
