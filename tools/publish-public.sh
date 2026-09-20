#!/bin/sh
# Ship this repository's history to the public repository. Development stays here, in the private repository that
# still holds the vendor material as untracked files; only what is committed is published, and only after the checks
# below pass. See docs/PUBLISHING.md.
#
#   tools/publish-public.sh --check            just run the checks over the history that would be published
#   tools/publish-public.sh                    checks, then show what would be pushed (dry run)
#   tools/publish-public.sh --push             checks, then push
#   tools/publish-public.sh --push --tag v0.1.0
#
# Options: --ref REF (default master), --remote URL (default the public repository), --force (publish a rewritten
# history; the public repository's own commits are discarded).
set -eu

ref=master
remote_url=https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver.git
mode=dry-run
tag=
force=
while [ $# -gt 0 ]; do
    case $1 in
        --check) mode=check ;;
        --push) mode=push ;;
        --ref) ref=${2:?--ref needs a branch}; shift ;;
        --remote) remote_url=${2:?--remote needs a URL}; shift ;;
        --tag) tag=${2:?--tag needs a name}; shift ;;
        --force) force=--force ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "publish-public: unknown argument '$1'; try --help" >&2; exit 2 ;;
    esac
    shift
done

# A source tarball has no repository to publish from; the CTest that runs --check skips on 77.
git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "SKIP: not a git checkout, nothing to publish"; exit 77; }
cd "$(git rev-parse --show-toplevel)"
git rev-parse --verify --quiet "$ref^{commit}" >/dev/null || { echo "publish-public: no such ref '$ref'" >&2; exit 1; }

# Every path that ever appears in the history being published. The vendor's driver files and everything derived
# from them belong to their owners and must never leave this machine; they are untracked and ignored here, and the
# history was rewritten to remove them, so a hit means something reintroduced one.
#
# analysis/ghidra_project/ is on this list because a Ghidra .rep database holds the *imported program*: the
# vendor DLL's bytes and the whole analysis of it. It was missed when this check was first written and reached
# the public repository in the v0.1.0 push (2026-09-20), which is why both histories were rewritten again.
echo "publish-public: checking every path in the history of '$ref'"
paths=$(git log --format= --name-only "$ref" | sort -u |
        grep -E '^(vendor/|analysis/decompiled/|analysis/disassembly/|analysis/ghidra_project/)' || true)
if [ -n "$paths" ]; then
    echo "publish-public: REFUSED: vendor material in the history to be published:" >&2
    echo "$paths" | sed 's/^/  /' >&2
    echo "Rewrite the history (git filter-repo --invert-paths --path vendor ...) before publishing." >&2
    exit 1
fi

# Compiled executables and libraries, whoever they belong to. Nothing this project builds is committed, so any of
# these in the history is either someone else's binary to redistribute (a third-party .exe reached the public
# repository the same way the Ghidra database did) or a build artefact that should never have been added.
echo "publish-public: checking the history for compiled binaries"
binaries=$(git log --format= --name-only "$ref" | sort -u |
           grep -iE '\.(exe|dll|sys|ocx|so|so\.[0-9]+|a|o|obj|lib|pdb|pkg\.tar\.[a-z]+|deb|rpm)$' || true)
if [ -n "$binaries" ]; then
    echo "publish-public: REFUSED: compiled binaries in the history to be published:" >&2
    echo "$binaries" | sed 's/^/  /' >&2
    echo "Rewrite the history to remove them before publishing." >&2
    exit 1
fi

# A real VIN identifies the car, so the captures were redacted with analysis/redact_vin.py. YV1TESTVIN0000000 is the
# invented one the tests use. Volvo VINs start YV1; pass MONGOOSE_PUBLISH_VIN_RE to widen this for another make.
vin_pattern=${MONGOOSE_PUBLISH_VIN_RE:-YV1[A-HJ-NPR-Z0-9]{14\}}
echo "publish-public: scanning the history for vehicle identification numbers"
vins=$(git grep -I -h -E -o "$vin_pattern" $(git rev-list "$ref") -- 2>/dev/null | sort -u | grep -v '^YV1TESTVIN0000000$' || true)
if [ -n "$vins" ]; then
    echo "publish-public: REFUSED: what look like real VINs are in the history:" >&2
    echo "$vins" | sed 's/^/  /' >&2
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "publish-public: note: the working tree has uncommitted changes; only committed work is published"
fi
echo "publish-public: checks passed for $(git rev-parse --short "$ref") ($(git rev-list --count "$ref") commits)"
[ "$mode" = check ] && exit 0

# A named remote keeps the URL out of the reflog of every push and makes an accidental push to the private
# repository impossible: the refspec below names this remote alone.
if git remote get-url public >/dev/null 2>&1; then
    current=$(git remote get-url public)
    [ "$current" = "$remote_url" ] || { echo "publish-public: remote 'public' is $current, not $remote_url" >&2; exit 1; }
else
    echo "publish-public: adding remote 'public' -> $remote_url"
    git remote add public "$remote_url"
fi

if [ "$mode" = dry-run ]; then
    echo "publish-public: dry run; nothing is pushed. It would push:"
    git push --dry-run $force public "$ref:refs/heads/master"
    [ -n "$tag" ] && git push --dry-run public "refs/tags/$tag"
    echo "publish-public: re-run with --push to publish."
    exit 0
fi

git push $force public "$ref:refs/heads/master"
if [ -n "$tag" ]; then
    git rev-parse --verify --quiet "refs/tags/$tag" >/dev/null || git tag -a "$tag" -m "$tag" "$ref"
    git push public "refs/tags/$tag"
fi
echo "publish-public: published $(git rev-parse --short "$ref") to $remote_url"
