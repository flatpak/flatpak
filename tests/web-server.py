#!/usr/bin/python3

import argparse
from wsgiref.handlers import format_date_time
from email.utils import parsedate
from calendar import timegm
import gzip
import ssl
import sys
import time
import zlib
import os
from http import HTTPStatus
from urllib.parse import parse_qs
import http.server as http_server
from io import BytesIO
import sys

class RequestHandler(http_server.SimpleHTTPRequestHandler):
    headers_log_file = None

    def handle_tokens(self):
        need_token_path = self.translate_path(self.path) + ".need_token"
        if os.path.isfile(need_token_path):
            with open(need_token_path, 'r') as content_file:
                token_content = content_file.read()
            token = None
            auth = self.headers.get("Authorization")
            if auth and auth.startswith("Bearer "):
                token = auth[7:]
            if token == None:
                self.send_response(HTTPStatus.UNAUTHORIZED, "No token")
                self.end_headers()
                return True
            if token != token_content:
                self.send_response(HTTPStatus.UNAUTHORIZED, "Wrong token")
                self.end_headers()
                return True
        return False

    def log_flatpak_headers(self):
        if self.headers_log_file is None:
            return

        for name in ("Flatpak-Ref", "Flatpak-Upgrade-From", "Flatpak-Is-Update"):
            value = self.headers.get(name)
            if value is not None:
                with open(self.headers_log_file, 'a+') as f:
                    f.write("%s: %s\n" % (name, value))

    def do_GET(self):
        self.log_flatpak_headers()
        if self.handle_tokens():
            return None
        self.headers.__delitem__("If-Modified-Since")
        return super().do_GET()

def run(args):
    RequestHandler.protocol_version = "HTTP/1.0"
    RequestHandler.headers_log_file = args.headers_log
    httpd = http_server.HTTPServer( ("127.0.0.1", 0), RequestHandler)

    if args.cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.options |= ssl.OP_NO_TLSv1 | ssl.OP_NO_TLSv1_1
        context.load_cert_chain(certfile=args.cert, keyfile=args.key)

        if args.mtls_cacert:
            context.load_verify_locations(cafile=args.mtls_cacert)
            # In a real application, we'd need to check the CN against authorized users
            context.verify_mode = ssl.CERT_REQUIRED

        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    host, port = httpd.socket.getsockname()[:2]
    with open("httpd-port", 'w') as file:
        file.write("%d" % port)
    with open("httpd-pid", 'w') as file:
        file.write("%d" % os.getpid())
    try:
        os.write(3, bytes("Started\n", 'utf-8'));
    except:
        pass
    if args.cert:
        print("Serving HTTPS on port %d" % port)
    else:
        print("Serving HTTP on port %d" % port)
    if args.dir:
        os.chdir(args.dir)
    httpd.serve_forever()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir")
    parser.add_argument("--headers-log")
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--mtls-cacert")
    args = parser.parse_args()

    run(args)
