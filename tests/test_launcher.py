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
printf '%s\\n' "$CONTRAIL_DIR" "$@"
"""


class LauncherTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="contrail launcher ")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.data = self.root / "user data"
        self.env = {k: v for k, v in os.environ.items()
                    if k not in {"LD_LIBRARY_PATH", "XDG_DATA_DIRS", "GIO_MODULE_DIR",
                                 "GSETTINGS_SCHEMA_DIR", "GDK_PIXBUF_MODULE_FILE"}}
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
        for prefix in (self.root / "usr/lib/contrail", self.root / "usr/lib/contrail-td",
                       self.root / "Contrail.AppDir/usr", self.root / "app"):
            with self.subTest(prefix=prefix):
                launcher = prefix / "bin/contrail"
                self.install(launcher, prefix / "libexec/contrail/contrail")
                out = self.launch(launcher, "--", "tg://resolve?domain=example", "two words")
                self.assertEqual(out, [str(prefix / "share/contrail"), "--",
                                       "tg://resolve?domain=example", "two words"])

    def test_symlinked_launcher(self):
        prefix = self.root / "opt/contrail"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        link = self.root / "bin/contrail"
        link.parent.mkdir()
        link.symlink_to(prefix / "bin/contrail")
        self.assertEqual(self.launch(link), [str(prefix / "share/contrail")])

    def test_legacy_data_is_left_alone(self):
        prefix = self.root / "usr"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        options = self.data / "contrail-data/tdata/experimental_options.json"
        options.parent.mkdir(parents=True)
        options.write_text('{"custom": 1}')
        self.launch(prefix / "bin/contrail")
        self.assertEqual(options.read_text(), '{"custom": 1}')

    def test_bundled_gtk_resources(self):
        # DEB, RPM and AppImage: GTK, GIO and gdk-pixbuf use the resources that come with the bundled GTK.
        prefix = self.root / "usr/lib/contrail"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        (prefix / "libexec/contrail/contrail").write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$GIO_MODULE_DIR\" \"$GSETTINGS_SCHEMA_DIR\" \"${XDG_DATA_DIRS%%:*}\" "
            "\"$(cat \"$GDK_PIXBUF_MODULE_FILE\")\"\n")
        for directory in ("lib/gio/modules", "lib/gdk-pixbuf-2.0"):
            (prefix / directory).mkdir(parents=True, exist_ok=True)
        loaders = prefix / "lib/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"
        loaders.write_text("#!/bin/sh\necho \"loaders with $LD_LIBRARY_PATH\"\n")
        loaders.chmod(0o755)
        self.env["XDG_RUNTIME_DIR"] = str(self.root / "runtime")
        out = self.launch(prefix / "bin/contrail")
        self.assertEqual(out, [str(prefix / "lib/gio/modules"), str(prefix / "share/glib-2.0/schemas"),
                               str(prefix / "share"), f"loaders with {prefix / 'lib'}"])

    def test_flatpak_uses_runtime_gtk_resources(self):
        prefix = self.root / "app"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        (prefix / "libexec/contrail/contrail").write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$GIO_MODULE_DIR\" \"$GSETTINGS_SCHEMA_DIR\" \"$GDK_PIXBUF_MODULE_FILE\"\n")
        self.env.update(GIO_MODULE_DIR="/usr/lib/gio/modules", GSETTINGS_SCHEMA_DIR="/usr/share/glib-2.0/schemas",
                        GDK_PIXBUF_MODULE_FILE="/usr/lib/gdk-pixbuf/loaders.cache")
        self.assertEqual(self.launch(prefix / "bin/contrail"),
                         [self.env[key] for key in ("GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "GDK_PIXBUF_MODULE_FILE")])

    def test_version_needs_no_display_or_data(self):
        prefix = self.root / "usr"
        self.install(prefix / "bin/contrail", prefix / "libexec/contrail/contrail")
        (prefix / "share/contrail").mkdir(parents=True)
        (prefix / "share/contrail/version").write_text("Contrail 0.1.0\n")
        env = {k: v for k, v in self.env.items() if k not in {"DISPLAY", "WAYLAND_DISPLAY"}}
        result = subprocess.run([str(prefix / "bin/contrail"), "--version"], env=env, check=True,
                                capture_output=True, text=True)
        self.assertEqual(result.stdout, "Contrail 0.1.0\n")
        self.assertFalse(self.data.exists())


if __name__ == "__main__":
    unittest.main()
