#!/usr/bin/python3

import argparse
import functools
import http.server as http_server
import os
import ssl
import urllib.parse


class RequestHandler(http_server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            # Expected when the client rejects our private CA or closes an
            # idle connection after a test sequence.
            pass

    def do_GET(self):
        has_client_cert = self.connection.getpeercert() is not None
        url = urllib.parse.urlsplit(self.path)

        if self.headers.get("User-Agent") != "flatpak-http-session-test":
            response = 400
        elif url.path == "/require-client-cert":
            response = 200 if has_client_cert else 403
        elif url.path == "/forbid-client-cert":
            response = 403 if has_client_cert else 200
        else:
            response = 404

        body = str(self.client_address[1]).encode("ascii")
        self.send_response(response)
        self.send_header("Content-Length", str(len(body)))
        if url.query == "close":
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)


def run(cert, key, client_ca, directory):
    # A TLS-profile change can open a new connection while the previous
    # keep-alive connection is still waiting for its next request.
    handler = RequestHandler
    if directory is not None:
        handler = functools.partial(http_server.SimpleHTTPRequestHandler, directory=directory)
    httpd = http_server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    context.load_verify_locations(client_ca)
    context.verify_mode = ssl.CERT_OPTIONAL
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    port = httpd.socket.getsockname()[1]
    with open("httpd-port", "w") as file:
        file.write(str(port))
    try:
        os.write(3, b"Started\n")
    except OSError:
        pass
    httpd.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--client-ca", required=True)
    parser.add_argument("--directory")
    args = parser.parse_args()
    run(args.cert, args.key, args.client_ca, args.directory)
