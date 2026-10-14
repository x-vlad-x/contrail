"""contrail-migrate-launcher: only the launcher the SSH install generated is moved away, nothing else."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP_ID = "io.github.x_vlad_x.Contrail"


def legacy_entry(home):
    """The user launcher update-contrail.sh wrote before the packages existed (its template, @HOME@ expanded)."""
    template = subprocess.run(["git", "show", f"ad4dbf8:packaging/{APP_ID}.desktop"], cwd=ROOT,
                              capture_output=True, text=True)
    text = template.stdout if template.returncode == 0 else (
        "[Desktop Entry]\nType=Application\nName=Contrail\n"
        f"Icon=@HOME@/.local/share/contrail/{APP_ID}.svg\nExec=@HOME@/.local/share/contrail/contrail -- %u\n")
    return text.replace("@HOME@", str(home))


def current_entry(home):
    """The launcher the current update-contrail.sh generates from the packaged desktop entry."""
    text = (ROOT / f"packaging/{APP_ID}.desktop").read_text()
    text = text.replace("Exec=contrail ", f"Exec={home}/.local/share/contrail/contrail ")
    return "".join(f"Icon={home}/.local/share/contrail/{APP_ID}.svg\n" if line.startswith("Icon=") else line
                   for line in text.splitlines(keepends=True))


class DesktopMigrationTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="contrail desktop ")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.home = self.root / "home"
        self.data = self.home / ".local/share"
        self.state = self.home / ".local/state"
        self.prefix = self.root / "usr"
        self.entry = self.data / f"applications/{APP_ID}.desktop"
        self.entry.parent.mkdir(parents=True)
        (self.prefix / "bin").mkdir(parents=True)
        (self.prefix / "share/applications").mkdir(parents=True)
        self.system_entry = self.prefix / f"share/applications/{APP_ID}.desktop"
        shutil.copy2(ROOT / f"packaging/{APP_ID}.desktop", self.system_entry)
        shutil.copy2(ROOT / "packaging/contrail", self.prefix / "bin/contrail")
        self.helper = self.prefix / "bin/contrail-migrate-launcher"
        shutil.copy2(ROOT / "packaging/contrail-migrate-launcher", self.helper)
        self.env = {k: v for k, v in os.environ.items()
                    if k not in {"XDG_DATA_HOME", "XDG_STATE_HOME", "SUDO_USER", "PKEXEC_UID"}}
        self.env["HOME"] = str(self.home)

    def migrate(self, **env):
        return subprocess.run([str(self.helper)], env={**self.env, **env}, capture_output=True, text=True)

    def backups(self):
        return list(self.state.glob(f"contrail/launcher-backups/*/{APP_ID}.desktop"))

    def test_moves_generated_launchers_and_keeps_everything_else(self):
        for text in (legacy_entry(self.home), current_entry(self.home)):
            with self.subTest(text=text[:40]):
                self.entry.write_text(text)
                legacy = self.data / "contrail"
                (legacy / "bin").mkdir(parents=True, exist_ok=True)
                (legacy / "bin/contrail").write_text("old program")
                account = self.data / "contrail-data/tdata/key_datas"
                account.parent.mkdir(parents=True, exist_ok=True)
                account.write_text("account")
                result = self.migrate()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(self.entry.exists())
                self.assertIn(text, [b.read_text() for b in self.backups()])
                self.assertEqual((legacy / "bin/contrail").read_text(), "old program")
                self.assertEqual(account.read_text(), "account")
        # Running it again changes nothing.
        count = len(self.backups())
        self.assertEqual(self.migrate().returncode, 0)
        self.assertEqual(len(self.backups()), count)

    def test_nothing_to_migrate(self):
        result = self.migrate()
        self.assertEqual(result.returncode, 0)
        self.assertFalse(self.state.exists())

    def test_custom_entries_are_left_alone(self):
        original = legacy_entry(self.home)
        for text in (original.replace(" -- %u", " --custom -- %u"), original + "Hidden=true\n",
                     original + "NoDisplay=true\n", original.replace("Icon=", "Icon=/elsewhere")):
            with self.subTest(text=text[-30:]):
                self.entry.write_text(text)
                self.assertNotEqual(self.migrate().returncode, 0)
                self.assertEqual(self.entry.read_text(), text)
                self.assertEqual(self.backups(), [])

    def test_symlink_is_left_alone(self):
        target = self.home / "custom.desktop"
        target.write_text(legacy_entry(self.home))
        self.entry.symlink_to(target)
        self.assertNotEqual(self.migrate().returncode, 0)
        self.assertTrue(self.entry.is_symlink())

    def test_needs_the_package(self):
        self.entry.write_text(legacy_entry(self.home))
        self.system_entry.unlink()
        self.assertNotEqual(self.migrate().returncode, 0)
        self.assertTrue(self.entry.exists())

    def test_refuses_sudo(self):
        self.entry.write_text(legacy_entry(self.home))
        self.assertNotEqual(self.migrate(SUDO_USER="someone").returncode, 0)
        self.assertTrue(self.entry.exists())

    def test_honours_xdg_data_home(self):
        custom = self.root / "custom data"
        entry = custom / f"applications/{APP_ID}.desktop"
        entry.parent.mkdir(parents=True)
        entry.write_text(legacy_entry(self.home))
        self.assertEqual(self.migrate(XDG_DATA_HOME=str(custom)).returncode, 0)
        self.assertFalse(entry.exists())


if __name__ == "__main__":
    unittest.main()
