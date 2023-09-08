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


/* 
 * assumes LP64.  See:
 *  /usr/include/x86_64-linux/gnu/bits/typesizes.h
 *  /usr/include/x86_64-linux/gnu/bits/types.h
 */
#define MU_PRI_off          "ld"    /* long int (s64) */
#define MU_PRI_pid          "d"     /* int (s32) */
#define MU_PRI_time         "ld"    /* long int (s64) */


#define MU_LIMITS_MAX_TIMESTAMP_SIZE 64


#define mu_panic(fmt, ...) \
    do { \
        fprintf(stderr, "[panic] %s:%d " fmt "\n", \
                __func__, __LINE__,##__VA_ARGS__); \
        exit(1); \
    } while (0)

#define mu_panic_errno(fmt, ...) \
    do { \
        fprintf(stderr, "[panic] %s:%d " fmt ": %s\n", \
                __func__, __LINE__,##__VA_ARGS__, strerror(errnum)); \
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
        fprintf(stderr, fmt "\n",##__VA_ARGS__)

#define mu_stderr_errno(errnum, fmt, ...) \
        fprintf(stderr, fmt ": %s\n",##__VA_ARGS__, strerror(errnum))

void * mu_calloc(size_t nmemb, size_t size);
void * mu_zalloc(size_t n);
char * mu_strdup(const char *s);

int mu_str_to_long(const char *s, int base, long *val);
int mu_str_to_int(const char *s, int base, int *val);
int mu_str_to_uint(const char *s, int base, unsigned int *val);

size_t mu_str_chomp(char *s);
size_t mu_strlcpy(char *dst, const char *src, size_t dsize);
size_t mu_strlcat(char *dst, const char *src, size_t dsize);

int mu_read_n(int fd, void *data, size_t count, size_t *total);
int mu_pread_n(int fd, void *data, size_t count, off_t offset, size_t *total);
int mu_write_n(int fd, const void *data, size_t count, size_t *total);
int mu_pwrite_n(int fd, const void *data, size_t count, off_t offset, size_t *total);

#endif /* _MU_H_ */
