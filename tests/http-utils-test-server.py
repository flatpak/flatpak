#!/usr/bin/python2

from wsgiref.handlers import format_date_time
from email.utils import parsedate
from calendar import timegm
from urlparse import parse_qs
import BaseHTTPServer
import time

server_start_time = int(time.time())

def parse_http_date(date):
    parsed = parsedate(date)
    if parsed is not None:
        return timegm(parsed)
    else:
        return None

class RequestHandler(BaseHTTPServer.BaseHTTPRequestHandler):
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
        for k, v in add_headers.items():
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

        self.end_headers()

        if response == 200:
            self.wfile.write("path=" + self.path + "\n");

def test():
    BaseHTTPServer.test(RequestHandler)

if __name__ == '__main__':
    test()
