"""The launcher in its three layouts, without a real program or a real account."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# A stand-in for the program: prints what the launcher passed to it.
FAKE_PROGRAM = """#!/bin/sh
printf '%s\\n' "$QT_QPA_PLATFORM" "$CONTRAIL_APP_ID" "$CONTRAIL_DIR" "${QT_PLUGIN_PATH:-unset}" "$@"
"""


class LauncherTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="contrail launcher ")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / "user data"
        self.env = {k: v for k, v in os.environ.items() if k not in {"QT_PLUGIN_PATH", "LD_LIBRARY_PATH"}}
        self.env.update(XDG_DATA_HOME=str(self.data), HOME=str(self.root / "home"))

    def install(self, launcher, program):
        launcher.parent.mkdir(parents=True, exist_ok=True)
        program.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "packaging/contrail", launcher)
        program.write_text(FAKE_PROGRAM)
        program.chmod(0o755)

    def launch(self, launcher, *arguments):
        return subprocess.run([str(launcher), *arguments], env=self.env, check=True,
                              capture_output=True, text=True).stdout.splitlines()

    def test_package_layout_under_any_prefix(self):
        for prefix in (self.root / "usr", self.root / "Contrail.AppDir/usr"):
            with self.subTest(prefix=prefix):
                launcher = prefix / "bin/contrail"
                self.install(launcher, prefix / "libexec/contrail/contrail")
                out = self.launch(launcher, "--", "tg://resolve?domain=example", "two words")
                workdir = str(self.data / "contrail-data") + "/"
                self.assertEqual(out, ["offscreen", "io.github.x_vlad_x.Contrail", str(prefix / "share/contrail"),
                                       "unset", "-workdir", workdir, "--", "tg://resolve?domain=example",
                                       "two words"])

    def test_symlinked_launcher(self):
        prefix = self.root / "opt/contrail"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        link = self.root / "bin/contrail"
        link.parent.mkdir()
        link.symlink_to(prefix / "bin/contrail")
        self.assertEqual(self.launch(link)[2], str(prefix / "share/contrail"))

    def test_development_install(self):
        directory = self.root / "home/.local/share/contrail"
        self.install(directory / "contrail", directory / "bin/contrail")
        out = self.launch(directory / "contrail")
        self.assertEqual(out[2:4], [str(directory), str(directory / "plugins")])

    def test_data_directory_and_url_handler_option(self):
        prefix = self.root / "usr"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        options = self.data / "contrail-data/tdata/experimental_options.json"
        self.launch(prefix / "bin/contrail")
        self.assertEqual(options.read_text().strip(), '{"skip-url-scheme-register": true}')
        # The user's own options are left alone.
        options.write_text('{"custom": 1}')
        self.launch(prefix / "bin/contrail")
        self.assertEqual(options.read_text(), '{"custom": 1}')

    def test_version_needs_no_display_or_data(self):
        prefix = self.root / "usr"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        (prefix / "share/contrail").mkdir(parents=True)
        (prefix / "share/contrail/version").write_text("Contrail 0.1.0-preview1\n")
        env = {k: v for k, v in self.env.items() if k not in {"DISPLAY", "WAYLAND_DISPLAY"}}
        result = subprocess.run([str(prefix / "bin/contrail"), "--version"], env=env, check=True,
                                capture_output=True, text=True)
        self.assertEqual(result.stdout, "Contrail 0.1.0-preview1\n")
        self.assertFalse(self.data.exists())


if __name__ == "__main__":
    unittest.main()
