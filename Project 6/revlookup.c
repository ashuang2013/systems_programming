#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xpthread.h"
#include "mu.h"
#include "uthash.h"

#define USAGE \
    "Usage:` revlookup [-h] [-q MAX_QUEUE_SIZE] [-t NUM_THREADS] IP_LIST_FILE\n" \
    "\n" \
    "Lookup the domain names for a list of IPv4 addresses.\n" \
    "\n" \
    "optional arguments\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit.\n" \
    "\n" \
    "   -q, --max-queue-size MAX_QUEUE_SIZE\n" \
    "       The maximum number of IPv4 addresses that the circular queue can store at one time.\n" \
    "       The main thread inserts each IP address from IP_LIST_FILE into this queue. \n" \
    "       The default is 10, and --max-queue-size must be greater than 0. \n" \
    "\n" \
    "   -t, --threads NUM_THREADS\n" \
    "       The number of worker threads to create and use. \n" \
    "       Each worker thread attempts to dequeue an IPv4 address from the queue and perform a DNS reverse lookup to resolve the address to a domain name. \n" \
    "       The default is 1, and --threads must be greater than 0. \n" \

#define die_errno(errnum, fmt, ...) \
    do { \
        fprintf(stderr, "[die] %s:%d " fmt ": %s\n", \
                __func__, __LINE__,##__VA_ARGS__, strerror(errnum)); \
        exit(1); \
    } while (0)

static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}


struct ipdomain {
    char ip_key[INET_ADDRSTRLEN]; /*key */
    char domain[NI_MAXHOST];
    UT_hash_handle hh;
};


struct ipdomain_hashtable {
    struct ipdomain *nodes;
    pthread_mutex_t lock;
};


struct ipdomain_hashtable *g_ipdomain_ht = NULL;


struct tpool {
    /* circular queue: an array where each element is a char[INET_ADDRSTRLEN] */
    char (*queue)[INET_ADDRSTRLEN];
    size_t max_queue_size;
    size_t sidx;
    size_t eidx; /* exclusive */
    /* 
     * need an explicit count of items in queue to disambiguate sidx == edix, which could
     * mean either empty or full
     */
    size_t queue_size;  

    /* queue locking and signaling */
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty; /* producer inserts to empty queue */
    pthread_cond_t queue_not_full;  /* consumer dequeues from full queue */
    pthread_cond_t queue_empty;     /* consumer dequeues and makes queue empty */
    bool shutdown;                  /* queue is empty and producer has no more data */

    /* worker threads */
    size_t num_threads;
    pthread_t *threads;
};


struct worker_arg {
    struct tpool *tpool;
    unsigned int id;
};


static struct ipdomain *
ipdomain_new(const char *ip_str, const char *domain)
{
    MU_NEW(ipdomain, node);
    size_t len;

    len = mu_strlcpy(node->ip_key, ip_str, sizeof(node->ip_key));
    assert(len < sizeof(node->ip_key));

    len = mu_strlcpy(node->domain, domain, sizeof(node->domain));
    assert(len < sizeof(node->domain));

    return node;
}


static void
ipdomain_free(struct ipdomain *node)
{
    free(node);
}


static struct ipdomain_hashtable *
ipdomain_hashtable_new(void)
{
    MU_NEW(ipdomain_hashtable, ht);
    pthread_mutexattr_t attr;

    ht->nodes = NULL;

    xpthread_mutexattr_init(&attr);
    xpthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    xpthread_mutex_init(&ht->lock, &attr);
    xpthread_mutexattr_destroy(&attr);

    return ht;

}

static void
ipdomain_hashtable_free(struct ipdomain_hashtable *ht)
{
    struct ipdomain *node, *tmp;

    HASH_ITER(hh, ht->nodes, node, tmp) {
        HASH_DEL(ht->nodes, node);
        ipdomain_free(node);
    }

    xpthread_mutex_destroy(&ht->lock);
    free(ht);
}


static bool
ipdomain_hashtable_has(struct ipdomain_hashtable *ht, const char *ip_str)
{
    bool found = false;
    struct ipdomain *node = NULL;

    xpthread_mutex_lock(&ht->lock);

    HASH_FIND_STR(ht->nodes, ip_str, node);
    if (node != NULL)
        found = true;

    xpthread_mutex_unlock(&ht->lock);

    return found;
}


static void
ipdomain_hashtable_insert(struct ipdomain_hashtable *ht, const char *ip_str, const char *domain)
{
    struct ipdomain *node;

    xpthread_mutex_lock(&ht->lock);

    HASH_FIND_STR(ht->nodes, ip_str, node);
    if (node != NULL)
        goto done;

    node = ipdomain_new(ip_str, domain);
    HASH_ADD_STR(ht->nodes, ip_key, node);

done:
    xpthread_mutex_unlock(&ht->lock);
}


static void
ipdomain_hashtable_print(const struct ipdomain_hashtable *ht)
{
    size_t i = 1;
    struct ipdomain *node, *tmp;

    HASH_ITER(hh, ht->nodes, node, tmp) {
        printf("%6zu: %s => %s\n", i, node->ip_key, node->domain);
        i++;
    } 
}


