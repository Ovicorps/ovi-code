# typed: strict
# frozen_string_literal: true

# Template for Casks/ovi-code.rb in the Ovicorps/homebrew-ovi-code tap.
# Run scripts/brew-cask-release.sh after the tagged GitHub release completes.
cask "ovi-code" do
  arch arm: "arm64", intel: "x86_64"

  version "REPLACE_WITH_VERSION"
  sha256 arm:   "REPLACE_WITH_ARM64_SHA256",
         intel: "REPLACE_WITH_X86_64_SHA256"

  url "https://github.com/Ovicorps/ovi-code/releases/download/v#{version}/ovi-code-darwin-#{arch}.tar.gz"
  name "OVI-CODE"
  desc "Terminal-based code-editing TUI agent powered by OpenRouter"
  homepage "https://github.com/Ovicorps/ovi-code"

  depends_on macos: ">= :ventura"

  binary "ovi-code"

  zap trash: "~/.ovi-code"
end
