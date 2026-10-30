# CI cache credential boundaries

## Trusted refs and Linux dependency image publisher

The publisher runs only for pushes to `dev` in `teagramhq/teagram-desktop` and
checks out that trusted commit. Pull request workflows cannot invoke its write
token or choose the image tag. The tag is derived from the tracked Linux
dependency recipe, and the job receives only `contents: read` and
`packages: write`. Pull request Linux jobs receive `packages: read` only.

## Post-merge Linux build

`Linux.` runs on the `dev` push itself, never from `workflow_run`. A
`workflow_run` build ran in the default-branch context: its check attached to
the current `dev` head while it checked out the source run's commit, and its
cache token could not write the `dev` scope. On the push, the check, the
checkout and the cache scope all belong to the pushed commit, and the build
fails before compiling unless the checked-out commit is `GITHUB_SHA`.

The publisher is the `publish-linux-deps-image` job of the same run, and the
build waits for it, so the build pulls the image of its own recipe. It is the
only job with `packages: write`. The build saves the compiler cache only when the
run is not a pull request and its ref is `refs/heads/dev`: a push to `dev`,
the nightly schedule, or a manual dispatch on `dev`. Each of those runs
trusted `dev` code. Pull request builds restore that cache and never save.

## macOS dependency artifact publisher

The `check-macos-deps-artifact` inspection job runs on `ubuntu-latest` only for
a push or schedule on `dev` in `teagramhq/teagram-desktop`. It checks out that
trusted ref, tags the GHCR artifact with the hash of the tracked macOS dependency
recipe, and reports whether the tag already exists. It receives `packages: read`
only.

The `build-macos-deps-artifact` job runs on macOS only when the inspection
reports a missing tag. It is the only job with `packages: write`; it restores
the matching legacy cache or builds the macOS dependencies, then publishes the
artifact. Pull request builds cannot publish a package. The macOS build,
packaged build, updater fixtures, and manual unit-feedback jobs receive
`packages: read` only.

The `ops-prune-macos-libraries` job runs on `ubuntu-latest` after an artifact hit or
a successful publish. Its `actions: write` token is used only to delete legacy
`macOS-libs-` Actions cache entries. Those caches are no longer written by any
macOS workflow. Consumers read a matching legacy cache only when the GHCR
artifact is missing. The publisher can use the old cache to seed the first
artifact, then the prune job deletes every legacy entry. Only trusted dev runs
can publish package contents.

## Manual feedback lanes

`Unit feedback.` and `MacOS unit feedback.` are dispatched on `dev` but build
a selected feature ref, so that ref's preparation and configure code runs
with the `dev` cache token. Both workflows set `cache-mode: read`: the cache
service refuses any save, so a feature ref cannot plant or overwrite entries
that trusted `dev` builds restore, whatever code it runs. Neither workflow
contains a save step, and `test_ci_cache_contract.sh` fails if either regains
cache write access. The Linux lane pulls the dependency image `dev`'s
publisher pushed for the ref's recipe with `packages: read`; on a missing tag
it builds the image locally and never pushes it. MacOS unit feedback uses
`packages: read` to pull the matching macOS dependency artifact, then tries a
read-only legacy cache before building dependencies locally.

## Actions cache cleanup

The `pull_request_target` cleanup runs only when a PR closes. It does not check
out, download, or execute PR code. Its only PR-derived input is GitHub's
numeric PR number, validated before it is used to construct the exact
`refs/pull/<number>/merge` cache scope. It does not use the PR title, body,
branch name, labels, or head repository in a shell command. Its job token has
`actions: write` and no contents or package permissions; it deletes caches
only from that PR merge ref.

The dev cache cleanup runs after a completed `MacOS.` or `MacOS Packaged.`
workflow, since either can save a dev compiler cache. It verifies
that GitHub reports both the source branch as `dev` and the source repository
as this repository. It does not check out or run source-workflow artifacts.
Its `actions: write` token is used only to list and delete cache IDs returned
for the exact `refs/heads/dev` ref and the `macOS-arm64-ccache-dev-` key
prefix. It keeps the newest entry of each profile, including the unprofiled
key `MacOS Packaged.` saves. PR-controlled workflow runs fail the source validation and cannot
select a cache key or ID.

The trusted `ops-prune-macos-libraries` job deletes the legacy `macOS-libs-`
cache entries from every ref only after the matching GHCR artifact is present.
This frees the former 9 GB dependency cache before the app build saves its
compiler cache. Pull request workflows have no write access to Actions caches
or packages, so they cannot delete caches or publish artifacts.

A completed `Linux.` run triggers a separate job with the same source
validation and token scope. It lists only the `Linux-tdesktop-default-` key
prefix on `refs/heads/dev`, keeps the newest entry, and deletes the older
ones. Each run ID-keyed save on `dev` otherwise stays until eviction. The
Linux dependency image lives in GHCR, not the Actions cache, so this job never
touches it.

Cache deletion can cause a later build to compile more work. It cannot change
source, packages, or deployed binaries; caches are reproducible. Reverting
the workflow change restores the previous cleanup behavior, while a missed
cache can be regenerated by the next trusted build.