/* For all tpool_queue_* functions, the caller must hold tpool->queue_lock */

static size_t
tpool_queue_size(const struct tpool *tpool)
{
    return tpool->queue_size;
}


static bool
tpool_queue_is_empty(const struct tpool *tpool)
{
    return tpool->queue_size == 0;
}


static bool
tpool_queue_is_full(const struct tpool *tpool)
{
    return tpool->queue_size == tpool->max_queue_size;
}


/* Precondition: queue is not empty */
static void
tpool_queue_dequeue(struct tpool *tpool, char *dst, size_t dst_size)
{
    size_t len;

    assert(!tpool_queue_is_empty(tpool));

    len = mu_strlcpy(dst, tpool->queue[tpool->sidx], dst_size);
    assert(len < dst_size);

    tpool->sidx = (tpool->sidx + 1) % tpool->max_queue_size;
    tpool->queue_size--;
}


/* Precondition: queue is not full */
static void
tpool_queue_insert(struct tpool *tpool, char *ip_str)
{
    size_t len;

    assert(!tpool_queue_is_full(tpool));

    len = mu_strlcpy(tpool->queue[tpool->eidx], ip_str, INET_ADDRSTRLEN);
    assert(len < INET_ADDRSTRLEN);

    tpool->eidx = (tpool->eidx + 1) % tpool->max_queue_size;
    tpool->queue_size++;
}


static struct worker_arg *
worker_arg_new(struct tpool *tpool, unsigned int id)
{
    MU_NEW(worker_arg, w);

    w->tpool = tpool;
    w->id = id;

    return w;
}


static void
worker_arg_free(struct worker_arg *w)
{
    free(w);
}


static void *
tpool_worker(void *arg /* worker_arg */)
{
    char ip_str[INET_ADDRSTRLEN] = {0};
    struct worker_arg *w = arg;
    struct tpool *tpool = w->tpool;

    for(;;) {
        xpthread_mutex_lock(&tpool->queue_lock);
        mu_pr_debug("worker %u: waiting for work", w->id);

        while(tpool_queue_is_empty(tpool) && !tpool->shutdown) 
            xpthread_cond_wait(&tpool->queue_not_empty, &tpool->queue_lock);

        if(tpool->shutdown) {
            xpthread_mutex_unlock(&tpool->queue_lock);
            mu_pr_debug("worker %u: exiting", w->id);
            worker_arg_free(w);
            pthread_exit(NULL);
        }

        tpool_queue_dequeue(tpool, ip_str, sizeof(ip_str));
        mu_pr_debug("worker %u: take %s", w->id, ip_str);

        /*
         * if queue was full and now it's not, signal so that the producer 
         * knows that it can now insert into the queue
         */
        if(tpool_queue_size(tpool) == tpool->max_queue_size-1)
            xpthread_cond_signal(&tpool->queue_not_full);

        /*
         * if queue was is not empty, signal so that, if the producer has 
         * no more data to insert, ti knows that the queue has been drained
         */          
        if(tpool_queue_is_empty(tpool))
            xpthread_cond_signal(&tpool->queue_empty);

        xpthread_mutex_unlock(&tpool->queue_lock);

        /* check if ip_str is in the hashtable, if it is continue */
        if(ipdomain_hashtable_has(g_ipdomain_ht, ip_str)) {
            continue;
        }
        else {
            /*
            * - getnameinfo to resolve ip to domain name
            * - insert the (ip_str, domain_name) into the hashtable
            */

            struct sockaddr_in addr;     /* input */
            socklen_t addrlen;           /* input */
            char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

            addr.sin_family = AF_INET; 
            addr.sin_port = htons(0); 
            inet_aton(ip_str, &addr.sin_addr);

            addrlen = sizeof(addr);

            getnameinfo((const struct sockaddr *)&addr, addrlen, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), 0);
           
            ipdomain_hashtable_insert(g_ipdomain_ht, ip_str, hbuf);
        }
    }

    worker_arg_free(w);

    return NULL;
}


static void
tpool_add_work(struct tpool *tpool, char *ip_str)
{
    xpthread_mutex_lock(&tpool->queue_lock);

    while(tpool_queue_is_full(tpool))
        xpthread_cond_wait(&tpool->queue_not_full, &tpool->queue_lock);

    mu_pr_debug("manager: add %s", ip_str);
    tpool_queue_insert(tpool, ip_str);
    xpthread_cond_signal(&tpool->queue_not_empty);

    xpthread_mutex_unlock(&tpool->queue_lock);
}


