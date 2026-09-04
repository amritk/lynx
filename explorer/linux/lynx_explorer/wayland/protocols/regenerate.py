#!/usr/bin/env python3
# Copyright 2026 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.
"""Regenerates the checked-in Wayland client protocol sources.

The generated sources are committed rather than produced at build time, so the
build needs neither wayland-scanner nor the wayland-protocols definitions. Run
this only when a protocol is added or its definition is updated.

  regenerate.py --scanner /path/to/wayland-scanner-1.19 [--protocol-dir DIR]

wayland-scanner 1.20 and newer emit calls to wl_proxy_marshal_flags(), which
libwayland only exports from 1.20 onwards. The Debian sysroot this repository
builds against ships libwayland 1.17, so the scanner MUST be older than 1.20;
this script refuses to run otherwise. Older scanners emit wl_proxy_marshal()
and wl_proxy_marshal_constructor(), which 1.17 does export, and the result
still runs against every newer libwayland.

A suitable scanner can be built from the wayland checkout this repository
already syncs for Linux, at a revision before "scanner: Use the new atomic
marshal/destroy function":

  cd third_party/angle/third_party/wayland
  git archive $(git log --format=%H -S wl_proxy_marshal_flags -- src/scanner.c \
      | tail -1)^ | tar -x -C /tmp/wl
  cd /tmp/wl && sed -e 's/@WAYLAND_VERSION_MAJOR@/1/' \
      -e 's/@WAYLAND_VERSION_MINOR@/19/' -e 's/@WAYLAND_VERSION_MICRO@/91/' \
      -e 's/@WAYLAND_VERSION@/1.19.91/' src/wayland-version.h.in > wayland-version.h
  cc -o wayland-scanner src/scanner.c src/wayland-util.c -Isrc -I. \
      $(pkg-config --cflags --libs expat)
"""

import argparse
import os
import re
import subprocess
import sys

# Protocols to generate, as (stability, name) pairs.
PROTOCOLS = [
    ("stable", "xdg-shell"),
    ("unstable", "xdg-decoration"),
]

MAX_SCANNER_VERSION = (1, 20)


def scanner_version(scanner):
    # --version prints to stderr on some builds.
    try:
        result = subprocess.run([scanner, "--version"], capture_output=True,
                                text=True)
    except OSError as error:
        raise SystemExit("could not run %s: %s" % (scanner, error))
    match = re.search(r"(\d+)\.(\d+)", result.stdout + result.stderr)
    if not match:
        raise SystemExit("could not determine the version of %s" % scanner)
    return (int(match.group(1)), int(match.group(2)))


def protocols_data_dir():
    try:
        return subprocess.check_output(
            ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"],
            text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit(
            "could not locate the wayland-protocols definitions: install "
            "wayland-protocols and pkg-config, or pass --protocol-dir")


def find_protocol_xml(data_dir, stability, name):
    # Unstable protocols are versioned on disk, e.g.
    # unstable/xdg-decoration/xdg-decoration-unstable-v1.xml.
    directory = os.path.join(data_dir, stability, name)
    candidates = [os.path.join(directory, "%s.xml" % name)]
    if os.path.isdir(directory):
        candidates += [
            os.path.join(directory, entry)
            for entry in sorted(os.listdir(directory))
            if entry.startswith(name) and entry.endswith(".xml")
        ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    raise SystemExit("no protocol XML for %s/%s under %s" %
                     (stability, name, data_dir))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scanner", required=True,
                        help="wayland-scanner older than 1.20")
    parser.add_argument("--protocol-dir", default=None,
                        help="wayland-protocols data dir (default: pkg-config)")
    parser.add_argument("--out-dir", default=os.path.dirname(
        os.path.abspath(__file__)))
    args = parser.parse_args()

    version = scanner_version(args.scanner)
    if version >= MAX_SCANNER_VERSION:
        raise SystemExit(
            "wayland-scanner %d.%d emits wl_proxy_marshal_flags(), which the "
            "sysroot's libwayland 1.17 does not export. Use a scanner older "
            "than 1.20; see the module docstring." % version)

    data_dir = args.protocol_dir or protocols_data_dir()
    for stability, name in PROTOCOLS:
        xml = find_protocol_xml(data_dir, stability, name)
        header = os.path.join(args.out_dir, "%s-client-protocol.h" % name)
        code = os.path.join(args.out_dir, "%s-protocol.c" % name)
        subprocess.check_call([args.scanner, "client-header", xml, header])
        subprocess.check_call([args.scanner, "private-code", xml, code])
        print("generated %s from %s" % (name, xml))


if __name__ == "__main__":
    main()
