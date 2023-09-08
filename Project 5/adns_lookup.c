#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mu.h"
#include "common.h"
#include "uthash.h"

#define USAGE \
    "Usage:` adns_lookup [-h] [-p PORT] [-t] HOST QUERY\n" \
    "\n" \
    "Make a lookup request to an adns server.\n" \
    "\n" \
    "optional arguments\n" \
    "   -h, --help\n" \
    "       Show usage statement and exit.\n" \
    "\n" \
    "   -p, --port PORT\n" \
    "       The port to listen on.\n" \
    "       (default: 9514)\n" \
    "\n" \
    "   -t, --tcp\n" \
    "       Use TCP instead of UDP.\n" \
    "\n" \
    "   HOST\n" \
    "       The IPv4 address of the ADNS server (e.g., 127.0.0.1).\n" \
    "\n" \
    "   QUERY\n" \
    "       The domain name to resolve (e.g., leo).\n" \


static void
usage(int status)
{
    puts(USAGE);
    exit(status);
}


static int
client_create_tcp(const char *srv_ip, const char *srv_port)
{
    struct sockaddr_in sa;
    int sk;
    int err;

    sk = socket(AF_INET, SOCK_STREAM, 0);
    if (sk == -1)
        mu_die_errno(errno, "socket");

    mu_init_sockaddr_in(&sa, srv_ip, srv_port);
    err = connect(sk, (struct sockaddr *)&sa, sizeof(sa));
    if (err == -1)
        mu_die_errno(errno, "connect");

    return sk;
}


static int
client_create_udp(const char *srv_ip, const char *srv_port)
{
    struct sockaddr_in sa;
    int sk;

    sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == -1)
        mu_die_errno(errno, "socket");

    mu_init_sockaddr_in(&sa, srv_ip, srv_port);

    return sk;
}


static int
tcp_lookup(int sk, char* query_str) {
    char peer_str[MU_LIMITS_MAX_INET_STR_SIZE] = {0};
    uint8_t buf[MAX_MESSAGE_SIZE] = {0};
    uint8_t hdr[HEADER_SIZE] = {0};

    int err;
    size_t total;
    ssize_t n;

    struct message msg;

    /* create message */
    msg.id = 801;                               //randomly chosen id
    msg.type = QTYPE_A;
    msg.body_len = (int)strlen(query_str);
    message_set_body(&msg, query_str);
    
    /* serialize query string */
    n = message_serialize(&msg, buf, sizeof(buf));
    if(n < 0)
        mu_die("message_serialize");

    /* send message */
    err = mu_write_n(sk, buf, (size_t)n, &total);
    if(err < 0)
        mu_stderr_errno(-err, "%s: TCP send failed", peer_str);
    
    /* receive header */
    err = mu_read_n(sk, hdr, sizeof(hdr), &total);
    if(err < 0) {
        mu_stderr_errno(-err, "%s: error handling TCP request", peer_str);
        goto request_done;
    } else if(total != sizeof(hdr)) {
        mu_stderr("%s: disconnected: failed to receive complete header", peer_str);
        goto request_done;
    }

    /* parse header */
    n = message_deserialize_header(&msg, hdr, sizeof(hdr));
    if(n < 0) {
        mu_stderr("%s: malformed message header", peer_str);
        goto request_done;
    }

    if(msg.body_len == 0) {
        if(msg.type == RCODE_FORMERR) {
            printf("malformed request\n");
            message_set_error(&msg, RCODE_FORMERR);
        }
        else if(msg.type == RCODE_NXDOMAIN) {
            printf("not found\n");
            message_set_error(&msg, RCODE_NXDOMAIN);           
        }
        goto request_done;
    }

    if(msg.body_len > MAX_BODY_LEN) {
        mu_stderr("%s: body length too large (%" PRIu16 ")", peer_str, msg.body_len);
        /* TODO: err resp */
        message_set_error(&msg, RCODE_FORMERR);
        goto request_done;
    }

    /* receive body */
    err = mu_read_n(sk, msg.body, msg.body_len, &total);
    if(err < 0) {
        mu_stderr_errno(-err, "%s: error handling TCP request", peer_str);
        goto request_done;
    } else if(total != msg.body_len) {
        mu_stderr("%s: disconnected: failed to receive complete body", peer_str);
        goto request_done;
    }

    printf("%s\n", msg.body);
    close(sk);
    return 0;

request_done:
    close(sk);
    return 1;
}


