#define _GNU_SOURCE 

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "list.h"
#include "mu.h"


#define CMD_INITIAL_CAP_ARGS 8

#define USAGE \
    "Usage: bsh [-h] \n" \
    "\n" \
    "The bsh shell implements pipelines (|) and redirection of stdout (>) and stdin (<).\n" \
    "\n" \
    "Optional Arguments:\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit with status 0.\n" \
    "\n" \

#define die(fmt, ...) \
    do { \
        fprintf(stderr, "[die] %s:%d " fmt "\n", \
                __func__, __LINE__,##__VA_ARGS__); \
        exit(1); \
    } while (0)


struct cmd {
    struct list_head list;

    char **args;
    size_t num_args;
    size_t cap_args;

    pid_t pid;
};

struct pipeline {
    struct list_head head;  /* cmds */
    size_t num_cmds;
    char *in_file;
    char *out_file;
    char *append_file;
    bool append;
};


static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}


static struct cmd *
cmd_new(void)
{
    MU_NEW(cmd, cmd);
    size_t i;

    cmd->cap_args = CMD_INITIAL_CAP_ARGS;
    cmd->args = mu_mallocarray(cmd->cap_args, sizeof(char *));

    for (i = 0; i < cmd->cap_args; i++)
        cmd->args[i] = NULL;

    return cmd;
}


static void
cmd_push_arg(struct cmd *cmd, const char *arg)
{
    if (cmd->num_args == cmd->cap_args) {
        cmd->args = mu_reallocarray(cmd->args, cmd->cap_args * 2, sizeof(char *));
        cmd->cap_args *= 2;
    }

    cmd->args[cmd->num_args] = mu_strdup(arg);
    cmd->num_args += 1;
}


static void
cmd_pop_arg(struct cmd *cmd)
{
    assert(cmd->num_args > 0);

    free(cmd->args[cmd->num_args - 1]);
    cmd->args[cmd->num_args - 1] = NULL;

    cmd->num_args--;
}


static void
cmd_free(struct cmd *cmd)
{
    size_t i;

    for (i = 0; i < cmd->num_args; i++)
        free(cmd->args[i]);

    free(cmd->args);
    free(cmd);
}

#if 0
static void
cmd_print(const struct cmd *cmd)
{
    size_t i;

    printf("cmd {num_args:%zu, cap_args:%zu}:\n",
            cmd->num_args, cmd->cap_args);
    for (i = 0; i < cmd->num_args; i++)
        printf("\t[%zu] = \"%s\"\n", i, cmd->args[i]);
}
#endif

static struct pipeline *
pipeline_new(char *line)
{
    MU_NEW(pipeline, pipeline);
    struct cmd *cmd = NULL;
    char *s1, *s2, *command, *arg;
    char *saveptr1, *saveptr2;
    int i;

    INIT_LIST_HEAD(&pipeline->head);

    for (i = 0, s1 = line; ; i++, s1 = NULL) {
        /* break into commands */
        command = strtok_r(s1, "|", &saveptr1);
        if (command == NULL)
            break;

        cmd = cmd_new();

        /* parse the args of a single command */
        for (s2 = command; ; s2 = NULL) {
            arg = strtok_r(s2, " \t", &saveptr2);
            if (arg == NULL)    
                break;
            cmd_push_arg(cmd, arg);
        }

        list_add_tail(&cmd->list, &pipeline->head);
        pipeline->num_cmds += 1;
    }

    int count = 0;
    struct cmd *tmp;

    /* TODO: parse I/O redirects */
    if(!list_empty(&pipeline->head)) { //check to make sure list is not empty
        tmp = list_last_entry(&pipeline->head, struct cmd, list); 

        for(int i=tmp->num_args-1; i>0; i--) {
            char *arg = tmp->args[i];

            if(arg[0] == '>' && arg[1] == '>') {
                pipeline->append_file = mu_strdup(arg+2);
                count++;    
            }
            else if(arg[0] == '<') {
                pipeline->in_file = mu_strdup(arg+1);
                count++;
            }
            else if(arg[0] == '>') {
                pipeline->out_file = mu_strdup(arg+1);
                count++;
            }
            else {
                break;
            }
        }

        while(count != 0) {
            cmd_pop_arg(tmp);
            count--;
        }
    }
    return pipeline;
}


static void
pipeline_free(struct pipeline *pipeline)
{
    struct cmd *cmd, *tmp;

    list_for_each_entry_safe(cmd, tmp, &pipeline->head, list) {
        list_del(&cmd->list);
        cmd_free(cmd);
    }

    free(pipeline);
}

#if 0
static void
pipeline_print(const struct pipeline *pipeline)
{
    struct cmd *cmd;

    list_for_each_entry(cmd, &pipeline->head, list) {
        cmd_print(cmd);
    }
}
#endif


