# Distributing OVI-CODE via Homebrew

OVI-CODE ships as a **Homebrew tap**: a small GitHub repo that holds the
formula. Users then `brew tap` it once and `brew install ovi-code`.

```
packaging/homebrew/
  ovi-code.rb     # the formula (source of truth, with sha256 placeholders)
  README.md       # this file
scripts/
  brew-release.sh # fills in the version + sha256s for a tagged release
```

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

## Cutting a release

1. Tag and push:

   ```bash
   git tag v0.1.0
   git push origin v0.1.0
   ```

2. Generate the filled formula (computes all three sha256s):

   ```bash
   scripts/brew-release.sh v0.1.0
   ```

   This rewrites `packaging/homebrew/ovi-code.rb` in place. To write straight
   into a checked-out tap instead:

   ```bash
   scripts/brew-release.sh v0.1.0 ../homebrew-ovi-code/Formula/ovi-code.rb
   # slug is auto-derived from `git remote get-url origin` (Ovicorps/ovi-code)
   ```

3. Smoke-test the build locally:

   ```bash
   brew install --build-from-source ./packaging/homebrew/ovi-code.rb
   ovi-code --help
   ```

## Publishing the tap (one-time)

Create a repo named `homebrew-ovi-code` under the `Ovicorps` org (the
`homebrew-` prefix is required by Homebrew):

```bash
# on GitHub: create Ovicorps/homebrew-ovi-code, then locally:
git clone https://github.com/Ovicorps/homebrew-ovi-code
mkdir -p homebrew-ovi-code/Formula
cp packaging/homebrew/ovi-code.rb homebrew-ovi-code/Formula/ovi-code.rb
cd homebrew-ovi-code && git add Formula/ovi-code.rb && git commit -m "ovi-code 0.1.0" && git push
```

## Installing (end users)

```bash
brew tap Ovicorps/ovi-code        # adds the Ovicorps/homebrew-ovi-code tap
brew install ovi-code
```

Or in one shot without a separate tap step:

```bash
brew install Ovicorps/ovi-code/ovi-code
```

Bleeding edge from `main`:

```bash
brew install --HEAD Ovicorps/ovi-code/ovi-code
```

> `brew-release.sh` derives the owner/name slug from
> `git remote get-url origin` automatically, so the generated formula always
> points at the right repo even if the remote changes.
