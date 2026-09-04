#!/usr/bin/env python3
# Copyright 2026 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.
"""Builds the explorer cards and stages them next to the executable.

Stages both the homepage card, which is what the explorer opens when started
with no argument, and the showcase bundles the homepage's "Lynx Showcases"
entry navigates to.

The showcase staging deliberately does not reuse explorer/showcase/
build_and_copy.py. That script writes into the android, ios, harmony, windows
and macos *source* trees at fixed paths, so calling it from here would have a
Linux build modify five other platforms' checked-in resources as a side
effect. It performs the same two steps this does: build the showcase, then
copy every .lynx.bundle out of the @lynx-example packages and the menu.

Usage:
  build_resources.py <root_out_dir> <root_dir> <stamp_file>
"""

import os
import shutil
import subprocess
import sys


def copy_bundles(src_dir, dest_dir):
    """Copies every .lynx.bundle in |src_dir| into |dest_dir|.

    The destination is created only once there is something to put in it, so
    that a package carrying no bundle leaves no empty directory behind to look
    like a staged one.
    """
    if not os.path.isdir(src_dir):
        return 0
    names = sorted(name for name in os.listdir(src_dir)
                   if name.endswith('.lynx.bundle'))
    if not names:
        return 0
    os.makedirs(dest_dir, exist_ok=True)
    for filename in names:
        shutil.copy2(os.path.join(src_dir, filename),
                     os.path.join(dest_dir, filename))
    return len(names)


def stage_showcase(root_dir, root_out_dir):
    """Builds the showcase cards and stages them under resources/showcase."""
    showcase_dir = os.path.join(root_dir, 'explorer', 'showcase')
    # pnpm is not on PATH during a build; this helper points at the copy under
    # buildtools, as explorer/showcase/build_and_copy.py does.
    sys.path.append(os.path.join(root_dir, 'tools', 'js_tools'))
    from pnpm_helper import run_pnpm_command
    run_pnpm_command(['pnpm', 'run', 'build'], showcase_dir)

    dest_root = os.path.join(root_out_dir, 'lynx_explorer', 'resources',
                             'showcase')
    # Rebuilt from scratch: a package dropped from the showcase should not
    # linger in an incremental build and keep appearing in the menu.
    if os.path.isdir(dest_root):
        shutil.rmtree(dest_root)

    menu_src = os.path.join(showcase_dir, 'menu', 'dist')
    if copy_bundles(menu_src, os.path.join(dest_root, 'menu')) == 0:
        raise RuntimeError('no showcase menu bundle was produced at %s' %
                           menu_src)

    # The @lynx-example packages ship their bundles prebuilt, so these are
    # copied rather than built.
    examples_dir = os.path.join(showcase_dir, 'node_modules', '@lynx-example')
    if not os.path.isdir(examples_dir):
        raise RuntimeError(
            '%s is missing; run pnpm install at the repository root' %
            examples_dir)
    for name in sorted(os.listdir(examples_dir)):
        copy_bundles(os.path.join(examples_dir, name, 'dist'),
                     os.path.join(dest_root, name))

    check_against_manifest(dest_root)


def check_against_manifest(dest_root):
    """Fails if the staged bundles differ from showcase_bundles.txt.

    BUILD.gn declares its outputs from that manifest, so anything staged but
    not listed is invisible to ninja -- it would not notice the file going
    missing and would not restage it. Anything listed but not staged is a
    declared output that never appears, which leaves the action permanently
    out of date and rebuilds the whole showcase on every invocation. Both are
    silent, so they are turned into a build failure that says what to do.
    """
    manifest_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 'showcase_bundles.txt')
    with open(manifest_path) as manifest_file:
        expected = {line.strip() for line in manifest_file if line.strip()}

    if not os.path.isdir(dest_root):
        raise RuntimeError('the showcase staging directory %s does not exist' %
                           dest_root)

    staged = set()
    for directory, _, filenames in os.walk(dest_root):
        for filename in filenames:
            if filename.endswith('.lynx.bundle'):
                staged.add(
                    os.path.relpath(os.path.join(directory, filename),
                                    dest_root))
    if staged == expected:
        return

    # The staged set is the truth, and it has just been computed, so the
    # corrected manifest is written out rather than described. Copying that
    # file is the whole fix -- there is no generator to re-run, and so no way
    # for a regeneration command to fail halfway and leave the manifest empty.
    corrected = os.path.join(os.path.dirname(os.path.abspath(dest_root)),
                             'showcase_bundles.txt')
    with open(corrected, 'w') as corrected_file:
        for path in sorted(staged):
            corrected_file.write(path + '\n')

    lines = ['the staged showcase bundles no longer match %s' % manifest_path]
    for path in sorted(staged - expected):
        lines.append('  staged but not listed: %s' % path)
    for path in sorted(expected - staged):
        lines.append('  listed but not staged: %s' % path)
    lines.append('If that is the intended set, install it with:')
    lines.append('  cp %s \\' % corrected)
    lines.append('     %s' % manifest_path)
    raise RuntimeError('\n'.join(lines))


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 1

    root_out_dir = os.path.abspath(argv[1])
    root_dir = os.path.abspath(argv[2])
    stamp_file = os.path.abspath(argv[3])

    homepage_dir = os.path.join(root_dir, 'explorer', 'homepage')
    bundle_src = os.path.join(homepage_dir, 'dist', 'main.lynx.bundle')

    subprocess.check_call([sys.executable, 'build.py'], cwd=homepage_dir)
    if not os.path.isfile(bundle_src):
        print('homepage bundle was not produced at %s' % bundle_src,
              file=sys.stderr)
        return 1

    bundle_dir = os.path.join(root_out_dir, 'lynx_explorer', 'resources',
                              'homepage')
    os.makedirs(bundle_dir, exist_ok=True)
    shutil.copy2(bundle_src, os.path.join(bundle_dir, 'main.lynx.bundle'))

    stage_showcase(root_dir, root_out_dir)

    os.makedirs(os.path.dirname(stamp_file), exist_ok=True)
    with open(stamp_file, 'w') as stamp:
        stamp.write('')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
