#define _GNU_SOURCE

#include <sys/types.h>

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
#include <signal.h>
#include <time.h>

#include "list.h"
#include "mu.h"

#define MY_SIG (SIGRTMIN+2) //36

#define USAGE \
    "Usage: mcron [-h] [-l LOG_FILE] CONFIG_FILE \n" \
    "\n" \
    "The mcron utility logs commands based on a user-supplied scheudle.\n" \
    "\n" \
    "Optional Arguments:\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit with status 0.\n" \
    "\n" \
    "   -l, --log-file LOG_FILE\n" \
    "       Use LOG_FILE as the log file. If LOG_FILE already exists, it is truncated and overwritten. If LOG_FILE is a path, the intermediate directories must already exist.\n" \
    "       If this option is not specified, then the default is to creat a file called mcron.log in the working directory.\n" \

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

static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}

struct job {
    struct list_head list;
    char *cmd;
    int number; //the associated job number
    struct itimerspec itimerspec;
    timer_t timer_id;
};

struct schedule {
    struct list_head head;
    /* ... any other fields you might want ... */
};

static struct job *
job_new(const char *cmd, unsigned int secs, unsigned int num)
{
    struct job *job = mu_zalloc(sizeof(struct job));

    job->cmd = mu_strdup(cmd);
    job->itimerspec.it_interval.tv_sec = secs;   
    job->number = num;

    return job;
}

static void
job_free(struct job *job)
{
    free(job->cmd);
    free(job);
}


/* Return NULL on invalid configuration line */
static struct job *
job_from_config_line(char *line, unsigned int num)
{
    char *p = line;
    char *cmd;
    bool found_space = false;
    unsigned int secs;
    int err;

    mu_str_chomp(line);

    while (*p) {
        if (isspace(*p)) {
            found_space = true;
            break;
        }
        p++;
    }

    if (!found_space)
        return NULL;

    *p = '\0';
    cmd = p+1;

    err = mu_str_to_uint(line, 10, &secs);
    if (err != 0)
        return NULL;

    return job_new(cmd, secs, num);
}

/*
 * On success, return the number of lines read.
 * On failure, return a negative errno value.
 */
static ssize_t
read_config(const char *path, struct schedule* schedule)
{
    FILE *fh;
    ssize_t len = 0;
    size_t n = 0;
    char *line = NULL;
    ssize_t ret = 0;

    struct job* job;

    fh = fopen(path, "r");
    if (fh == NULL) {
        ret = -errno;
        goto out;
    }

    while (1) {
        errno = 0;
        len = getline(&line, &n, fh);
        if (len == -1) {
            if (errno != 0)
                ret = -errno;
            goto out;
        }

        job = job_from_config_line(line, ret);
        list_add(&job->list, &schedule->head);

        ret++;
    }

out:
    free(line);
    //job_free(job);
    if (fh != NULL)
        fclose(fh);

    return ret;
}

static void
group_init(struct schedule *schedule)
{
    INIT_LIST_HEAD(&schedule->head);
}

#if 0
static void
group_deinit(struct schedule *schedule)
{
    struct job *job, *tmp;

    list_for_each_entry_safe(job, tmp, &schedule->head, list) {
        list_del(&job->list);
        job_free(job);
    }
}
#endif

/*
 * Write the UTC timestamp for the current time into `buf`.  `buf_size` is
 * the size in bytes of `buf`, which must be large enough to hold the
 * timestamp and its terminating nul-byte.  If `buf` is not large enough,
 * the function terminates the process.
 */
void
timestamp_utc(void *buf, size_t buf_size)
{
    time_t t;
    struct tm tm;
    size_t n;

    time(&t);
    gmtime_r(&t, &tm);
    n = strftime(buf, buf_size, "%Y/%m/%d %H:%M:%S UTC", &tm);
    if (n == 0)
        mu_die("strftime");
} 

static void
create_pid() {
    FILE *fh;
    //char * str;

    //pid_t pid = getpid();
    //sprintf(str, "%d", pid);

    fh = fopen("mcron.pid", "w+");
    fprintf(fh, "%d\n", getpid());

    fclose(fh);
}

static void
arm(struct job *job)
{
    job->itimerspec.it_value = job->itimerspec.it_interval;
    timer_settime(job->timer_id, 0, &job->itimerspec, NULL);
}

