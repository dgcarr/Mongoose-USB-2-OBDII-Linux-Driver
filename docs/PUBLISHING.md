# Publishing to the public repository

This project lives in two places. Work happens in the **private** repository, which is also where the vendor's
Windows driver files and the Ghidra decompilations made from them sit on disk (`vendor/`,
`analysis/decompiled/`, `analysis/disassembly/`: untracked, ignored, never committed). What is published is the
**public** repository, <https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver>, which people clone to use the
driver.

Publishing is one command:

```sh
tools/publish-public.sh            # checks, then a dry run: what would be pushed
tools/publish-public.sh --push     # checks, then push master to the public repository
```

## What the script checks first

It refuses to publish rather than warn, because a push cannot be taken back: anyone may have cloned in between,
and GitHub keeps unreferenced commits reachable by hash.

1. **Vendor material anywhere in the history.** Every path in every commit of the branch being published is
   examined, not just the current tree. Those files belong to their owners; the history was rewritten once
   (`git filter-repo --invert-paths`) to remove them, and a hit means something has reintroduced one. Deleting the
   file in a later commit does not help: the blob is still in the history, which is exactly what this catches.
2. **Vehicle identification numbers.** The captures were redacted with `analysis/redact_vin.py`. Anything matching a
   Volvo VIN (`YV1` and fourteen more characters) fails the check, except `YV1TESTVIN0000000`, the invented one the
   tests use. For another make, set `MONGOOSE_PUBLISH_VIN_RE` to a pattern that matches its VINs.

`ctest -R publishable` runs these checks against HEAD, so the ordinary test run notices a problem long
before a release. HEAD is used because a GitHub pull_request checkout is detached and has no local `master`.

The script pushes through a named remote, `public`, and refuses to run if that remote already points somewhere
else, so a slip cannot push private work to the wrong place. It pushes one refspec, `master:refs/heads/master`, and
never `--all`: no other branch, and no `refs/pull/*`, goes with it.

## The usual cycle

1. Work, commit and push in the private repository as normal. CI runs there.
2. When a change is ready for users, run `tools/publish-public.sh` (dry run) and read what it would push.
3. `tools/publish-public.sh --push`.
4. For a release, `tools/publish-public.sh --push --tag v0.1.0`. Move the CHANGELOG's `Unreleased` section under
   that version number first, and commit that.

Because both repositories share the same history, a plain push fast-forwards. If the history is ever rewritten
again, `--force` is needed and every existing clone of the public repository becomes wrong; say so in the release
notes if it happens.

## Why a separate repository at all

The private repository's **pull-request refs** (`refs/pull/1/head`, `refs/pull/2/head`) still contain the vendor
files. They are managed by GitHub and cannot be rewritten or deleted from here, so making that repository public
would expose them. The public repository was created fresh from the scrubbed history instead. Do not make the
private repository public, and do not push its pull-request refs anywhere.

## What went wrong once, and what now stops it

The first public push (v0.1.0, 2026-09-20) carried two things it should not have, both added in the
**initial commit** and deleted from the tree later, so neither was visible at HEAD and both were still
reachable in the published history:

- `analysis/ghidra_project/` -- the Ghidra project database, about 40 MiB. This is vendor material: a
  `.rep` database holds the **imported program**, so `monpj432.dll`'s bytes and the whole analysis of it
  were in it. The path check listed `vendor/`, `analysis/decompiled/` and `analysis/disassembly/` but not
  this one, which was an oversight rather than a decision: the same files are gitignored precisely because
  they are regenerable vendor-derived material.
- `reference/openvehiclediag.exe` -- a 10.6 MiB third-party Windows binary, 88% of the repository's size,
  and not ours to redistribute either.

Both histories were rewritten with `git filter-repo --invert-paths` and force-pushed, and `v0.1.0` was
re-tagged. The published **tree** never changed: the tree hash at the tip was identical before and after,
so no released file was affected, only history that should never have been there. The repository went from
25 MiB to 1.4 MiB.

The checks above now cover both cases -- `analysis/ghidra_project/` is in the path list, and a separate
check refuses any compiled binary (`.exe`, `.dll`, `.so`, `.a`, `.deb`, a built package, and so on)
anywhere in the history. Both were verified to fire against the pre-rewrite history.

**The lesson worth keeping:** `git status` and a look at the working tree tell you nothing about this.
Only a scan of every path in every commit does, which is what these checks do and why they refuse rather
than warn. Anyone who cloned the public repository between the first push and the rewrite still has the
old blobs, and GitHub can keep unreferenced objects reachable by hash until it garbage-collects; ask
GitHub support to purge them if that matters.

## What is not checked

- **The adapter's serial number** (`AOLHE0000003666A`) appears in the captures, a test string and the docs. It
  identifies the adapter, not the car, and the decision (2026-09-20) was to keep it.
- Anything a new kind of secret would need: there are no credentials or tokens in this repository, and nothing
  scans for them. `/home/dgcarr` paths appear in notes and scripts; cosmetic.
