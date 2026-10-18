#!/usr/bin/env python3
# Copyright 2026 Notolibra. All rights reserved.
"""Quilt-style patch series for the Notolibra engine.

The series lives in patches/series, one patch per line, in apply order.
Blank lines and lines starting with '#' are ignored.

Conventions (plan.md §4.3-§4.4):
  * One upstream file per patch, and the patch is named after that file with
    '/' replaced by '-' (Brave's convention), so a conflict points at one file.
    Patches for one concern sit in one directory: patches/<concern>/<name>.patch
  * Every patch starts with a free-text header (before the first 'diff --git')
    giving the reason and, if one exists, an upstream bug link. `git apply`
    ignores the header; `refresh` keeps it.
  * A header line 'Repo: <path>' applies the patch inside a nested git
    repository of the checkout (e.g. third_party/ffmpeg). Default: src itself.

Commands:
  check     ungoogled-chromium-style validation: every listed patch exists,
            every patch file is listed, one file per patch, the file it changes
            exists, and the series applies cleanly (or is already applied).
  status    applied / unapplied / conflict for each patch.
  apply     apply the series in order (skips patches already applied).
  unapply   reverse the series in reverse order (skips unapplied patches).
  refresh   regenerate patches from the working tree (all, or those named).
  new       new <concern> <upstream-file> [reason]: create a patch from the
            current diff of that file and append it to the series.

This tool is our own code; ungoogled-chromium's utils/ (BSD-3) were read as
reference for which checks to run.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

ENGINE = pathlib.Path(__file__).resolve().parent.parent
PATCHES = ENGINE / 'patches'
SERIES = PATCHES / 'series'
SRC = pathlib.Path(os.environ.get('NOTOLIBRA_CHROMIUM_SRC', ENGINE.parent))

DIFF_RE = re.compile(r'^diff --git a/(\S+) b/(\S+)$', re.M)
REPO_RE = re.compile(r'^Repo:\s*(\S+)\s*$', re.M)


class Patch:
    def __init__(self, rel):
        self.rel = rel
        self.path = PATCHES / rel
        self.text = self.path.read_text() if self.path.exists() else ''
        m = REPO_RE.search(self.header)
        self.repo_rel = m.group(1).strip('/') if m else ''
        self.repo = SRC / self.repo_rel if m else SRC
        self.files = [b for _, b in DIFF_RE.findall(self.text)]

    @property
    def header(self):
        i = self.text.find('diff --git ')
        return self.text if i < 0 else self.text[:i]

    def git(self, *args, check=False):
        return subprocess.run(['git', '-C', str(self.repo), *args],
                              capture_output=True, text=True, check=check)

    def applies(self):
        return self.git('apply', '--check', str(self.path)).returncode == 0

    def applied(self):
        return self.git('apply', '--check', '--reverse',
                        str(self.path)).returncode == 0

    def state(self):
        if self.applied():
            return 'applied'
        if self.applies():
            return 'unapplied'
        return 'conflict'


def expected_name(upstream_file, repo_rel=''):
    path = f'{repo_rel}/{upstream_file}' if repo_rel else upstream_file
    return path.replace('/', '-') + '.patch'


def read_series():
    if not SERIES.exists():
        return []
    out = []
    for line in SERIES.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            out.append(line)
    return out


def cmd_check(args):
    errors = []
    series = read_series()
    listed = set(series)
    on_disk = {str(p.relative_to(PATCHES)) for p in PATCHES.rglob('*.patch')}
    for rel in series:
        if rel not in on_disk:
            errors.append(f'{rel}: listed in series but missing')
    for rel in sorted(on_disk - listed):
        errors.append(f'{rel}: patch file not listed in series')
    if len(series) != len(listed):
        errors.append('series lists a patch more than once')
    for rel in series:
        p = Patch(rel)
        if not p.text:
            continue
        if len(p.files) != 1:
            errors.append(f'{rel}: changes {len(p.files)} files; one per patch')
            continue
        f = p.files[0]
        want = expected_name(f, p.repo_rel)
        if pathlib.Path(rel).name != want:
            errors.append(f'{rel}: should be named {want}')
        creates = re.search(r'^--- /dev/null$', p.text, re.M)
        if not creates and not (p.repo / f).exists():
            errors.append(f'{rel}: upstream file {f} does not exist')
        st = p.state()
        if st == 'conflict':
            errors.append(f'{rel}: does not apply cleanly (and is not applied)')
    for e in errors:
        print('FAIL', e)
    print(f'{len(series)} patches, {len(errors)} problems')
    return 1 if errors else 0


def cmd_status(args):
    for rel in read_series():
        print(f'{Patch(rel).state():10} {rel}')
    return 0


def cmd_apply(args):
    failed = []
    for rel in read_series():
        p = Patch(rel)
        if p.applied():
            continue
        r = p.git('apply', '--whitespace=nowarn', str(p.path))
        if r.returncode:
            failed.append(rel)
            print(f'FAIL {rel}\n{r.stderr.strip()}')
            if not args.keep_going:
                break
        else:
            print(f'applied {rel}')
    if failed:
        print(f'{len(failed)} patch(es) failed; fix them, then `refresh`.')
    return 1 if failed else 0


def cmd_unapply(args):
    failed = []
    for rel in reversed(read_series()):
        p = Patch(rel)
        if not p.applied():
            continue
        r = p.git('apply', '--reverse', str(p.path))
        if r.returncode:
            failed.append(rel)
            print(f'FAIL {rel}\n{r.stderr.strip()}')
        else:
            print(f'reversed {rel}')
    return 1 if failed else 0


def diff_for(repo, upstream_file):
    r = subprocess.run(['git', '-C', str(repo), 'diff', '--no-color',
                        '--no-ext-diff', 'HEAD', '--', upstream_file],
                       capture_output=True, text=True, check=True)
    if r.stdout:
        return r.stdout
    # A new file isn't in HEAD; diff it against /dev/null.
    if (repo / upstream_file).exists():
        r = subprocess.run(['git', 'diff', '--no-index', '--no-color',
                            '/dev/null', upstream_file],
                           capture_output=True, text=True, cwd=repo)
        return r.stdout
    return ''


def cmd_refresh(args):
    targets = args.patches or read_series()
    for rel in targets:
        p = Patch(rel)
        if len(p.files) != 1:
            print(f'skip {rel}: changes {len(p.files)} files')
            continue
        d = diff_for(p.repo, p.files[0])
        if not d:
            print(f'skip {rel}: {p.files[0]} has no changes in the tree')
            continue
        new = p.header + d
        if new != p.text:
            p.path.write_text(new)
            print(f'refreshed {rel}')
    return 0


def cmd_new(args):
    repo = SRC / args.repo if args.repo else SRC
    d = diff_for(repo, args.file)
    if not d:
        print(f'{args.file} has no changes')
        return 1
    rel = f'{args.concern}/{expected_name(args.file, args.repo or "")}'
    path = PATCHES / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    header = (args.reason or 'Reason: TODO') + '\n'
    if args.repo:
        header += f'Repo: {args.repo}\n'
    path.write_text(header + '\n' + d)
    with SERIES.open('a') as f:
        f.write(rel + '\n')
    print(f'created {rel}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('check')
    sub.add_parser('status')
    a = sub.add_parser('apply')
    a.add_argument('--keep-going', action='store_true')
    sub.add_parser('unapply')
    r = sub.add_parser('refresh')
    r.add_argument('patches', nargs='*')
    n = sub.add_parser('new')
    n.add_argument('concern')
    n.add_argument('file')
    n.add_argument('reason', nargs='?')
    n.add_argument('--repo')
    args = ap.parse_args()
    if not (SRC / '.gn').exists():
        sys.exit(f'{SRC} is not a Chromium src checkout '
                 '(set NOTOLIBRA_CHROMIUM_SRC)')
    return globals()['cmd_' + args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
