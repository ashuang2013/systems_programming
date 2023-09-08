#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "list.h"

#define USAGE \
    "Usage: sgrep [-c] [-h] [-n] [-q] [-B NUM] STR FILE \n" \
    "\n" \
    "Print lines in FILE that match PATTERN.\n" \
    "\n" \
    "Optional Arguments:\n" \
    "   -c, --count\n" \
    "       Suppress normal output; instead print a count of matching lines for the input file.\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit.\n" \
    "\n" \
    "   -n, --line-number\n" \
    "       Prefix each line of output with the 1-based line number of the file, followed immediately by a colon.\n" \
    "   -q, --quiet\n" \
    "       Do not write anything to stdout. Exit immediate with zero status if any match was found.\n" \
    "       If a match is not found, exit with a non-zero status.\n" \
    "\n" \
    "   -B NUM, --before-context NUM\n" \
    "       Print NUM lines of leading context before matching lines.\n"

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

/*
 * On success, return 0 and set val to the parsed value.
 * On failure, return a negative errno value.
 */
static int
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
static int
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

static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}

struct Arguments {
    bool count;
    bool help;
    bool line_number;
    bool quiet;
    bool ignore_case;
    bool invert_match;
    size_t before_context;
};

struct node {
    struct list_head list;
    char *line;
    int line_number;
};

struct queue {
    struct list_head head;
    size_t size;
    /* ... any other fields you might want ... */
};

#define mu_die(fmt, ...) \
    do { \
        fprintf(stderr, "[die] %s:%d " fmt "\n", \
                __func__, __LINE__,##__VA_ARGS__); \
        exit(1); \
    } while (0)

static void *
mu_malloc(size_t n)
{
    void *p;

    p = malloc(n);
    if (p == NULL)
        mu_die("out of memory");

    return p;
}

static char *
mu_strdup(const char *s)
{
    char *p;

    p = strdup(s);
    if (p == NULL)
        mu_die("out of memory");

    return p;
}

static struct node *
node_new(const char *line, int line_number)
{
    struct node *node;

    node = mu_malloc(sizeof(*node));
    node->line = mu_strdup(line);
    node->line_number = line_number;

    return node;
}

static void
node_free(struct node *node)
{
    free(node->line);
    free(node);
}

static void
queue_init(struct queue *queue)
{
    INIT_LIST_HEAD(&queue->head);
    queue->size = 0;
}

static void
queue_print(const struct queue *queue, struct Arguments args)
{
    struct node *node;

    if(args.line_number == true) {
        list_for_each_entry(node, &queue->head, list) {
            printf("%d:%s", node->line_number, node->line);
        }
    }
    else {
        list_for_each_entry(node, &queue->head, list) {
            printf("%s", node->line);
        }
    }
}

static void
queue_insert(struct queue *queue, struct node *node)
{
    list_add_tail(&node->list, &queue->head);
    queue->size += 1;
}

static struct node *
queue_remove(struct queue *queue)
{
    struct node *node;

    node = list_first_entry_or_null(&queue->head, struct node, list);
    if (node == NULL)
        mu_die("queue_remove: empty queue");

    list_del(&node->list);
    queue->size -= 1;

    return node;
}

static size_t
queue_size(const struct queue *queue)
{
    return queue->size;
}

static void
queue_deinit(struct queue *queue)
{
    struct node *node, *tmp;

    list_for_each_entry_safe(node, tmp, &queue->head, list) {
        list_del(&node->list);
        node_free(node);
    }

    queue->size = 0;
}

/*
 * On success, return the number of lines read.
 * On failure, return a negative errno value.
 */
static ssize_t
read_lines(const char *path, const char *str, struct Arguments args)
{
    FILE *fh;
    ssize_t len = 0;
    size_t n = 0;
    char *line = NULL;
    ssize_t ret = 0;

    fh = fopen(path, "r");
    if (fh == NULL) {
        ret = -errno;
        die("no such file exists");
        goto out;
    }

    //--count
    int count = 0;    
    if(args.count == true) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;

                printf("%d\n", count);
                ret = (count > 0) ? 0 : 1;
                goto out;
            }

            if(strstr(line, str)) {
                count++;
            }
        }
    }

    //--quiet
    if(args.quiet == true) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;
                
                ret = 1;
                goto out;
            }
            
            if(strstr(line, str)) {
                ret = 0;
                goto out;
            }
        }
    }

    //--ignore-case
    if(args.ignore_case == true) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;
                
                ret = 0;
                goto out;
            }

            if(strcasestr(line, str)) {
                args.line_number == true ? printf("%zd:%s", ret+1, line) : printf("%s", line);
            }

            ret++;
        }
    }

    //--invert-match
    if(args.invert_match == true) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;
                
                ret = 1;
                goto out;
            }
            
            if(!strstr(line, str)) {
                args.line_number == true ? printf("%zd:%s", ret+1, line) : printf("%s", line);
            }

            ret++;
        }
    }

    struct queue queue;
    struct node *node;

    queue_init(&queue);

    //--before-context NUM
    if(args.before_context > 0) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;

                queue_deinit(&queue);
                goto out;
            }

            node = node_new(line, ret+1);

            //we have found our target 
            if(strstr(line, str)) {
                queue_print(&queue, args);
                args.line_number == true ? printf("%zd:%s", ret+1, line) : printf("%s", line);
            }

            //we have not reached the before_context threshold in items yet
            if(queue_size(&queue) < args.before_context)
                queue_insert(&queue, node);
            else {
                queue_remove(&queue);
                queue_insert(&queue, node);
            }

            ret++;
        }
    }

    //--line-number
    if(args.line_number == true) {
        while (1) {
            errno = 0;
            len = getline(&line, &n, fh);
            if (len == -1) {
                if (errno != 0)
                    ret = -errno;
                
                ret = 0;
                goto out;
            }
            
            if(strstr(line, str)) {
                printf("%zd:%s", ret+1, line);
            }
            ret++;
        }
    }

