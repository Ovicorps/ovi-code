import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class HomebrewReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ovi-code-homebrew-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}")
        self.stub("git", '#!/bin/sh\nprintf "https://github.com/Ovicorps/ovi-code.git\\n"\n')
        self.stub("curl", '#!/usr/bin/env python3\nimport sys\nsys.stdout.write("archive:" + sys.argv[-1])\n')
        self.stub("gh", '''#!/usr/bin/env python3
import pathlib, sys
directory = pathlib.Path(sys.argv[sys.argv.index("--dir") + 1])
for arch in ("arm64", "x86_64"):
    (directory / f"ovi-code-darwin-{arch}.tar.gz").write_bytes(arch.encode())
''')

    def stub(self, name, contents):
        path = self.bin / name
        path.write_text(contents)
        path.chmod(0o755)

    def generate(self, script, output, tag="v9.8.7"):
        return subprocess.run(
            ["bash", str(ROOT / "scripts" / script), tag, str(output)],
            env=self.env, text=True, capture_output=True,
        )

    def test_formula_uses_released_dependencies_and_preserves_template(self):
        source = self.directory / "released-source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text('''FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.99.0
)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v99.0.0
)
''')
        self.env["OVI_CODE_RELEASE_SOURCE_DIR"] = str(source)
        template = ROOT / "packaging/homebrew/ovi-code.rb"
        before = template.read_bytes()
        output = self.directory / "Formula/ovi-code.rb"
        result = self.generate("brew-release.sh", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        formula = output.read_text()
        for url in (
            "https://github.com/Ovicorps/ovi-code/archive/refs/tags/v9.8.7.tar.gz",
            "https://github.com/nlohmann/json/archive/refs/tags/v3.99.0.tar.gz",
            "https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v99.0.0.tar.gz",
        ):
            self.assertIn(url, formula)
            self.assertIn(hashlib.sha256(f"archive:{url}".encode()).hexdigest(), formula)
        self.assertNotIn("REPLACE_WITH_", formula)
        self.assertEqual(template.read_bytes(), before)

    def test_cask_has_exact_architecture_checksums(self):
        output = self.directory / "Casks/ovi-code.rb"
        result = self.generate("brew-cask-release.sh", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        cask = output.read_text()
        self.assertIn('version "9.8.7"', cask)
        for arch in ("arm64", "x86_64"):
            self.assertIn(hashlib.sha256(arch.encode()).hexdigest(), cask)
        self.assertIn("strategy :github_latest", cask)
        self.assertNotIn("REPLACE_WITH_", cask)

    def test_failed_download_does_not_replace_existing_cask(self):
        self.stub("gh", "#!/bin/sh\nexit 1\n")
        output = self.directory / "ovi-code.rb"
        output.write_text("previous valid cask")
        result = self.generate("brew-cask-release.sh", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_text(), "previous valid cask")

    def test_invalid_tag_is_rejected(self):
        result = self.generate("brew-release.sh", self.directory / "ovi-code.rb", "invalid")
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
