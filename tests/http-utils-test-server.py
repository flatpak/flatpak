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

def gzip_with_sync_flush(contents_bytes):
    buf = BytesIO()
    gzfile = gzip.GzipFile(mode='wb', fileobj=buf)
    half = max(1, len(contents_bytes) // 2)

    gzfile.write(contents_bytes[:half])
    gzfile.flush(zlib.Z_SYNC_FLUSH)
    split = len(buf.getvalue())
    gzfile.write(contents_bytes[half:])
    gzfile.close()

    return buf.getvalue(), split

class RequestHandler(http_server.BaseHTTPRequestHandler):
    request_counts = {}

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

        contents = "path=" + self.path + "\n"

        if 'partial-fail-non-206-on-resume' in query:
            n_requests = RequestHandler.request_counts.get(self.path, 0)
            RequestHandler.request_counts[self.path] = n_requests + 1

            contents_bytes = contents.encode('utf-8')
            range_header = self.headers.get("Range")
            if range_header and range_header.startswith("bytes="):
                self.send_response(204)
                self.end_headers()
            elif n_requests == 0:
                half = max(1, len(contents_bytes) // 2)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes[:half])
                self.wfile.flush()
                self.connection.shutdown(2)
            else:
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes)
            return

        if 'compressed-partial-fail' in query:
            n_requests = RequestHandler.request_counts.get(self.path, 0)
            RequestHandler.request_counts[self.path] = n_requests + 1
            range_seen_key = self.path + ':range-seen'
            range_header = self.headers.get("Range")

            if RequestHandler.request_counts.get(range_seen_key, 0) > 0:
                contents_bytes = b"unexpected range retry\n"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes)
                return

            if range_header and range_header.startswith("bytes="):
                RequestHandler.request_counts[range_seen_key] = 1
                contents_bytes = b"unexpected range\n"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes)
                return

            contents_bytes = (contents + ("data\n" * 4096)).encode('utf-8')
            compressed, split = gzip_with_sync_flush(contents_bytes)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=UTF-8")
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(compressed)))
            self.end_headers()

            if n_requests == 0:
                self.wfile.write(compressed[:split])
                self.wfile.flush()
                self.connection.shutdown(2)
            else:
                self.wfile.write(compressed)
            return

        if 'empty-reply-then-ok' in query:
            n_requests = RequestHandler.request_counts.get(self.path, 0)
            RequestHandler.request_counts[self.path] = n_requests + 1

            if n_requests == 0:
                self.connection.close()
                return

        if 'error-then-ok' in query:
            n_requests = RequestHandler.request_counts.get(self.path, 0)
            RequestHandler.request_counts[self.path] = n_requests + 1

            if n_requests == 0:
                contents_bytes = b"server-error\n"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes)
                return

        if 'partial-fail-no-body-on-resume' in query:
            n_requests = RequestHandler.request_counts.get(self.path, 0)
            RequestHandler.request_counts[self.path] = n_requests + 1

            contents_bytes = contents.encode('utf-8')
            range_header = self.headers.get("Range")
            if range_header and range_header.startswith("bytes="):
                byte_range = range_header[len("bytes="):]
                start = int(byte_range.split('-')[0])
                remainder = contents_bytes[start:]
                self.send_response(206)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Range",
                                 "bytes %d-%d/%d" % (start, len(contents_bytes) - 1, len(contents_bytes)))
                self.send_header("Content-Length", str(len(remainder)))
                self.end_headers()

                if n_requests == 1:
                    self.wfile.flush()
                    self.connection.shutdown(2)
                else:
                    self.wfile.write(remainder)
            elif n_requests == 0:
                half = max(1, len(contents_bytes) // 2)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes[:half])
                self.wfile.flush()
                self.connection.shutdown(2)
            else:
                contents_bytes = b"unexpected restart\n"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes)
            return

        if 'partial-fail' in query:
            # On the first request, send half the content then close the
            # connection to simulate a transient network failure mid-download.
            # On a retry with a Range header (resume), serve the remainder.
            contents_bytes = contents.encode('utf-8')
            range_header = self.headers.get("Range")
            if range_header and range_header.startswith("bytes="):
                byte_range = range_header[len("bytes="):]
                start = int(byte_range.split('-')[0])
                remainder = contents_bytes[start:]
                self.send_response(206)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Range",
                                 "bytes %d-%d/%d" % (start, len(contents_bytes) - 1, len(contents_bytes)))
                self.send_header("Content-Length", str(len(remainder)))
                self.end_headers()
                self.wfile.write(remainder)
            else:
                half = max(1, len(contents_bytes) // 2)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(contents_bytes)))
                self.end_headers()
                self.wfile.write(contents_bytes[:half])
                self.wfile.flush()
                self.connection.shutdown(2)
            return

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