static void
tpool_wait_finish(struct tpool *tpool)
{
    size_t i;

    xpthread_mutex_lock(&tpool->queue_lock);

    while(!tpool_queue_is_empty(tpool))
        xpthread_cond_wait(&tpool->queue_empty, &tpool->queue_lock);

    mu_pr_debug("manager: queue empty; shutting down");
    tpool->shutdown = true;

    /* signal any workers that are still sleeping on arrival of new data */
    xpthread_cond_broadcast(&tpool->queue_not_empty);
    xpthread_mutex_unlock(&tpool->queue_lock);

    /* wait for worker to exit */
    mu_pr_debug("manager: waiting for workers to exit");
    for(i=0; i<tpool->num_threads; i++) 
        xpthread_join(tpool->threads[i], NULL);
    
    mu_pr_debug("manager: all workers exited");
}


static struct tpool *
tpool_new(size_t num_worker_threads, size_t max_queue_size)
{
    MU_NEW(tpool, tpool);
    pthread_mutexattr_t attr;
    struct worker_arg *w;
    unsigned int i;

    tpool->num_threads = num_worker_threads;
    tpool->max_queue_size = max_queue_size;
    tpool->queue = mu_mallocarray(max_queue_size, INET_ADDRSTRLEN);

    xpthread_mutexattr_init(&attr);
    xpthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    xpthread_mutex_init(&tpool->queue_lock, NULL);
    xpthread_mutexattr_destroy(&attr);

    xpthread_cond_init(&tpool->queue_not_empty, NULL);
    xpthread_cond_init(&tpool->queue_not_full, NULL);
    xpthread_cond_init(&tpool->queue_empty, NULL);

    tpool->threads = mu_mallocarray(num_worker_threads, sizeof(pthread_t));
    for (i = 0; i < num_worker_threads; i++) {
        w = worker_arg_new(tpool, i);
        mu_pr_debug("manager: spawning worker %u", w->id);
        xpthread_create(&tpool->threads[i], NULL, tpool_worker, w);
    }

    return tpool;
}


static void
tpool_free(struct tpool *tpool)
{
    xpthread_mutex_destroy(&tpool->queue_lock);
    xpthread_cond_destroy(&tpool->queue_not_empty);
    xpthread_cond_destroy(&tpool->queue_not_full);
    xpthread_cond_destroy(&tpool->queue_empty);

    free(tpool->threads);
    free(tpool->queue);
    free(tpool);
}


static bool
is_ipv4_str(const char *s) {
    int err;
    struct sockaddr_in sai;

    err = inet_pton(AF_INET, s, &sai.sin_addr);
    if(err == 1) 
        return true;
    else if(err == 0)
        return false;
    else 
        mu_panic("inet_pton returned %d (%s)\n", err, strerror(errno));
}

static void
tpool_process_file(struct tpool *tpool, char *input_file)
{
    FILE *fh;

    size_t n = 0;
    ssize_t len = 0;

    char *line = NULL;

    MU_UNUSED(tpool);

    fh = fopen(input_file, "r");
    if(fh == NULL)
        mu_die_errno(errno, "can't open \"%s\"", input_file);

    while(1) {
        errno = 0;
        len = getline(&line, &n, fh);

        if(len == -1) {
            if(errno != 0)
                mu_die_errno(errno, "error reading \"%s\"", input_file);
            goto out;    
        }

        mu_str_chomp(line);

        if(!is_ipv4_str(line)) {
            mu_stderr("%s: invalid IPv4 string: \"%s\"", input_file, line);
            continue;
        }

        tpool_add_work(tpool, line);
    }

out:
    free(line);
    fclose(fh);
}


int 
main(int argc,char *argv[])
{
    int opt, nargs;
    const char *short_opts = ":hq:t:";
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"max-queue-size", required_argument, NULL, 'q'},
        {"threads", required_argument, NULL, 't'},
        {NULL, 0, NULL, 0}
    };

    int ret;
    int qsize = 10, threads = 1;

    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
        case 'h':   /* help */
            usage(0);
            break;
        case 'q':
            ret = mu_str_to_int(optarg, 10, &qsize);
            if (ret != 0)
                die_errno(-ret, "invalid value for --max-queue-size: \"%s\"", optarg);

            break;
        case 't':
            ret = mu_str_to_int(optarg, 10, &threads);
            if (ret != 0)
                die_errno(-ret, "invalid value for --threads: \"%s\"", optarg);

            break;
        case '?':
            mu_die("unknown option %c", optopt);
            break;
        case ':':
            mu_die("missing option argument for option %c", optopt);
            break;
        default:
            mu_panic("unexpected getopt_long return value: %c\n", (char)opt);
        }
    }

    struct tpool *tpool;

    nargs = argc - optind;
    if(nargs != 1)
        mu_die("Usage: %s [-h] [-q MAX_QUEUE_SIZE] [-t NUM_THREADS] IP_LIST_FILE ", argv[0]);

    g_ipdomain_ht = ipdomain_hashtable_new(); //TODO: would I be declring a new hashtable here?

    tpool = tpool_new(threads, qsize); 
    tpool_process_file(tpool, argv[optind]); //read in the file
    tpool_wait_finish(tpool);

    ipdomain_hashtable_print(g_ipdomain_ht);

    tpool_free(tpool);
    ipdomain_hashtable_free(g_ipdomain_ht);

    return 0;
}
