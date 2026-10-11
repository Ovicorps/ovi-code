# Homebrew formula for OVI-CODE.
#
# This file is the source of truth for the formula. It is copied into a
# Homebrew tap (e.g. ovicorps/homebrew-ovi-code -> Formula/ovi-code.rb) by
# `scripts/brew-release.sh`, which also fills in the version, source URL and
# all sha256 checksums for a given git tag.
#
# The build is fully offline: nlohmann/json and FTXUI are vendored as Homebrew
# `resource`s and handed to CMake via FETCHCONTENT_SOURCE_DIR_* so FetchContent
# never touches the network inside Homebrew's build sandbox.
class OviCode < Formula
  desc "Tiny terminal-based code-editing TUI agent (ReAct, multi-provider)"
  homepage "https://github.com/Ovicorps/ovi-code"
  url "https://github.com/Ovicorps/ovi-code/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "REPLACE_WITH_SOURCE_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/Ovicorps/ovi-code.git", branch: "main"

  depends_on "cmake" => :build

  # libcurl: bundled with macOS; on Linux pull from Homebrew.
  uses_from_macos "curl"
  on_linux do
    depends_on "curl"
  end

  # Pinned to the GIT_TAGs in CMakeLists.txt. Keep these in sync when the
  # FetchContent_Declare tags change.
  resource "json" do
    url "https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz"
    sha256 "REPLACE_WITH_JSON_TARBALL_SHA256"
  end

  resource "ftxui" do
    url "https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz"
    sha256 "REPLACE_WITH_FTXUI_TARBALL_SHA256"
  end

  def install
    json_src = buildpath/"_brew_deps/json"
    ftxui_src = buildpath/"_brew_deps/ftxui"
    json_src.install resource("json")
    ftxui_src.install resource("ftxui")

    system "cmake", "-S", ".", "-B", "build",
           "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
           "-DFETCHCONTENT_SOURCE_DIR_JSON=#{json_src}",
           "-DFETCHCONTENT_SOURCE_DIR_FTXUI=#{ftxui_src}",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "OVI-CODE", shell_output("#{bin}/ovi-code --help")
    assert_match "Deliberate", shell_output("#{bin}/ovi-code plugins")
  end
end