out:
    free(line);
    if (fh != NULL)
        fclose(fh);

    return ret;
}

int
main(int argc,char *argv[])
{
    int opt, nargs;
    /*
     * An option that takes a required argument is followed by a ':'.
     * The leading ':' suppresses getopt_long's normal error handling.
     */
    const char *short_opts = ":chnqivB:";
    struct option long_opts[] = {
        {"count", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {"line-number", no_argument, NULL, 'n'},
        {"quiet", no_argument, NULL, 'q'},
        {"ignore-case", no_argument, NULL, 'i'},
        {"invert-match", no_argument, NULL, 'v'},
        {"before-context", required_argument, NULL, 'B'},
        {NULL, 0, NULL, 0}
    };

    struct Arguments arguments = {false, false, false, false, false, false, 0};

    int before_context = -1;
    int ret = 0;
    ssize_t EXIT_STATUS = 0;
    
    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (opt == -1) {
            /* processed all command-line options */
            break;
        }

        switch (opt) {
        case 'c':
            arguments.count = true;
            break;
        case 'h':
            arguments.help = true;
            usage(0);
            return 0;
        case 'n':
            arguments.line_number = true;
            break;
        case 'q':
            arguments.quiet = true;
            break;
        case 'i':
            arguments.ignore_case = true;
            break;
        case 'v':
            arguments.invert_match = true;
            break;
        case 'B':
            ret = mu_str_to_int(optarg, 10, &before_context);
            if (ret != 0)
                die_errno(-ret, "invalid value for --before-context: \"%s\"", optarg);

            arguments.before_context = before_context;
            break;
        case '?':
            die("unknown option '%c' (decimal: %d)", optopt, optopt);
            break;
        case ':':
            die("missing option argument for option %c", optopt);
            break;
        default:
            die("unexpected getopt_long return value: %c\n", (char)opt);
        }
    }

    /*
     * optind is the index in argv of the first non-option (that is, the first
     * positional argument).
     */
    nargs = argc - optind;
    if (nargs != 2)
        die("expected two positional arguments, but found %d", nargs);

    EXIT_STATUS = read_lines(argv[argc-1], argv[argc-2], arguments);
    //printf("ignore_case: %s\n", ignore_case ? "true" : "false");
    //printf("max_count: %d\n", max_count);
    //printf("PATTERN: \"%s\"\n", argv[optind]);
    //printf("FILE: \"%s\"\n", argv[optind + 1]);

    exit(EXIT_STATUS);
}