static int
create_timers(struct schedule* schedule)
{
    struct job *job, *tmp;
    struct sigevent ev;
    int err;
    
    list_for_each_entry_safe(job, tmp, &schedule->head, list) {
        memset(&ev, 0x00, sizeof(ev));
        ev.sigev_notify = SIGEV_SIGNAL;
        ev.sigev_signo = MY_SIG;
        ev.sigev_value.sival_ptr = job;

        err = timer_create(CLOCK_REALTIME, &ev, &job->timer_id);
        if (err == -1)
            mu_die_errno(errno, "timer_create");

        arm(job);
    }

    return 0;
}

static void
destroy_timers(struct schedule* schedule)
{
    int err;
    struct job *job, *tmp;
    
    list_for_each_entry_safe(job, tmp, &schedule->head, list) {
        err = timer_delete(job->timer_id);
        if (err == -1)
            mu_die_errno(errno, "timer_delete");

        list_del(&job->list);
        job_free(job);
    }
}

static void
run(const char *path, char *log_file, int delay) {
    //arm the timers, create the signal timers using #define, then use a switch statment to do case handling. Also mask th
    //create the timers, put them into a set, mask the set, arm the timers, sigwaitinfo to handle input
    sigset_t set;
    siginfo_t info;
    int signo;
    unsigned int log_num = 0;

    //time_t now;
    struct job* job;
    struct schedule* schedule = malloc(sizeof(schedule));
    
    group_init(schedule);
    //group_init(&timers);
    read_config(path, schedule);

    //open mcron.log to write to
    FILE *fh;
    char buf[1024];

    fh = fopen(log_file, "w+");
    if (fh == NULL)
        mu_die_errno(errno, "can't create log file");
    
    setvbuf(fh, NULL, _IOLBF, 0);
    
    //create and add all the signals into the set
    sigemptyset(&set);
    sigaddset(&set, MY_SIG);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGHUP);

    sigprocmask(SIG_BLOCK, &set, NULL);

    if(delay > 0) 
        sleep(delay);

    //create and arm the timers
    create_timers(schedule);

    while(1) {
        signo = sigwaitinfo(&set, &info);

        switch(signo) {
        case SIGTERM:
            unlink("mcron.pid");
            fclose(fh);
            exit(0);
            break;
        case SIGINT:
            unlink("mcron.pid");
            fclose(fh);
            exit(0);
            break;
        case SIGUSR1:
            ; char fname[128] = {0};
            sprintf(fname, "%s-%d", log_file, log_num);

            fclose(fh);
            rename(log_file, fname);
            fh = fopen(log_file, "w");
            setvbuf(fh, NULL, _IOLBF, 0);

            log_num++;
            break;
        case SIGHUP:
            destroy_timers(schedule);       //destroy and disarm timers
            //group_deinit(schedule);       //destroy the job schedule
            
            //schedule = malloc(sizeof(schedule));
            //group_init(schedule);
            read_config(path, schedule);
            create_timers(schedule);        //recreate and rearm timers
            break;
        default:
            if(signo == MY_SIG) {
                job = info.si_value.sival_ptr;
                timestamp_utc(buf, sizeof(buf));
                fprintf(fh, "%s %d %s\n", buf, job->number, job->cmd);
            }
            break;
        }
    }
}

int
main(int argc, char *argv[])
{
    int opt;
    /*
     * An option that takes a required argument is followed by a ':'.
     * The leading ':' suppresses getopt_long's normal error handling.
     */
    const char *short_opts = ":hd:l::";
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"log-file", required_argument, NULL, 'l'},
        {NULL, 0, NULL, 0}
    };

    int ret = 0, delay = 0;
    char *log_file = "mcron.log";
    
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
        case 'd':
            ret = mu_str_to_int(optarg, 10, &delay);
            if (ret != 0)
                die_errno(-ret, "invalid value for --before-context: \"%s\"", optarg);

            break;
        case 'l':
            ; FILE *fh;

            //fopen checks all conditions, otherwise returns NULL and our default log_file will be name "mcron.log"
            fh = fopen(optarg, "w+");

            if (fh != NULL) {
                log_file = optarg;
                fclose(fh);
            }
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

    if(fopen(argv[argc-1], "r") == NULL) {
        exit(-1);
    }

    create_pid();
    run(argv[argc-1], log_file, delay);

    return 0;
}