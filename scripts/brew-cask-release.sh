#!/usr/bin/env bash
# Generate a Homebrew cask from the macOS archives attached to a GitHub release.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
template="$here/packaging/homebrew/ovi-code-cask.rb"
tag="${1:-}"
out="${2:-$here/packaging/homebrew/Casks/ovi-code.rb}"

if [[ -z "$tag" ]]; then
  echo "usage: $0 <tag> [output.rb]" >&2
  echo "       e.g. $0 v0.2.0 ../homebrew-ovi-code/Casks/ovi-code.rb" >&2
  exit 2
fi
if [[ "$tag" != v* ]]; then
  echo "tag must start with 'v' (for example, v0.2.0)" >&2
  exit 2
fi

remote="$(git -C "$here" remote get-url origin 2>/dev/null || true)"
slug="${remote#https://github.com/}"
slug="${slug#git@github.com:}"
slug="${slug%.git}"
if [[ -z "$slug" || "$slug" == "$remote" ]]; then
  slug="Ovicorps/ovi-code"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo ">> downloading $tag release archives from $slug" >&2
gh release download "$tag" --repo "$slug" --dir "$tmp" \
  --pattern 'ovi-code-darwin-arm64.tar.gz' \
  --pattern 'ovi-code-darwin-x86_64.tar.gz'

arm_archive="$tmp/ovi-code-darwin-arm64.tar.gz"
intel_archive="$tmp/ovi-code-darwin-x86_64.tar.gz"
arm_sha="$(shasum -a 256 "$arm_archive" | cut -d ' ' -f 1)"
intel_sha="$(shasum -a 256 "$intel_archive" | cut -d ' ' -f 1)"
version="${tag#v}"

mkdir -p "$(dirname "$out")"
sed \
  -e "s/REPLACE_WITH_VERSION/$version/" \
  -e "s/REPLACE_WITH_ARM64_SHA256/$arm_sha/" \
  -e "s/REPLACE_WITH_X86_64_SHA256/$intel_sha/" \
  "$template" > "$out"

echo ">> wrote $out" >&2
echo "   arm64 sha256 : $arm_sha" >&2
echo "   x86_64 sha256: $intel_sha" >&2
