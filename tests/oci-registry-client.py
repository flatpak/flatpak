#!/usr/bin/python3

import sys

import http.client
import urllib.parse


if sys.argv[2] == 'add':
    detach_icons = '--detach-icons' in sys.argv
    if detach_icons:
        sys.argv.remove('--detach-icons')
    params = {'d': sys.argv[5]}
    if detach_icons:
        params['detach-icons'] = 1
    query = urllib.parse.urlencode(params)
    conn = http.client.HTTPConnection(sys.argv[1])
    path = "/testing/{repo}/{tag}?{query}".format(repo=sys.argv[3],
                                                   tag=sys.argv[4],
                                                   query=query)
    conn.request("POST", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)
elif sys.argv[2] == 'delete':
    conn = http.client.HTTPConnection(sys.argv[1])
    path = "/testing/{repo}/{ref}".format(repo=sys.argv[3],
                                          ref=sys.argv[4])
    conn.request("DELETE", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)
else:
    print("Usage: oci-registry-client.py [add|remove] ARGS", file=sys.stderr)
    sys.exit(1)

