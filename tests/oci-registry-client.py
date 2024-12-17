#!/usr/bin/python3

import argparse
import ssl
import sys

import http.client
import urllib.parse


def get_conn(args):
    parsed = urllib.parse.urlparse(args.url)
    if parsed.scheme == "http":
        return http.client.HTTPConnection(host=parsed.hostname, port=parsed.port)
    elif parsed.scheme == "https":
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.options |= ssl.OP_NO_TLSv1 | ssl.OP_NO_TLSv1_1
        if args.cert:
            context.load_cert_chain(certfile=args.cert, keyfile=args.key)
        if args.cacert:
            context.load_verify_locations(cafile=args.cacert)
        return http.client.HTTPSConnection(
            host=parsed.hostname, port=parsed.port, context=context
        )
    else:
        assert False, "Bad scheme: " + parsed.scheme


def run_add(args):
    params = {"d": args.oci_dir}
    if args.detach_icons:
        params["detach-icons"] = "1"
    query = urllib.parse.urlencode(params)
    conn = get_conn(args)
    path = "/testing/{repo}/{tag}?{query}".format(
        repo=args.repo, tag=args.tag, query=query
    )
    conn.request("POST", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)


def run_delete(args):
    conn = get_conn(args)
    path = "/testing/{repo}/{ref}".format(repo=args.repo, ref=args.ref)
    conn.request("DELETE", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)


parser = argparse.ArgumentParser()
parser.add_argument("--url", required=True)
parser.add_argument("--cacert")
parser.add_argument("--cert")
parser.add_argument("--key")

subparsers = parser.add_subparsers()
subparsers.required = True

add_parser = subparsers.add_parser("add")
add_parser.add_argument("repo")
add_parser.add_argument("tag")
add_parser.add_argument("oci_dir")
add_parser.add_argument("--detach-icons", action="store_true", default=False)
add_parser.set_defaults(func=run_add)

delete_parser = subparsers.add_parser("delete")
delete_parser.add_argument("repo")
delete_parser.add_argument("ref")
delete_parser.set_defaults(func=run_delete)

args = parser.parse_args()
args.func(args)