static int
pipeline_wait_all(const struct pipeline *pipeline) {
    int wstatus, exit_status;
    pid_t pid;

    struct cmd *cmd;

    list_for_each_entry(cmd, &pipeline->head, list) {
        assert(cmd->pid);
        
        pid = waitpid(cmd->pid, &wstatus, 0);
        if(pid == -1)
            mu_die_errno(errno, "waitpid");

        if(WIFEXITED(wstatus))
            exit_status = WEXITSTATUS(wstatus);
        else if(WIFSIGNALED(wstatus))
            exit_status = 128 + WTERMSIG(wstatus);
    }

    return exit_status;
}

static void
pipeline_eval(struct pipeline *pipeline) {
    int exit_status, err;
    int rfd, wfd, prev_rfd;
    pid_t pid;
    size_t cmd_idx = 0;

    int pfd[2];
    bool created_pipe = false;

    struct cmd *cmd;

    list_for_each_entry(cmd, &pipeline->head, list) {
        created_pipe = false;
        if((pipeline->num_cmds > 1) && cmd_idx != pipeline->num_cmds - 1) {
            pipe(pfd);
            if(err == -1)
                mu_die_errno(errno, "pipe");

            created_pipe = true;
        }

        pid = fork();
        if(pid == -1)
            mu_die_errno(errno, "fork error");

        if(pid == 0) {
            //child

            //adjust stdin
            if(created_pipe) {
                err = close(pfd[0]);
                if(err == -1)
                    mu_die_errno(errno, "child failed to close read end");
            }
        
            if(cmd_idx == 0) {
                if(pipeline->in_file != NULL) {
                    rfd = open(pipeline->in_file, O_RDONLY);
                    if(err == -1)
                        mu_die_errno(errno, "can't open %s", pipeline->in_file);   
                }
                else {
                    rfd = STDIN_FILENO;
                }        
            }
            else {
                rfd = prev_rfd;
            }

            if(rfd != STDIN_FILENO) {
                dup2(rfd, STDIN_FILENO);
                close(rfd);
            }

            //adjust stdout
            if(cmd_idx == (pipeline->num_cmds - 1)) {
                if(pipeline->out_file != NULL) {
                    wfd = open(pipeline->out_file, O_WRONLY|O_CREAT|O_TRUNC, 0664);
                    if(err == -1)
                        mu_die_errno(errno, "can't close %s", pipeline->out_file);   
                }
                else if(pipeline->append_file != NULL) {
                    wfd = open(pipeline->append_file, O_WRONLY|O_CREAT|O_APPEND, 0664);
                    if(err == -1)
                        mu_die_errno(errno, "can't close %s", pipeline->append_file); 
                }
                else {
                    rfd = STDOUT_FILENO;
                }   
            }
            else {
                wfd = pfd[1];
            }
            
            if(wfd != STDOUT_FILENO) {
                dup2(wfd, STDOUT_FILENO);
                close(wfd);
            }

            execvp(cmd->args[0], cmd->args);
            mu_die_errno(errno, "can't exec \"%s\"", cmd->args[0]);
        }

        //parent
        cmd->pid = pid;

        if(cmd_idx != 0) {
            err = close(prev_rfd);
            if(err == -1) 
                mu_die_errno(errno, "parent failed to close write-end");            
        }

        if(created_pipe) {
            err = close(pfd[1]);
            if(err == -1) 
                mu_die_errno(errno, "parent failed to close write-end");

            prev_rfd = pfd[0];

        }

        cmd_idx++;
    }

    exit_status = pipeline_wait_all(pipeline);
    (void)exit_status;

    return;
}

int
main(int argc, char *argv[])
{
    ssize_t len_ret = 0;
    size_t len = 0;
    char *line = NULL;
    struct pipeline *pipeline = NULL;

    /* TODO: getopt_long */
    int opt;
    /*
     * An option that takes a required argument is followed by a ':'.
     * The leading ':' suppresses getopt_long's normal error handling.
     */
    const char *short_opts = ":h";
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    
    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (opt == -1) {
            /* processed all command-line options */
            break;
        }

        switch (opt) {
        case 'h':
            usage(0);
            exit(0);
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

    MU_UNUSED(argc);
    MU_UNUSED(argv);

    /* REPL */
    while (1) {
        if (isatty(fileno(stdin)))
            printf("> ");
        len_ret = getline(&line, &len, stdin);
        if (len_ret == -1)
            goto out;
        
        mu_str_chomp(line);
        pipeline = pipeline_new(line);

        //pipeline_print(pipeline);
        pipeline_eval(pipeline);
        pipeline_free(pipeline);
    }

out:
    free(line);
    return 0;
}