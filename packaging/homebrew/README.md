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
```

## How the cask works

Pushing a `v*` tag runs `.github/workflows/release.yml`. It builds native
Apple Silicon and Intel binaries, packages them as `.tar.gz` archives, records
their SHA-256 checksums, and attaches everything to a GitHub release. The cask
selects the matching archive with Homebrew's `on_arm` / `on_intel` blocks.

The source repository and its release assets must be publicly downloadable for
public Homebrew installs. The tap repository must also exist at
`Ovicorps/homebrew-ovi-code`.

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
brew install --cask ovi-code
```

Or in one shot without a separate tap step:

```bash
brew install --cask Ovicorps/ovi-code/ovi-code
```

Build the latest `main` branch from source:

```bash
brew install --HEAD Ovicorps/ovi-code/ovi-code
```

> `brew-release.sh` derives the owner/name slug from
> `git remote get-url origin` automatically, so the generated formula always
> points at the right repo even if the remote changes.
