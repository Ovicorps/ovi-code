# Distributing OVI-CODE via Homebrew

OVI-CODE ships through a **Homebrew tap** containing both a prebuilt macOS cask
and an optional source-build formula.

```
packaging/homebrew/
  ovi-code.rb          # source-build formula template
  ovi-code-cask.rb     # prebuilt macOS cask template
  README.md
scripts/
  brew-release.sh       # generates the formula
  brew-cask-release.sh  # generates the cask from release archives
.github/workflows/
   release.yml           # builds arm64/x86_64 archives for every v* tag
   homebrew.yml          # synchronizes the tap with GitHub's latest release
```

## How the cask works

Pushing a `v*` tag runs `.github/workflows/release.yml`. It builds native
Apple Silicon and Intel binaries, packages them as `.tar.gz` archives, records
their SHA-256 checksums, and attaches everything to a GitHub release. The cask
selects the matching archive with Homebrew's `on_arm` / `on_intel` blocks. Each
archive contains the executable and the bundled Deliberate plugin; the source
formula installs the same plugin through CMake.

The source repository and its release assets must be publicly downloadable for
public Homebrew installs. The tap repository must also exist at
`Ovicorps/homebrew-ovi-code`.

## Automatic latest-release updates

After release assets are published, the Release workflow calls **Update Homebrew
tap**. It resolves GitHub's `/releases/latest` endpoint (excluding drafts and
prereleases), generates both the cask and source formula with SHA-256 checksums,
and commits them directly to `Ovicorps/homebrew-ovi-code`. Dependency resource
versions come from the released tag, not unreleased changes on `main`.

The same workflow runs on published/edited release events, hourly as a recovery
check, and manually through **Actions → Update Homebrew tap → Run workflow**.
GitHub's default workflow token cannot trigger a second workflow through a release
event, so the explicit call from Release ensures immediate synchronization.
Updates are serialized, unchanged files produce no commit, and a final latest-tag
check prevents a stale release from overwriting a newer one.

### Required one-time configuration

1. Create and initialize the public `Ovicorps/homebrew-ovi-code` repository with
   a default branch.
2. Create a fine-grained GitHub token scoped to that repository with **Contents:
   read and write** permission. The token owner must have write access; approve
   organization access if required. Its permissions must allow pushes to the
   tap's default branch, including any branch-protection requirements.
3. Store the token in this source repository's Actions secret
   **`HOMEBREW_TAP_TOKEN`**. The normal `GITHUB_TOKEN` cannot write to another repo.
4. Run **Update Homebrew tap** once to synchronize the existing latest release.

Missing credentials or release archives fail the workflow rather than publishing
a cask with missing checksums. Keep the secret valid to maintain synchronization.
The manual steps below remain available as a fallback.

### Repair a release with missing macOS archives

Run **Actions → Release → Run workflow** on `main`, entering the existing tag
(for example, `v0.1.1`). The workflow builds and tests that exact tagged source
on native Apple Silicon and Intel macOS 26 runners, uploads missing archives
and checksums to the existing release, then synchronizes Homebrew. Existing
assets are kept rather than replaced, preserving published checksums. The newer
runner toolchain is required for the app's C++20 cancellation support; packaged
binaries still target macOS 13 and newer.

## How the formula builds offline

CMake fetches `nlohmann/json` and `FTXUI` via `FetchContent` at configure
time. Homebrew's build sandbox blocks network access, so the formula instead
declares both as Homebrew `resource`s, downloads them up front, and points
CMake at the unpacked sources with `FETCHCONTENT_SOURCE_DIR_JSON` /
`FETCHCONTENT_SOURCE_DIR_FTXUI` plus `FETCHCONTENT_FULLY_DISCONNECTED=ON`.
No CMake changes are required and the build never hits the network.

The `resource` versions are pinned to the same `GIT_TAG`s as
`CMakeLists.txt`. `scripts/brew-release.sh` reads those tags straight from
`CMakeLists.txt`, so they stay in lockstep automatically.

## Cutting a cask release

1. Update the project version in `CMakeLists.txt`, then tag and push:

   ```bash
   git tag v0.2.0
   git push origin v0.2.0
   ```

2. Wait for the `Release` GitHub Actions workflow to publish both macOS
   archives.

3. Generate the cask directly into a checked-out tap:

   ```bash
   scripts/brew-cask-release.sh v0.2.0 \
     ../homebrew-ovi-code/Casks/ovi-code.rb
   ```

4. Audit and test the cask:

   ```bash
   brew audit --cask --strict ../homebrew-ovi-code/Casks/ovi-code.rb
   brew install --cask ../homebrew-ovi-code/Casks/ovi-code.rb
   ovi-code --help
   ```

5. Commit and push `Casks/ovi-code.rb` in the tap.

## Updating the source formula

The formula remains available for users who prefer a local source build. After
the tag is public, generate and smoke-test it with:

```bash
scripts/brew-release.sh v0.2.0 \
  ../homebrew-ovi-code/Formula/ovi-code.rb
brew install --build-from-source \
  ../homebrew-ovi-code/Formula/ovi-code.rb
```

## Publishing the tap (one-time)

Create a repo named `homebrew-ovi-code` under the `Ovicorps` org (the
`homebrew-` prefix is required by Homebrew):

```bash
# on GitHub: create Ovicorps/homebrew-ovi-code, then locally:
git clone https://github.com/Ovicorps/homebrew-ovi-code
mkdir -p homebrew-ovi-code/Formula
mkdir -p homebrew-ovi-code/Casks
```

## Installing (end users)

```bash
brew tap Ovicorps/ovi-code        # adds the Ovicorps/homebrew-ovi-code tap
brew update
brew install --cask ovi-code
```

Or in one shot without a separate tap step:

```bash
brew update
brew install --cask Ovicorps/ovi-code/ovi-code
```

Homebrew installs the version in its local tap metadata, so `brew update` is
necessary to guarantee fresh metadata when auto-updates are disabled or cached.
The tap is updated after publication; wait for **Update Homebrew tap** to finish.
For an already installed copy, use:

```bash
brew update
brew upgrade --cask Ovicorps/ovi-code/ovi-code
```

Build the latest `main` branch from source:

```bash
brew install --HEAD Ovicorps/ovi-code/ovi-code
```

> `brew-release.sh` derives the owner/name slug from
> `git remote get-url origin` automatically, so the generated formula always
> points at the right repo even if the remote changes.
