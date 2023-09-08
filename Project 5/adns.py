#!/usr/bin/env python3

import argparse
import socketserver
import logging
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

VERSION = 'adns v1.0'

def die(msg):
    logging.error(msg)
    sys.exit(1)


def read_zone_file(path):
    d = {}
    try:
        with open(path) as f:
            for lineno, line in enumerate(f.readlines()):
                toks = line.strip().split()
                if len(toks) != 2:
                    die("error reading zone file '%s' at line #%d" % (path, lineno + 1))
                d[toks[0]] = toks[1]
    except OSError as e:
        die("error reading zone file: %s" % (str(e), ))

    return d


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


def make_response(_id, rcode, body_str=''):
    body_bytes = body_str.encode('utf-8')
    assert len(body_bytes) <= MAX_BODY_LEN
    resp = struct.pack('>IHH', _id, rcode, len(body_bytes))
    resp += body_bytes
    return resp


def process_message(zone, qtype, query):
    assert len(query) > 0
    if qtype == QTYPE_A:
        ip = zone.get(query)
        if ip:
            return (RCODE_NOERROR, ip)
        else:
            return (RCODE_NXDOMAIN, '')
    elif qtype == QTYPE_PTR:
        for domain, ip in zone.items():
            if ip == query:
                return (RCODE_NOERROR, domain)
        return (RCODE_NXDOMAIN, '')
    elif qtype == QTYPE_TXT:
        if query == 'version':
            return (RCODE_NOERROR, VERSION)
        else:
            return (RCODE_NXDOMAIN, '')
    else:
        return (RCODE_FORMERR, '')


class ADNSTCPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        sk = self.request

        logging.debug("%s: connected", self.client_address)
        try:
            hdr = recv_n(sk, HEADER_SIZE)
            if len(hdr) != HEADER_SIZE:
                logging.warning("%s: disconnected: failed to receive complete header", self.client_address)
                return

            _id, qtype, body_len = struct.unpack('>IHH', hdr)
            if body_len == 0:
                logging.warning("%s: zero-length body", self.client_address)
                sk.sendall(make_response(_id, RCODE_FORMERR))
                return

            if body_len > MAX_BODY_LEN:
                logging.warning("%s: body length too large (%d)", self.client_address, body_len)
                sk.sendall(make_response(_id, RCODE_FORMERR))
                return

            body = recv_n(sk, body_len)
            if len(body) != body_len:
                logging.warning("%s: disconnected: failed to receive complete body", self.client_address)
                return

            query = body.decode('utf-8')
            logging.debug("%s: request: id=0x%08x, type=%d, body_len=%d, body=\"%s\"", \
                    self.client_address, _id, qtype, body_len, query)

            rcode, reply = process_message(self.server.zone, qtype, query)
            logging.debug("%s: response: id=0x%08x, type=%d, body_len=%d, body=\"%s\"", \
                    self.client_address, _id, rcode, len(reply), reply)
            sk.sendall(make_response(_id, rcode, reply))
        except OSError as e:
            logging.warning("%s: error handling TCP request: %s", self.client_address, str(e))


class ADNSTCPServer(socketserver.TCPServer):
    allow_reuse_address = True
    def __init__(self, server_address, zone):
        socketserver.TCPServer.__init__(self, server_address, ADNSTCPHandler)
        self.zone = zone


class ADNSUDPHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sk = self.request
        try:
            msg_size = len(data)
            if msg_size < HEADER_SIZE:
                logging.warning("%s: incomplete header", self.client_address)
                return

            _id, qtype, body_len = struct.unpack('>IHH', data[:HEADER_SIZE])
            if body_len == 0:
                logging.warning("%s: zero-length body", self.client_address)
                sk.sendto(make_response(_id, RCODE_FORMERR), self.client_address)
                return

            if body_len > MAX_BODY_LEN:
                logging.warning("%s: body length too large (%d)", self.client_address, body_len)
                sk.sendto(make_response(_id, RCODE_FORMERR), self.client_address)
                return

            if (HEADER_SIZE + body_len) > msg_size:
                logging.warning("%s: incomplete (truncated) body", self.client_address)
                sk.sendto(make_response(_id, RCODE_FORMERR), self.client_address)
                return
            
            query = data[HEADER_SIZE:HEADER_SIZE+body_len].decode('utf-8')
            logging.debug("%s: request: id=0x%08x, type=%d, body_len=%d, body=\"%s\"", \
                    self.client_address, _id, qtype, body_len, query)

            rcode, reply = process_message(self.server.zone, qtype, query)
            logging.debug("%s: response: id=0x%08x, type=%d, body_len=%d, body=\"%s\"", \
                    self.client_address, _id, rcode, len(reply), reply)
            sk.sendto(make_response(_id, rcode, reply), self.client_address)
        except OSError as e:
            logging.warning("%s: error handling UDP request: %s", self.client_address, str(e))


class ADNSUDPServer(socketserver.UDPServer):
    def __init__(self, server_address, zone):
        socketserver.UDPServer.__init__(self, server_address, ADNSUDPHandler)
        self.zone = zone


def main():
    logging.basicConfig(level=logging.DEBUG, format="%(levelname)s - %(message)s")

    parser = argparse.ArgumentParser(description="A simplified version of a DNS server for IPv4")
    parser.add_argument("zone_file",
            help="Zone configuration file")
    parser.add_argument("-i", "--interface", 
            help="The interface to listen on.  (default: INADDR_ANY)",
            default='')
    parser.add_argument("-p", "--port", 
            help="The port to listen on.  (default: %d)" % DEFAULT_PORT,
            type=int, default=DEFAULT_PORT)
    parser.add_argument("-t", "--tcp", 
            help="Use TCP instead of UDP.",
            action='store_true')

    args = parser.parse_args()

    zone = read_zone_file(args.zone_file)
    klass = ADNSTCPServer if args.tcp else ADNSUDPServer

    with klass((args.interface, args.port), zone) as server:
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            server.server_close()
            sys.exit(0)


if __name__ == '__main__':
    main()
