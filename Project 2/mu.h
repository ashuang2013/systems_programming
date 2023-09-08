#ifndef _MU_H_
#define _MU_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MU_UNUSED(x) do { (void)(x); } while (0)

#define MU_MIN(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

#define MU_PRI_off         "ld"        /* long int (s64) */

#define mu_panic(fmt, ...) \
    do { \
        fprintf(stderr, "[panic] %s:%d " fmt "\n", \
                __func__, __LINE__,##__VA_ARGS__); \
        exit(1); \
    } while (0)

#define mu_die(fmt, ...) \
    do { \
        fprintf(stderr, fmt "\n",##__VA_ARGS__); \
        exit(1); \
    } while (0)

#define mu_die_errno(errnum, fmt, ...) \
    do { \
        fprintf(stderr, fmt ": %s\n",##__VA_ARGS__, strerror(errnum)); \
        exit(1); \
    } while (0)

#define mu_stderr(fmt, ...) \
        fprintf(stderr, fmt "\n",##__VA_ARGS__);

#define mu_stderr_errno(errnum, fmt, ...) \
        fprintf(stderr, fmt ": %s\n",##__VA_ARGS__, strerror(errnum));

int mu_str_to_long(const char *s, int base, long *val);
int mu_str_to_int(const char *s, int base, int *val);

int mu_read_n(int fd, void *data, size_t count, size_t *total);
int mu_pread_n(int fd, void *data, size_t count, off_t offset, size_t *total);

int mu_write_n(int fd, const void *data, size_t count, size_t *total);
int mu_pwrite_n(int fd, const void *data, size_t count, off_t offset, size_t *total);

#endif /* _MU_H_ */