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


def run_add_sig(args):
    params = {"s": args.signature}
    query = urllib.parse.urlencode(params)
    conn = get_conn(args)
    path = "/testing-sig/{repo}/{digest}?{query}".format(
        repo=args.repo, digest=args.digest, query=query
    )
    conn.request("POST", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)


def run_delete_sig(args):
    conn = get_conn(args)
    path = "/testing-sig/{repo}/{digest}".format(
        repo=args.repo, digest=args.digest
    )
    conn.request("DELETE", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)


def run_set_auth(args):
    params = {"token": args.token}
    if args.expire_on_path is not None:
        params["expire-on-path"] = args.expire_on_path
    if args.next_token is not None:
        params["next-token"] = args.next_token
    if args.token_update_file is not None:
        params["token-update-file"] = args.token_update_file
    query = urllib.parse.urlencode(params)
    conn = get_conn(args)
    path = "/testing-auth?{query}".format(query=query)
    conn.request("POST", path)
    response = conn.getresponse()
    if response.status != 200:
        print(response.read(), file=sys.stderr)
        print("Failed: status={}".format(response.status), file=sys.stderr)
        sys.exit(1)


def run_reset_auth(args):
    conn = get_conn(args)
    conn.request("DELETE", "/testing-auth")
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

add_sig_parser = subparsers.add_parser("add-signature")
add_sig_parser.add_argument("repo")
add_sig_parser.add_argument("digest")
add_sig_parser.add_argument("signature")
add_sig_parser.set_defaults(func=run_add_sig)

delete_sig_parser = subparsers.add_parser("delete-signature")
delete_sig_parser.add_argument("repo")
delete_sig_parser.add_argument("digest")
delete_sig_parser.set_defaults(func=run_delete_sig)

set_auth_parser = subparsers.add_parser("set-auth")
set_auth_parser.add_argument("--token", required=True)
set_auth_parser.add_argument("--expire-on-path", default=None)
set_auth_parser.add_argument("--next-token", default=None)
set_auth_parser.add_argument("--token-update-file", default=None)
set_auth_parser.set_defaults(func=run_set_auth)

reset_auth_parser = subparsers.add_parser("reset-auth")
reset_auth_parser.set_defaults(func=run_reset_auth)

args = parser.parse_args()
args.func(args)