static int
udp_lookup(int sk, char* query_str, const char* host_str) {
    char peer_str[MU_LIMITS_MAX_INET_STR_SIZE] = {0};
    uint8_t buf[MAX_MESSAGE_SIZE] = {0};

    int val;
    ssize_t n;
    socklen_t addr_size;

    struct sockaddr_in addr;
    struct message msg;

    /* create message */
    msg.id = 801;                               //randomly chosen id
    msg.type = QTYPE_A;
    msg.body_len = (int)strlen(query_str);
    message_set_body(&msg, query_str);

    /* create sockaddr)in */
    addr.sin_family = AF_INET; 
    mu_str_to_int(host_str, 10, &val);
    addr.sin_port = htons(val); 
    addr.sin_addr.s_addr = INADDR_ANY; 

    /* serialize query string */
    n = message_serialize(&msg, buf, sizeof(buf));
    if(n < 0)
        mu_die("message_serialize");

    /* send message */
    addr_size = sizeof(addr);
    n = sendto(sk, buf, (size_t)n, 0, (const struct sockaddr *)&addr, addr_size);
    if(n == -1)
        mu_stderr_errno(errno, "%s: sendto", peer_str);
    
    /* receive message */
    n = recvfrom(sk, buf, sizeof(buf), 0, (struct sockaddr *)&addr, &addr_size);
    if(n == -1)
        mu_die_errno(errno, "recvfrom");

    /* parse header */
    n = message_deserialize(&msg, buf, sizeof(buf));
    if(n < 0) {
        if(n == -ENOMSG) {
            mu_stderr("%s: incomplete header", peer_str);
        }
        else if(n == -E2BIG) {
            mu_stderr("%s: body length too large (%" PRIu16 ")", peer_str, msg.body_len);
            message_set_error(&msg, RCODE_FORMERR);
            goto request_done;
        }
        else {
            mu_stderr("%s: malformed request\n", peer_str);
        }
    }

    if(msg.body_len == 0) {
        if(msg.type == RCODE_FORMERR) {
            printf("malformed request\n");
            message_set_error(&msg, RCODE_FORMERR);
        }
        else if(msg.type == RCODE_NXDOMAIN) {
            printf("not found\n");
            message_set_error(&msg, RCODE_NXDOMAIN);           
        }
        goto request_done;     
    }

    printf("%s\n", msg.body);
    close(sk);
    return 0;

request_done:
    close(sk);
    return 1;
}


int 
main(int argc,char *argv[])
{
    int opt, nargs;
    const char *short_opts = ":hp:t";
    struct option long_opts[] = {
        {"help", no_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"tcp", no_argument, NULL, 't'},
        {NULL, 0, NULL, 0}
    };

    int sk, ret;
    bool is_tcp = false;
    
    const char *port_str = NULL;

    while (1) {
        opt = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
        case 'h':   /* help */
            usage(0);
            break;
        case 'p':
            port_str = mu_strdup(optarg);
            break;
        case 't':
            is_tcp = true;
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

    nargs = argc - optind;
    if (nargs != 2)
        mu_die("expected two positional arguments host and query, but found %d", nargs);

    const char *host_str = argv[optind];
    char *query_str = argv[optind+1];
    
    if(is_tcp) {
        sk = client_create_tcp(host_str, port_str != NULL ? port_str : DEFAULT_PORT_STR);
        ret = tcp_lookup(sk, query_str); 
    }
    else {
        sk = client_create_udp(host_str, port_str != NULL ? port_str : DEFAULT_PORT_STR);
        ret = udp_lookup(sk, query_str, port_str != NULL ? port_str : DEFAULT_PORT_STR);
    }

    //free(port_str);
    //free(host_str);
    //free(query_str);

    return ret;
}
/*
TCP: Create the client, serialize the data, send the data, and then deserialize the return packet?
    connect
    write/send
    read/recv
    close
Make sure UDP gets sent in one datagram
    senddto(buf)
    recvfrom()

-p 5678
127.0.0.1 
*/

/*
UDP is using datagram sockets 
TCP uses stream sockets (bidirectional connection)

socket() creates a new socket
bind() binds a socket to an address
listen() allows a stream socket to accept incoming connection from other sockets
appectp() accpets a connection from a peer appolication on a listneing stream socket
connect() establishes a connection with another socket
chpt 56 does good intro
chapter 59 1223
*/

//TODO: look to make sure that udp and tcp are formatted correctly using the .py version