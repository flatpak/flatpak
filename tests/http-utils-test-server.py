#!/usr/bin/python3

from wsgiref.handlers import format_date_time
from email.utils import parsedate
from calendar import timegm
import gzip
import sys
import time
import zlib
import os

from urllib.parse import parse_qs
import http.server as http_server
from io import BytesIO

server_start_time = int(time.time())

def parse_http_date(date):
    parsed = parsedate(date)
    if parsed is not None:
        return timegm(parsed)
    else:
        return None

class RequestHandler(http_server.BaseHTTPRequestHandler):
    def do_GET(self):
        parts = self.path.split('?', 1)
        path = parts[0]
        if len(parts) == 1:
            query = {}
        else:
            query = parse_qs(parts[1], keep_blank_values=True)

        response = 200
        add_headers = {}

        if 'modified-time' in query:
            modified_since = self.headers.get("If-Modified-Since")
            if modified_since:
                modified_since_time = parse_http_date(modified_since)
                if modified_since_time <= server_start_time:
                    response = 304
            add_headers["Last-Modified"] = format_date_time(server_start_time)

        if 'etag' in query:
            etag = str(server_start_time)

            if self.headers.get("If-None-Match") == etag:
                response = 304
            add_headers['Etag'] = etag

        self.send_response(response)
        for k, v in list(add_headers.items()):
            self.send_header(k, v)

        if 'max-age' in query:
            self.send_header('Cache-Control', 'max-age=' + query['max-age'][0])
        if 'no-cache' in query:
            self.send_header('Cache-Control', 'no-cache')
        if 'expires-past' in query:
            self.send_header('Expires', format_date_time(server_start_time - 3600))
        if 'expires-future' in query:
            self.send_header('Expires', format_date_time(server_start_time + 3600))

        if response == 200:
            self.send_header("Content-Type", "text/plain; charset=UTF-8")

        contents = "path=" + self.path + "\n"

        if not 'ignore-accept-encoding' in query:
            accept_encoding = self.headers.get("Accept-Encoding")
            if accept_encoding and accept_encoding == 'gzip':
                self.send_header("Content-Encoding", "gzip")

                buf = BytesIO()
                gzfile = gzip.GzipFile(mode='wb', fileobj=buf)
                if isinstance(contents, bytes):
                    gzfile.write(contents)
                else:
                    gzfile.write(contents.encode('utf-8'))
                gzfile.close()
                contents = buf.getvalue()

        self.end_headers()

        if response == 200:
            if isinstance(contents, bytes):
                self.wfile.write(contents)
            else:
                self.wfile.write(contents.encode('utf-8'))

def run(dir):
    RequestHandler.protocol_version = "HTTP/1.0"
    httpd = http_server.HTTPServer( ("127.0.0.1", 0), RequestHandler)
    host, port = httpd.socket.getsockname()[:2]
    with open("httpd-port", 'w') as file:
        file.write("%d" % port)
    try:
        os.write(3, bytes("Started\n", 'utf-8'));
    except:
        pass
    print("Serving HTTP on port %d" % port);
    if dir:
        os.chdir(dir)
    httpd.serve_forever()

if __name__ == '__main__':
    dir = None
    if len(sys.argv) >= 2 and len(sys.argv[1]) > 0:
        dir = sys.argv[1]

    run(dir)
