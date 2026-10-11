#!/usr/bin/env bash
#
# brew-release.sh — produce a ready-to-ship Homebrew formula for OVI-CODE.
#
# It computes the sha256 of:
#   1. the GitHub source tarball for a given tag,
#   2. the nlohmann/json tarball pinned in CMakeLists.txt,
#   3. the FTXUI tarball pinned in CMakeLists.txt,
# and writes a filled-in formula (placeholders replaced).
#
# Usage:
#   scripts/brew-release.sh <tag> [output.rb]
#
# Examples:
#   scripts/brew-release.sh v0.1.0
#   scripts/brew-release.sh v0.1.0 ../homebrew-ovi-code/Formula/ovi-code.rb
#
# Prereqs: git, curl, shasum (all present on stock macOS).
# The <tag> must already be pushed to the GitHub remote so the archive URL
# resolves — run `git tag v0.1.0 && git push origin v0.1.0` first.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
template="$here/packaging/homebrew/ovi-code.rb"

tag="${1:-}"
out="${2:-$here/packaging/homebrew/Formula/ovi-code.rb}"

if [[ -z "$tag" ]]; then
  echo "usage: $0 <tag> [output.rb]" >&2
  echo "       e.g. $0 v0.1.0" >&2
  exit 2
fi
if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][A-Za-z0-9.-]+)?$ ]]; then
  echo "tag must be a version starting with v (for example, v0.2.0)" >&2
  exit 2
fi

# --- repo slug (owner/name) from the git remote ----------------------------
remote="$(git -C "$here" remote get-url origin 2>/dev/null || true)"
if [[ -z "$remote" ]]; then
  echo "!! no git 'origin' remote; defaulting slug to Ovicorps/ovi-code" >&2
  slug="Ovicorps/ovi-code"
else
  slug="$(echo "$remote" \
    | sed -E 's#^git@github.com:#https://github.com/#' \
    | sed -E 's#^https://github.com/##' \
    | sed -E 's#\.git$##')"
fi
echo ">> repo slug : $slug" >&2
echo ">> tag       : $tag"  >&2

# --- read the FetchContent tags out of CMakeLists.txt ----------------------
# Keeps json/ftxui versions in lockstep with the actual build.
cml="${OVI_CODE_RELEASE_SOURCE_DIR:-$here}/CMakeLists.txt"
json_url="$(grep -A2 'FetchContent_Declare(json' "$cml" | grep GIT_REPOSITORY | awk '{print $2}')"
json_tag="$(grep -A3 'FetchContent_Declare(json' "$cml" | grep GIT_TAG | awk '{print $2}')"
ftxui_url="$(grep -A2 'FetchContent_Declare(ftxui' "$cml" | grep GIT_REPOSITORY | awk '{print $2}')"
ftxui_tag="$(grep -A3 'FetchContent_Declare(ftxui' "$cml" | grep GIT_TAG | awk '{print $2}')"

json_tarball="${json_url%.git}/archive/refs/tags/${json_tag}.tar.gz"
ftxui_tarball="${ftxui_url%.git}/archive/refs/tags/${ftxui_tag}.tar.gz"
src_tarball="https://github.com/${slug}/archive/refs/tags/${tag}.tar.gz"

# --- sha256 helper ---------------------------------------------------------
sha_of() {
  local url="$1"
  curl -fsSL "$url" | shasum -a 256 | awk '{print $1}'
}

echo ">> hashing source tarball: $src_tarball" >&2
src_sha="$(sha_of "$src_tarball")"
echo ">> hashing json  : $json_tarball" >&2
json_sha="$(sha_of "$json_tarball")"
echo ">> hashing ftxui : $ftxui_tarball" >&2
ftxui_sha="$(sha_of "$ftxui_tarball")"

# --- emit the filled formula ----------------------------------------------
mkdir -p "$(dirname "$out")"
sed \
  -e "s#https://github.com/Ovicorps/ovi-code#https://github.com/${slug}#g" \
  -e "s#REPLACE_WITH_TAG#${tag}#g" \
  -e "s#REPLACE_WITH_JSON_URL#${json_tarball}#g" \
  -e "s#REPLACE_WITH_FTXUI_URL#${ftxui_tarball}#g" \
  -e "s/REPLACE_WITH_SOURCE_TARBALL_SHA256/${src_sha}/" \
  -e "s/REPLACE_WITH_JSON_TARBALL_SHA256/${json_sha}/" \
  -e "s/REPLACE_WITH_FTXUI_TARBALL_SHA256/${ftxui_sha}/" \
  "$template" > "$out.tmp"
mv "$out.tmp" "$out"

echo "" >&2
echo ">> wrote $out" >&2
echo "   source sha256 : $src_sha" >&2
echo "   json   sha256 : $json_sha" >&2
echo "   ftxui  sha256 : $ftxui_sha" >&2
echo "" >&2
echo "Next:" >&2
echo "  brew install --build-from-source $out   # local smoke test" >&2
echo "  # or commit it into your tap repo and: brew install <owner>/ovi-code/ovi-code" >&2
