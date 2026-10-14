#!/usr/bin/env python3
"""Assembles the local release kit in out/release/<version>/ from packages built at a clean commit.
Never tags, pushes or publishes anything.

  python3 tools/prepare_release.py [--validation]

Before this: tools/build-packages.sh all (with your own API credentials), then the Debian sources:
  python3 tools/prepare_release.py --collect-sources
--validation accepts packages built with the public test API credentials and writes the kit to
out/release/<version>-validation/, marked as not for release: for checking the release tooling only.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

from collect_debian_sources import verify_dsc, fetch_file

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "out/packages"
SOURCES = ROOT / "out/corresponding-sources"
# FuriOS 14's mirror is Debian testing as of this snapshot; it serves no sources itself.
FURIOS_SNAPSHOT = "20260615T140756Z"
PART_SIZE = 64 * 1024 * 1024
TARGETS = {
    "arm64": dict(arch="aarch64", format="deb", container="localhost/tdesktop-cross", snapshot=FURIOS_SNAPSHOT),
    "amd64": dict(arch="x86_64", format="appimage", container="localhost/contrail-desktop", snapshot=None),
}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(*args, cwd=ROOT):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()


def workspace(deb_arch):
    target = TARGETS[deb_arch]
    return PACKAGES / f"{target['format']}-{deb_arch}"


def stage(deb_arch):
    return workspace(deb_arch) / ("Contrail.AppDir" if deb_arch == "amd64" else "root")


def inventories(deb_arch):
    doc = stage(deb_arch) / "usr/share/doc/contrail"
    found = [doc / "build-inputs.json"]
    if (doc / "bundled/bundled-packages.json").exists():
        found.append(doc / "bundled/bundled-packages.json")
    return found


def collect_sources():
    for deb_arch, target in TARGETS.items():
        command = ["python3", ROOT / "tools/collect_debian_sources.py", "--container", target["container"],
                   "--output", SOURCES / f"debian-{deb_arch}", *inventories(deb_arch)]
        if target["snapshot"]:
            command += ["--snapshot", target["snapshot"]]
        subprocess.run([str(c) for c in command], check=True)


def check_packages(version, commit, validation):
    infos = {}
    for deb_arch in TARGETS:
        info = json.loads((workspace(deb_arch) / "build-info.json").read_text())
        problems = []
        if info["version"] != version:
            problems.append(f"version {info['version']}")
        if info["project_commit"] != commit or info["project_dirty"]:
            problems.append(f"built from {info['project_commit'][:12]}{' with changes' if info['project_dirty'] else ''}")
        if info["api_credentials"] != "own" and not validation:
            problems.append("built with the public test API credentials")
        if problems:
            raise RuntimeError(f"Rebuild the {deb_arch} package from the clean release commit: {', '.join(problems)}")
        infos[deb_arch] = info
    cores = {(i["core_tag"], i["core_commit"], i["core_patches_sha256"]) for i in infos.values()}
    if len(cores) != 1:
        raise RuntimeError(f"The packages were built from different cores: {cores}")
    return infos


def core_archive(destination, core_commit):
    """Telegram Desktop at the pinned commit with every submodule, from git objects (without the patches)."""
    core = ROOT / "tdesktop"
    if git("rev-parse", "HEAD", cwd=core) != core_commit:
        raise RuntimeError("tdesktop/ is not at the core commit the packages were built from")
    entries = [("", core)]
    status = git("submodule", "status", "--recursive", cwd=core)
    for line in status.splitlines():
        path = line[1:].split()[1]
        entries.append((path, core / path))
    target = destination / "telegram-desktop.tar"
    with tarfile.open(target, "w", format=tarfile.PAX_FORMAT) as output:
        for prefix, directory in entries:
            stream = subprocess.run(["git", "archive", "--format=tar", "HEAD"], cwd=directory, check=True,
                                    capture_output=True).stdout
            with tarfile.open(fileobj=io.BytesIO(stream)) as archive:
                for member in archive:
                    if member.isdir() and prefix and member.name in ("", "."):
                        continue
                    data = archive.extractfile(member) if member.isfile() else None
                    member.name = "/".join(filter(None, ["tdesktop", prefix, member.name]))
                    if member.linkname and member.islnk():
                        member.linkname = "/".join(filter(None, ["tdesktop", prefix, member.linkname]))
                    output.addfile(member, data)
        revisions = (f"Telegram Desktop {git('describe', '--tags', cwd=core)} {core_commit}\n"
                     f"Submodules (path commit):\n{status}\n").encode()
        info = tarfile.TarInfo("tdesktop/CONTRAIL-REVISIONS.txt")
        info.size = len(revisions)
        output.addfile(info, io.BytesIO(revisions))
    subprocess.run(["xz", "-T0", "-6", "-f", target], check=True)
    return target.with_name(target.name + ".xz")


def project_archive(destination, version, commit, date):
    names = sorted(filter(None, git("ls-files", "-z").split("\0")))
    if any(".local." in name or name.startswith(("out/", "tdesktop/", "deps-aarch64/", "deps-x86_64/", "build-aarch64/", "build-x86_64/")) or
           name.endswith(".env") for name in names):
        raise RuntimeError("Private or generated files must not enter the source archive")
    manifest = {name: digest(ROOT / name) for name in names}
    archive_path = destination / f"Contrail-{version}-sources.tar.gz"
    prefix = f"contrail-{version}/"
    with tarfile.open(archive_path, "w:gz") as archive:
        for name in names:
            info = archive.gettarinfo(ROOT / name, arcname=prefix + name)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = date
            with (ROOT / name).open("rb") as stream:
                archive.addfile(info, stream)
        content = (json.dumps(dict(commit=commit, date=date, files=manifest), indent=2) + "\n").encode()
        member = tarfile.TarInfo(prefix + "SOURCE-REVISION.json")
        member.size, member.mode, member.mtime = len(content), 0o644, date
        archive.addfile(member, io.BytesIO(content))
    return archive_path


class PartWriter(io.RawIOBase):
    """A file written straight into numbered parts of PART_SIZE bytes (the whole file never exists on disk).
    Keeps the SHA-256 of the whole content."""

    def __init__(self, path):
        super().__init__()
        self.path, self.parts, self.hash = path, [], hashlib.sha256()
        self.stream, self.left, self.position = None, 0, 0
        for stale in path.parent.glob(path.name + ".part*"):
            stale.unlink()

    def writable(self):
        return True

    def tell(self):
        return self.position

    def write(self, data):
        data = memoryview(data)
        self.hash.update(data)
        written = 0
        while written < len(data):
            if self.left == 0:
                if self.stream:
                    self.stream.close()
                part = self.path.with_name(self.path.name + f".part{len(self.parts) + 1:02}")
                self.parts.append(part)
                self.stream, self.left = part.open("wb"), PART_SIZE
            chunk = data[written:written + self.left]
            self.stream.write(chunk)
            self.left -= len(chunk)
            written += len(chunk)
        self.position += len(data)
        return len(data)

    def close(self):
        if self.stream:
            self.stream.close()
            self.stream = None
        super().close()


def split(path):
    """Splits an existing file into parts (see PartWriter) and removes it."""
    writer = PartWriter(path)
    with path.open("rb") as stream:
        shutil.copyfileobj(stream, writer, 1024 * 1024)
    writer.close()
    path.unlink()
    return writer.parts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--collect-sources", action="store_true")
    parser.add_argument("--validation", action="store_true")
    args = parser.parse_args()
    if args.collect_sources:
        collect_sources()
        return

    version = (ROOT / "VERSION").read_text().strip()
    commit = git("rev-parse", "HEAD")
    if git("status", "--porcelain"):
        raise RuntimeError("Commit the reviewed changes before preparing release assets")
    infos = check_packages(version, commit, args.validation)
    info = infos["arm64"]
    date = int(info["source_date"])
    destination = ROOT / "out/release" / (version + ("-validation" if args.validation else ""))
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    # Corresponding sources: everything except Contrail's own repository (that is the separate archive).
    bundle = ROOT / "out/release-work" / version
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    files = {}
    files[core_archive(bundle, info["core_commit"])] = "telegram-desktop.tar.xz"
    for deb_arch, target in TARGETS.items():
        deps = ROOT / f"deps-{target['arch']}/sources"
        manifest = json.loads((deps / "manifest.json").read_text())
        for name, record in manifest.items():
            path = deps / record["archive"]
            if digest(path) != record["sha256"]:
                raise RuntimeError(f"Changed dependency source: {path}")
            files[path] = f"libraries-{target['arch']}/{record['archive']}"
        for extra in ("manifest.json", "rust-crates.txt", "rustc-version.txt"):
            if (deps / extra).exists():
                files[deps / extra] = f"libraries-{target['arch']}/{extra}"
        debian = SOURCES / f"debian-{deb_arch}"
        metadata = json.loads((debian / "sources.json").read_text())
        if metadata["errors"]:
            raise RuntimeError(f"Incomplete Debian sources: {debian}")
        from collect_debian_sources import needed_sources
        wanted = set()
        for inventory in inventories(deb_arch):
            wanted |= needed_sources(json.loads(inventory.read_text()))
        available = {(s["package"], s["version"]) for s in metadata["sources"]}
        if missing := wanted - available:
            raise RuntimeError(f"Debian sources missing for {deb_arch}: {sorted(missing)}")
        for source in metadata["sources"]:
            for record in source["files"]:
                path = debian / record["name"]
                if not path.is_file() or path.stat().st_size != record["size"] or digest(path) != record["sha256"]:
                    raise RuntimeError(f"Missing or changed Debian source file: {path}")
                files[path] = f"debian-{deb_arch}/{record['name']}"
            verify_dsc(source, debian)
        files[debian / "sources.json"] = f"debian-{deb_arch}/sources.json"
    runtime = json.loads((ROOT / "packaging/runtime-sources.json").read_text())
    tools = json.loads((ROOT / "packaging/appimage-tools.json").read_text())
    if runtime["runtime_commit"] != tools["runtime-x86_64"]["source_commit"]:
        raise RuntimeError("The AppImage runtime binary and source pins disagree")
    runtime_dir = SOURCES / "appimage-runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    unavailable = []
    for record in runtime["files"]:
        try:
            fetch_file(record, runtime_dir)
        except (RuntimeError, OSError):
            # A check of the tooling may run where some hosts are blocked; a release may not.
            if not args.validation:
                raise
            unavailable.append(record["url"])
            continue
        files[runtime_dir / record["name"]] = f"appimage-runtime/{record['name']}"
    files[ROOT / "packaging/runtime-sources.json"] = "appimage-runtime/runtime-sources.json"
    files[ROOT / "packaging/LICENSES.md"] = "README.md"

    sources_tar = destination / f"Contrail-{version}-corresponding-sources.tar"
    writer = PartWriter(sources_tar)
    with tarfile.open(fileobj=writer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for path, name in sorted(files.items(), key=lambda item: item[1]):
            member = archive.gettarinfo(path, arcname=f"contrail-{version}-corresponding-sources/{name}")
            member.uid = member.gid = 0
            member.uname = member.gname = ""
            member.mtime = date
            with path.open("rb") as stream:
                archive.addfile(member, stream)
    writer.close()
    whole_sum, parts = writer.hash.hexdigest(), writer.parts
    (destination / "SOURCES.md").write_text(
        "# Corresponding sources\n\n"
        "These files are not needed to install Contrail. They hold the complete sources of what the packages "
        "contain besides Contrail's own code (which is in "
        f"`Contrail-{version}-sources.tar.gz`): Telegram Desktop with all its submodules, the libraries built "
        "for the packages, the Debian source packages of the Debian parts, and the AppImage runtime.\n\n"
        f"Download all {len(parts)} files `{sources_tar.name}.part*` and `SHA256SUMS` into one folder, then:\n\n"
        "```sh\nsha256sum --check --ignore-missing SHA256SUMS\n"
        f"cat {sources_tar.name}.part* > {sources_tar.name}\n"
        f"echo '{whole_sum}  {sources_tar.name}' | sha256sum --check\n"
        f"tar -xf {sources_tar.name}\n```\n\n"
        "Inside, `README.md` explains what each part is. A Debian source package unpacks with "
        "`dpkg-source -x <package>_<version>.dsc`.\n")

    assets = [*parts, destination / "SOURCES.md", project_archive(destination, version, commit, date)]
    for deb_arch in TARGETS:
        name = json.loads((workspace(deb_arch) / "build-info.json").read_text())["file"]
        shutil.copy2(PACKAGES / name, destination / name)
        assets.append(destination / name)
        shutil.copy2(workspace(deb_arch) / "build-environment.txt", destination / f"build-environment-{deb_arch}.txt")
        shutil.copy2(stage(deb_arch) / "usr/share/doc/contrail/build-inputs.json",
                     destination / f"build-inputs-{deb_arch}.json")
        assets += [destination / f"build-environment-{deb_arch}.txt", destination / f"build-inputs-{deb_arch}.json"]
    shutil.copy2(stage("amd64") / "usr/share/doc/contrail/bundled/bundled-packages.json",
                 destination / "bundled-packages-amd64.json")
    assets.append(destination / "bundled-packages-amd64.json")
    for source, target in (("packaging/INSTALL.md", "README.md"), ("packaging/RELEASE-NOTES.md", "RELEASE-NOTES.md"),
                           ("packaging/LICENSES.md", "LICENSES.md")):
        shutil.copy2(ROOT / source, destination / target)
        assets.append(destination / target)
    if args.validation:
        warning = destination / "NOT-FOR-RELEASE.txt"
        warning.write_text("Built with Telegram's public test API credentials to check the release tooling. "
                           "Do not publish these files.\n"
                           + "".join(f"Not downloaded here (the release run fetches it): {url}\n"
                                     for url in unavailable))
        assets.append(warning)
    (destination / "SHA256SUMS").write_text("".join(f"{digest(p)}  {p.name}\n" for p in sorted(assets)))
    shutil.rmtree(bundle)

    unexpected = {p.name for p in destination.iterdir()} - {p.name for p in assets} - {"SHA256SUMS"}
    if unexpected:
        raise RuntimeError(f"Files outside the asset list: {sorted(unexpected)}")
    print(f"Prepared {destination}")
    print(f"Commit {commit}; Telegram Desktop {info['core_tag']} {info['core_commit']}")
    for path in sorted(destination.iterdir()):
        print(f"  {path.stat().st_size / 2**20:9.1f} MiB  {path.name}")
    print("Nothing was tagged, pushed or published.")


if __name__ == "__main__":
    main()
