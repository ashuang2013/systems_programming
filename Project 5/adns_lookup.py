#!/usr/bin/env python3

import argparse
import logging
import random
import socket
import struct
import sys


DEFAULT_PORT = 9514

HEADER_SIZE = 8
MAX_DOMAIN_LEN = 253
MAX_BODY_LEN = MAX_DOMAIN_LEN
MAX_MESSAGE_SIZE = HEADER_SIZE + MAX_BODY_LEN

QTYPE_A = 1
QTYPE_PTR = 12
QTYPE_TXT = 16

RCODE_NOERROR = 0
RCODE_FORMERR = 1
RCODE_NXDOMAIN = 3


def recv_n(sk, n):
    """Read n bytes.  If EOF, returns less than n bytes."""
    data = sk.recv(n)
    total = len(data)
    if total == 0:
        return data

    need = n - total
    while need:
        part = sk.recv(need)
        if part == 0:
            return data
        data = data + part
        total = len(data)
        need = n - total

    return data


def print_response(_id, _type, body_len, ip):
    logging.debug("response {id:0x%08x, type:%d, body_len:%d, body:\"%s\"}",
        _id, _type, body_len, ip)

    if _type == RCODE_NOERROR:
        print(ip)
    elif _type == RCODE_FORMERR:
        print("malformed request\n")
    elif _type == RCODE_NXDOMAIN:
        print("not found\n")
    else:
        print("unknown response type: %d", _type)


def make_request(qtype, query):
    assert len(query) <= MAX_DOMAIN_LEN 
    _id = random.randrange(0, (2**32)-1)
    query_bytes = query.encode('utf-8')
    header = struct.pack('>IHH', _id, qtype, len(query_bytes))

    logging.debug("request {id:0x%08x, type:%d, body_len:%d, body:\"%s\"}",
        _id, qtype, len(query_bytes), query)

    return header + query_bytes


def tcp_lookup(server, qtype, query):
    try:
        sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sk.connect(server)
        sk.sendall(make_request(qtype, query))

        hdr = recv_n(sk, HEADER_SIZE)
        if len(hdr) != HEADER_SIZE:
            logging.warning("disconnected: failed to receive complete header")
            return 1

        _id, _type, body_len = struct.unpack('>IHH', hdr)
        if body_len > MAX_BODY_LEN:
            logging.warning("body length too large (%d)", body_len)
            return 1

        body = ''
        if body_len:
            body = recv_n(sk, body_len)
            if len(body) != body_len:
                logging.warning("disconnected: failed to receive complete body")
                return 1
            body = body.decode('utf-8')

        print_response(_id, _type, body_len, body)
        return _type

    except OSError as e:
        logging.error("error making TCP request: %s", str(e))
        return 1


def udp_lookup(server, qtype, query):
    try:
        sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sk.sendto(make_request(qtype, query), server)
        data, src = sk.recvfrom(MAX_MESSAGE_SIZE)

        msg_size = len(data)
        if msg_size < HEADER_SIZE:
            logging.warning("response has incomplete header")
            return 1

        _id, _type, body_len = struct.unpack('>IHH', data[:HEADER_SIZE])

        if body_len > MAX_BODY_LEN:
            logging.warning("response has body length that is too large (%d)", body_len)
            return 1

        if (HEADER_SIZE + body_len) > msg_size:
            logging.warning("response has incomplete (truncated) body") 
            return 1

        ip = data[HEADER_SIZE:HEADER_SIZE+body_len].decode('utf-8')
        print_response(_id, _type, body_len, ip)
        return _type
    except OSError as e:
        logging.error("error making UDP request: %s", str(e))
        return 1


def main():
    random.seed()

    parser = argparse.ArgumentParser(description="Make a lookup request to an adns server")
    parser.add_argument("host",
            help="The IP address of the adns server")
    parser.add_argument("query",
            help="The value to loookup")
    parser.add_argument("-p", "--port", 
            help="The adns server's port.  (default: %d)" % DEFAULT_PORT,
            type=int, default=DEFAULT_PORT)
    parser.add_argument("-t", "--tcp", 
            help="Use TCP instead of UDP.",
            action='store_true')
    parser.add_argument("-v", "--verbose", 
            help="Use verbose logging.",
            action='store_true')
    parser.add_argument("-r", "--reverse", 
            help="""By default, QUERY is expected to be a domain name.  If -x is
            passed, QUERY is an IP address, and the query returns the
            associated domain name""",
            action='store_true')
    parser.add_argument("-x", "--txt", 
            help="""Make a TXT query, where the query is an arbitary key to
            lookup.  The only value that the server supports is "version",
            which returns the ADNS version that the server is running.""",
            action='store_true')


    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.WARNING
    logging.basicConfig(level=level, format="%(levelname)s - %(message)s")

    if args.reverse and args.txt:
        die('cannot specify both --reverse and --text')

    if args.reverse:
        qtype = QTYPE_PTR
    elif args.txt:
        qtype = QTYPE_TXT
    else:
        qtype = QTYPE_A

    server = (args.host, args.port)
    if args.tcp:
        ret = tcp_lookup(server, qtype, args.query)
    else:
        ret = udp_lookup(server, qtype, args.query)

    sys.exit(ret)


if __name__ == '__main__':
    main()
