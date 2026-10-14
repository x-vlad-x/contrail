"""Release metadata and tooling: consistent versions and names, source classification, archive splitting."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import collect_debian_sources  # noqa: E402
import package  # noqa: E402
import prepare_release  # noqa: E402

APP_ID = "io.github.x_vlad_x.Contrail"
VERSION = (ROOT / "VERSION").read_text().strip()


class MetadataTest(unittest.TestCase):
    def test_version_format_and_debian_ordering(self):
        self.assertRegex(VERSION, r"^\d+\.\d+\.\d+(-(preview|rc)\d+)?$")
        self.assertEqual(package.deb_version("0.1.0-preview1"), "0.1.0~preview1-1")
        self.assertEqual(package.deb_version("0.1.0"), "0.1.0-1")
        # "~" sorts before anything: a preview is older than the final release.
        result = subprocess.run(["dpkg", "--compare-versions", "0.1.0~preview1-1", "lt", "0.1.0-1"])
        if result.returncode != 127:
            self.assertEqual(result.returncode, 0)

    def test_metainfo_names_this_release(self):
        root = ET.parse(ROOT / f"packaging/{APP_ID}.metainfo.xml").getroot()
        self.assertEqual(root.findtext("id"), APP_ID)
        releases = [r.get("version") for r in root.iter("release")]
        self.assertEqual(releases[0], package.deb_version(VERSION).removesuffix("-1"))
        self.assertIn("Telegram API", "".join(root.find("description").itertext()))

    def test_desktop_entry_is_relocatable(self):
        entry = (ROOT / f"packaging/{APP_ID}.desktop").read_text()
        self.assertNotIn("@HOME@", entry)
        self.assertIn("\nExec=contrail -- %u\n", entry)
        self.assertIn(f"\nIcon={APP_ID}\n", entry)
        self.assertIn(f"\nStartupWMClass={APP_ID}\n", entry)
        name = re.search(r"^Name=(.*)$", entry, re.M).group(1)
        self.assertNotIn("Telegram", name)  # Telegram API Terms of Service, 2.3

    def test_documents_name_the_real_files(self):
        deb = f"contrail_{package.deb_version(VERSION).replace('~', '-')}_arm64.deb"
        appimage = f"Contrail-{VERSION}-x86_64.AppImage"
        guide = (ROOT / "packaging/INSTALL.md").read_text()
        notes = (ROOT / "packaging/RELEASE-NOTES.md").read_text()
        for text in (guide, notes):
            self.assertIn(deb, text)
            self.assertIn(appimage, text)
            # GitHub rewrites "~" in asset names: the Debian version's "~" stays inside the package only.
            self.assertNotIn("~preview", text)

    def test_no_private_files_tracked(self):
        names = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True).stdout.split()
        if not names:
            self.skipTest("not a git checkout")
        for name in names:
            self.assertNotIn(".local.", name)
            self.assertFalse(name.endswith(".env"), name)
            self.assertFalse(name.startswith(("out/", "tdesktop/", "deps-aarch64/", "deps-x86_64/", "build-aarch64/", "build-x86_64/")), name)


class SourceClassificationTest(unittest.TestCase):
    def package(self, binary, source):
        return dict(binary_package=binary, binary_version="1", source_package=source, source_version="1")

    def test_build_inputs(self):
        inventory = dict(
            packages=[self.package("qt6-base-dev:arm64", "qtbase"), self.package("libqt6core6:arm64", "qtbase"),
                      self.package("librange-v3-dev", "range-v3"), self.package("linux-libc-dev:arm64", "linux"),
                      self.package("libgcc-15-dev:arm64", "gcc-15"), self.package("libc6:arm64", "glibc")],
            headers={"qt6-base-dev:arm64": 10, "librange-v3-dev": 5, "linux-libc-dev:arm64": 3},
            static=[dict(file="/usr/lib/gcc/libgcc.a", packages=["libgcc-15-dev:arm64"]),
                    dict(file="/src/deps/libtg_owt.a", origin="built")],
            dynamic={"libQt6Core.so.6": ["libqt6core6:arm64"], "libc.so.6": ["libc6:arm64"]})
        self.assertEqual(collect_debian_sources.needed_sources(inventory), {("range-v3", "1"), ("gcc-15", "1")})

    def test_bundled_packages(self):
        bundled = [dict(self.package("libgtk-4-1", "gtk4"), files=["usr/lib/libgtk-4.so.1"])]
        self.assertEqual(collect_debian_sources.needed_sources(bundled), {("gtk4", "1")})

    def test_dsc_must_match_its_files(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            orig = directory / "pkg_1.0.orig.tar.xz"
            orig.write_bytes(b"source")
            digest = hashlib.sha256(b"source").hexdigest()
            (directory / "pkg_1.0-1.dsc").write_text(
                f"Format: 3.0 (quilt)\nSource: pkg\nVersion: 1.0-1\nChecksums-Sha256:\n {digest} 6 {orig.name}\n")
            record = lambda path: dict(name=path.name, size=path.stat().st_size,
                                       sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            source = dict(package="pkg", version="1.0-1",
                          files=[record(directory / "pkg_1.0-1.dsc"), record(orig)])
            collect_debian_sources.verify_dsc(source, directory)
            with self.assertRaises(ValueError):
                collect_debian_sources.verify_dsc(dict(source, version="1.0-2"), directory)
            with self.assertRaises(ValueError):
                collect_debian_sources.verify_dsc(dict(source, files=source["files"][:1]), directory)


class SplitTest(unittest.TestCase):
    def test_parts_join_back_and_stale_parts_go(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "sources.tar"
            content = bytes(range(256)) * 1000
            archive.write_bytes(content)
            (Path(directory) / "sources.tar.part09").write_bytes(b"stale")
            original = prepare_release.PART_SIZE
            prepare_release.PART_SIZE = 100_000
            try:
                parts = prepare_release.split(archive)
            finally:
                prepare_release.PART_SIZE = original
            self.assertEqual([p.name for p in parts], [f"sources.tar.part{i:02}" for i in range(1, 4)])
            self.assertEqual(b"".join(p.read_bytes() for p in parts), content)
            self.assertFalse((Path(directory) / "sources.tar.part09").exists())


class AppImageToolsTest(unittest.TestCase):
    def test_tools_are_pinned(self):
        tools = json.loads((ROOT / "packaging/appimage-tools.json").read_text())
        runtime = json.loads((ROOT / "packaging/runtime-sources.json").read_text())
        for name, tool in tools.items():
            self.assertRegex(tool["sha256"], r"^[0-9a-f]{64}$", name)
            self.assertTrue(tool["url"].startswith("https://"), name)
        self.assertEqual(tools["runtime-x86_64"]["source_commit"], runtime["runtime_commit"])


if __name__ == "__main__":
    unittest.main()
