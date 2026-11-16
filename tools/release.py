#!/usr/bin/env python3
"""Release a version of ShotMvp: tag, build the binaries from that tag, attach them.

Card #23 is the reason this file exists: v0.2.0 was tagged with no .exe attached to it, so the
release existed and nothing could be downloaded from it. Building the artifacts and putting them on
the release is a step of this script now, not a manual "remember to attach".

The build toolchain (g++, windres) lives on the Windows side of this machine, the git repository on
the Linux side. The script therefore exports the tag into the Windows mirror, builds it there with
the project's own build.ps1 -Selftest, and brings the two executables back named with the version.
The selftest is a gate: if it does not pass, the script stops before anything is published.

Run:  python3 tools/release.py v0.2.1 [--milestone "<milestone title>"] [--no-milestone]
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, tarfile, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = 'viz6411/Easy-screenshot-tool'
MIRROR = '/mnt/c/Users/viz/Documents/Strata/Workspace'
PWSH = '/mnt/c/Program Files/PowerShell/7/pwsh.exe'


def sh(cmd, cwd=None, check=True):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit('command failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return (r.stdout or '') + (r.stderr or '')


def git(*a, check=True):
    return sh(['git'] + list(a), cwd=ROOT, check=check)


def gh(*a, check=True):
    return sh(['gh'] + list(a), cwd=ROOT, check=check)


def gh_ok(*a):
    """A question, not a command: does this release exist yet? Checking the exit code is the point -
    `gh` writes its 'not found' complaint to stderr, so looking at the output would answer yes."""
    return subprocess.run(['gh'] + list(a), cwd=ROOT, capture_output=True, text=True).returncode == 0


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest().upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version', help='tag to release, e.g. v0.2.1')
    ap.add_argument('--milestone', help='milestone title to close with this release')
    ap.add_argument('--no-milestone', action='store_true')
    ap.add_argument('--mirror', default=MIRROR, help='Windows mirror root that can be built in')
    ap.add_argument('--pwsh', default=PWSH, help='path to PowerShell 7 on the build side')
    args = ap.parse_args()
    ver = args.version
    tag_exists = git('tag', '-l', ver).strip() != ''

    # A release is a commit, so the tree being released has to be clean and already pushed.
    if git('status', '--porcelain').strip():
        sys.exit('working tree is not clean - commit everything before releasing')
    head = git('rev-parse', 'HEAD').strip()
    remote = git('rev-parse', 'origin/main').strip()
    if not tag_exists and head != remote:
        sys.exit('HEAD (%s) is not what origin/main has (%s) - push before tagging' % (head[:7], remote[:7]))
    ref = ver if tag_exists else head
    print('releasing %s from %s' % (ver, ref[:8]))

    # 1. Export the released commit as its own source tree: the binaries must come from the tag,
    #    not from whatever the working directory happens to hold today.
    work = os.path.join(args.mirror, 'release-' + ver.lstrip('v'))
    win_work = work.replace('/mnt/c/', 'C:/').replace('\\', '/') if work.startswith('/mnt/c/') else work
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    tar = tarfile.open(fileobj=subprocess.Popen(['git', 'archive', ref], cwd=ROOT,
                                                stdout=subprocess.PIPE).stdout, mode='r|')
    tar.extractall(work)
    print('exported %s -> %s' % (ref[:8], work))

    # 2. Build there with the project's own script; the selftest must pass or nothing is published.
    out = sh([args.pwsh, '-NoProfile', '-Command',
              "Set-Location '%s'; ./build.ps1 -Selftest" % win_work.replace('/', '\\')])
    if 'SELFTEST PASSED' not in out:
        print(out[-2000:])
        sys.exit('the built binary did not pass its own selftest - refusing to publish')
    print([l for l in out.splitlines() if 'built' in l or 'SELFTEST' in l])

    # 3. Versioned names, so a downloaded file can be told apart from yesterday's download.
    dest = os.path.join(ROOT, 'build', 'release', ver)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    assets = []
    for exe, name in (('ShotMvp.exe', 'ShotMvp-%s.exe' % ver),
                      ('ShotMvp_console.exe', 'ShotMvp_console-%s.exe' % ver)):
        srcf = os.path.join(work, 'build', exe)
        if not os.path.isfile(srcf):
            sys.exit('no %s in the build output' % srcf)
        dst = os.path.join(dest, name)
        shutil.copy2(srcf, dst)
        assets.append(dst)

    body = ['Релиз %s.' % ver, '']
    for f in assets:
        body.append('`%s` — %d байт, SHA-256 `%s`' % (os.path.basename(f), os.path.getsize(f), sha256(f)))
    body += ['', 'Собрано из коммита %s скриптом `tools/release.py` (build.ps1 -Selftest на '
                 'собирающей машине); на этих файлах `SELFTEST PASSED (0 failures)`.' % ref[:8], '']

    # 4. What the release contains, taken from the milestone so the text cannot drift from the board.
    if not args.no_milestone and args.milestone:
        ms = [m for m in __import__('json').loads(gh('api', 'repos/%s/milestones' % REPO,
                                                     '-f', 'state=open', '-f', 'per_page=100'))
              if m['title'] == args.milestone]
        if not ms:
            sys.exit('milestone %r not found (open milestones only)' % args.milestone)
        issues = [i for i in __import__('json').loads(gh('api', 'repos/%s/issues' % REPO,
                                                          '-f', 'milestone=%d' % ms[0]['number'],
                                                          '-f', 'state=all', '-f', 'per_page=100'))
                  if 'pull_request' not in i]
        body.append('**Что в этой вехе** (milestone #%d, %s):' % (ms[0]['number'], ms[0]['title']))
        for i in sorted(issues, key=lambda x: x['number']):
            body.append('- #%d %s — %s' % (i['number'], i['title'],
                                           'закрыта' if i['state'] == 'closed' else 'ОСТАЁТСЯ ОТКРЫТОЙ'))
        body.append('')

    notes = tempfile.NamedTemporaryFile('w', suffix='.md', delete=False)
    notes.write('\n'.join(body)); notes.close()

    # 5. Tag (if this version has not been tagged yet) and the release with its binaries.
    if not tag_exists:
        git('tag', '-a', ver, '-m', 'ShotMvp %s' % ver)
        git('push', 'origin', ver)
        print('tagged %s' % ver)
    if gh_ok('release', 'view', ver, '--repo', REPO):
        gh('release', 'edit', ver, '--repo', REPO, '--notes-file', notes.name,
           *[os.path.abspath(f) for f in assets])
    else:
        gh('release', 'create', ver, '--repo', REPO, '--title', 'ShotMvp %s' % ver,
           '--notes-file', notes.name, *[os.path.abspath(f) for f in assets])
    print(gh('release', 'view', ver, '--repo', REPO, '--json', 'tagName,assets',
             '--jq', '"\\(.tagName): \\(.assets | map(.name + \" \" + (.size|tostring)) | join(\", \"))"'))

    if not args.no_milestone and args.milestone:
        ms_no = __import__('json').loads(gh('api', 'repos/%s/milestones' % REPO, '-f', 'state=all',
                                            '-f', 'per_page=100'))
        ms_no = [m for m in ms_no if m['title'] == args.milestone][0]
        open_issues = __import__('json').loads(gh('api', 'repos/%s/issues' % REPO,
                                                  '-f', 'milestone=%d' % ms_no['number'],
                                                  '-f', 'state=open', '-f', 'per_page=100'))
        open_issues = [i for i in open_issues if 'pull_request' not in i]
        print(gh('api', '-X', 'PATCH', 'repos/%s/milestones/%d' % (REPO, ms_no['number']),
                 '-f', 'state=closed', '--jq', '"milestone #\\(.number) \\(.state)"'))
        if open_issues:
            print('open issues left in the milestone (milestone closed anyway): %s'
                  % [i['number'] for i in open_issues])
    print('released %s with %d asset(s)' % (ver, len(assets)))


if __name__ == '__main__':
    main()
