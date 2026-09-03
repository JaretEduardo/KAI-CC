# Releasing KAI-CC

This is the maintainer-facing checklist for cutting a KAI-CC pre-release. It
exists because the audit for `v0.1.0-alpha.3` found no authoritative,
written release process anywhere in the repository - alpha.1 and alpha.2
were released from convention and memory. This document is intentionally
generic (`<version>`/`<extension-version>` placeholders) so it can be reused
for later releases rather than rewritten each time.

This process does **not** run automatically - no CI job tags, creates a
release, or uploads assets. Every step below is a deliberate, manual,
human-approved action.

## 1. Prepare the release branch

Branch from `master` as `release/<version>` (e.g. `release/0.1.0-alpha.3`),
following the repository's existing branch-naming convention (see
`CONTRIBUTING.md`).

## 2. Version, docs, and package updates

On the release branch:

- Bump the authoritative compiler version: `CMakeLists.txt`'s
  `KAI_CC_PRERELEASE_SUFFIX`, plus every live (non-historical) assertion
  that hard-codes the version string - currently `tests/VersionTests.cpp`
  and `scripts/test-windows-fresh-user.ps1`.
- Bump the VS Code extension version if this release bundles a materially
  different compiler: `editors/vscode/package.json` and the two root-package
  version fields in `editors/vscode/package-lock.json` (not unrelated
  dependency entries that happen to share the old version number).
- Add a new `## <version>` section to `CHANGELOG.md` and a new
  `docs/releases/<version>.md` following the existing format - see prior
  release notes for the expected structure.
- Update `README.md`'s current-version/status references (never rewrite
  genuinely historical statements, e.g. which release first shipped a given
  platform artifact).
- Add any newly-verified-executable examples to both release scripts'
  curated lists (`scripts/build-release-linux-x86_64.sh`,
  `scripts/build-release-windows-x86_64.sh`), with their exact expected
  stdout verified against an actual build first - see
  `examples/README.md`'s own curation policy.
- Fix any other stale "current status" claim an audit finds. Never rewrite
  `docs/releases/<older-version>.md` files or historical milestone markers
  (e.g. `post-alpha.2`) - those describe when a feature shipped, not what's
  current now.

## 3. PR to `master`

Open a pull request from `release/<version>` to `master`, same as any other
change.

## 4. All seven CI checks green

The alpha.3-era merge gate is seven required checks (see
`.github/workflows/ci.yml`): AI-native benchmark isolation, Compiler Linux
x86_64 portable release, Compiler Windows x86_64 build/test/package, VS Code
extension unit tests, VS Code Linux x64 VSIX, VS Code Windows x64 VSIX, and
Windows fresh-user smoke. Do not merge with any of them red or skipped.

## 5. Merge

Merge the PR into `master` (do not squash away the release-prep history
without reason - prior releases used ordinary merge commits).

## 6. Tag

```sh
git tag v<version>
```

on the merge commit.

## 7. Push the tag

```sh
git push origin v<version>
```

## 8. Obtain/build the four release assets

- **Linux archive** (`kai-<version>-linux-x86_64.tar.gz`) - either from the
  `compiler` CI job's build evidence, or built locally via
  `scripts/build-release-linux-x86_64.sh` (requires Podman or Docker).
- **Windows archive** (`kai-<version>-windows-x86_64.zip`) - from the
  `compiler-windows` CI job's build evidence (cannot be produced outside an
  MSYS2 UCRT64 environment - do not attempt to fake this on another
  platform).
- **Linux VSIX** (`kai-language-linux-x64-<extension-version>.vsix`) - from
  the `vscode-linux-package` CI job.
- **Windows VSIX** (`kai-language-win32-x64-<extension-version>.vsix`) -
  from the `vscode-windows-package` CI job.

Each release script also produces its own per-archive `.sha256` sidecar
(informational only - see step 9 for the official combined file).

## 9. Create the official combined `SHA256SUMS`

The final GitHub release includes exactly one `SHA256SUMS` file, covering
exactly the four binary/package assets above - never its own hash, never a
build-evidence or intermediate file:

```
kai-<version>-linux-x86_64.tar.gz
kai-<version>-windows-x86_64.zip
kai-language-linux-x64-<extension-version>.vsix
kai-language-win32-x64-<extension-version>.vsix
```

Once all four files are collected in one place (there is currently no
script that does this automatically across both platforms' CI jobs):

```sh
sha256sum \
    kai-<version>-linux-x86_64.tar.gz \
    kai-<version>-windows-x86_64.zip \
    kai-language-linux-x64-<extension-version>.vsix \
    kai-language-win32-x64-<extension-version>.vsix \
    > SHA256SUMS
```

## 10. Create the GitHub pre-release

Create a GitHub **pre-release** (not a final release, given alpha status)
tagged `v<version>`, using the new `docs/releases/<version>.md` content (or
a summary of it) as the release description.

## 11. Upload assets

Upload all five files to the release:

- Linux archive
- Windows archive
- Linux VSIX
- Windows VSIX
- `SHA256SUMS`

## 12. Verify hashes/downloads

Download each of the four assets fresh (not from local build output) and
confirm each hashes to exactly the value recorded in `SHA256SUMS`.

## 13. Smoke-test the public assets

Run through the README's Linux and Windows quickstarts against the actual
downloaded, published assets - not a local build - confirming `kaicc
--version` reports the expected version and `hello.kai` compiles and runs.
This is the same thing `scripts/test-windows-fresh-user.ps1` already proves
for Windows in CI; doing it by hand once against the real published URLs
closes the gap between "CI-built" and "actually downloadable."
