#!/usr/bin/env python3
"""Turn a finished build into a package. Called by tools/build-packages.sh; never installs anything.

  python3 tools/package.py deb --arch aarch64 --container IMAGE --api own|test --core-tag T --core-commit C
  python3 tools/package.py appimage --arch x86_64 ...

Commands that need the build environment (cmake --install, strip, dpkg-shlibdeps, the AppImage tools) run in
the build container, so the result doesn't depend on the host.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
APP_ID = "io.github.x_vlad_x.Contrail"
NAME = "contrail"
MAINTAINER = "Vladislav Shadiuk <6992531+x-vlad-x@users.noreply.github.com>"
HOMEPAGE = "https://github.com/x-vlad-x/contrail"
ARCHES = {
    "aarch64": dict(deb="arm64", triple="aarch64-linux-gnu"),
    "x86_64": dict(deb="amd64", triple="x86_64-linux-gnu"),
}
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
LICENSE_RE = re.compile(r"(^|/)(LICEN[CS]E|COPYING|NOTICE|PATENTS|AUTHORS|UNLICENSE)[^/]*$", re.I)
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".rs", ".py", ".go", ".js", ".m", ".mm", ".S", ".asm")


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, text=True, **kwargs)


def engine():
    return os.environ.get("CONTAINER_ENGINE") or shutil.which("podman") or shutil.which("docker")


def container_path(path):
    return Path("/src") / Path(path).resolve().relative_to(ROOT)


def execute(args, command, *, cwd=ROOT, capture=False, env=None, network=False):
    """Runs a command in the build container (paths under ROOT are /src there)."""
    tool = engine()
    user = (["--userns=keep-id"] if Path(tool).name == "podman"
            else ["--user", f"{os.getuid()}:{os.getgid()}"])
    options = ["run", "--rm", *user, "-e", "HOME=/tmp", "-v", f"{ROOT}:/src:z", "-w", container_path(cwd)]
    if not network:
        options.append("--network=none")
    for key, value in (env or {}).items():
        options += ["-e", f"{key}={value}"]
    return run([tool, *options, args.container, *command], capture_output=capture)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def project_state():
    """The source commit and whether the tree differs from it (release source archives carry a manifest)."""
    result = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=ROOT, text=True, capture_output=True)
    if result.returncode == 0 and Path(result.stdout.strip()).resolve() == ROOT:
        commit = run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True).stdout.strip()
        dirty = bool(run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True).stdout)
        return commit, dirty
    metadata = json.loads((ROOT / "SOURCE-REVISION.json").read_text())
    for name, expected in metadata["files"].items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT):
            raise RuntimeError(f"Unsafe source manifest entry: {name}")
        if not path.is_file() or digest(path) != expected:
            return metadata["commit"], True
    return metadata["commit"], False


def core_patch_digest():
    hasher = hashlib.sha256()
    for patch in sorted((ROOT / "patches").glob("*.patch")):
        hasher.update(patch.name.encode() + b"\0" + patch.read_bytes())
    return hasher.hexdigest()


def stage_paths(args):
    deb_arch = ARCHES[args.arch]["deb"]
    workspace = ROOT / "out/packages" / f"{args.format}-{deb_arch}"
    stage = workspace / ("Contrail.AppDir" if args.format == "appimage" else "root")
    return workspace, stage


def extract_licenses(archive, destination, wanted=None):
    """Copies the license files of a source archive; `wanted` limits it to some top directories."""
    count = 0
    with tarfile.open(archive) as source:
        for member in source:
            name = member.name
            if not member.isfile() or not LICENSE_RE.search(name) or name.endswith(SOURCE_SUFFIXES):
                continue
            parts = Path(name).parts
            if any(part in {".git", "test", "tests", "testdata", "examples"} for part in parts):
                continue
            if wanted is not None and not any(name.startswith(prefix) for prefix in wanted):
                continue
            target = destination / name
            if not target.resolve().is_relative_to(destination.resolve()):
                raise RuntimeError(f"Unsafe path in {archive}: {name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as stream:
                target.write_bytes(stream.read())
            count += 1
    return count


def static_notices(args, doc):
    """License texts of the libraries built by build-deps.sh and linked statically into the program."""
    sources = ROOT / f"deps-{args.arch}/sources"
    manifest = json.loads((sources / "manifest.json").read_text())
    target = doc / "static-libraries"
    target.mkdir(parents=True, exist_ok=True)
    for name, record in sorted(manifest.items()):
        archive = sources / record["archive"]
        if digest(archive) != record["sha256"]:
            raise RuntimeError(f"Source archive changed after the build: {archive}")
        if name == "tdesktop_rust":
            # Only the crates that are linked in, not build tools or other platforms' crates.
            crates = (sources / "rust-crates.txt").read_text().splitlines()
            wanted = ["rust/tlottie/", "rust/wallet-engine/"]
            for line in crates:
                crate, version = line.split()[:2]
                wanted.append(f"rust/tdesktop_rust/vendor/{crate}-{version.removeprefix('v')}/")
            found = extract_licenses(archive, target / name, wanted)
            shutil.copy2(sources / "rust-crates.txt", target / name / "crates.txt")
        else:
            found = extract_licenses(archive, target / name)
        if not found:
            raise RuntimeError(f"No license files found for the statically linked {name}")
    shutil.copy2(sources / "manifest.json", target / "sources.json")


def prepare_stage(args):
    workspace, stage = stage_paths(args)
    build = ROOT / f"build-{args.arch}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    triple = ARCHES[args.arch]["triple"]
    execute(args, ["env", f"DESTDIR={container_path(stage)}", "cmake", "--install", container_path(build),
                   "--component", "Contrail", "--prefix", "/usr"])
    program = stage / "usr/libexec/contrail/contrail"
    doc = stage / "usr/share/doc/contrail"
    # The Debian packages the build used: headers, static parts, shared libraries (see LICENSES.md).
    execute(args, ["python3", container_path(ROOT / "tools/build_inputs.py"), container_path(build),
                   container_path(program), container_path(doc / "build-inputs.json")])
    execute(args, [f"{triple}-strip", "--remove-section=.comment", "--remove-section=.note",
                   container_path(program)])
    icons = stage / "usr/share/icons/hicolor"
    svg = icons / f"scalable/apps/{APP_ID}.svg"
    for size in ICON_SIZES:
        png = icons / f"{size}x{size}/apps/{APP_ID}.png"
        png.parent.mkdir(parents=True, exist_ok=True)
        execute(args, ["rsvg-convert", "-w", size, "-h", size, "-o", container_path(png), container_path(svg)])
    execute(args, ["desktop-file-validate", container_path(stage / f"usr/share/applications/{APP_ID}.desktop")])
    execute(args, ["appstreamcli", "validate", "--no-net", "--explain",
                   container_path(stage / f"usr/share/metainfo/{APP_ID}.metainfo.xml")])

    version = (ROOT / "VERSION").read_text().strip()
    commit, dirty = project_state()
    source_date = int(run(["git", "log", "-1", "--format=%ct"], cwd=ROOT, capture_output=True).stdout.strip()
                      if (ROOT / ".git").exists() else json.loads((ROOT / "SOURCE-REVISION.json").read_text())["date"])
    static_notices(args, doc)
    image = run([engine(), "image", "inspect", "--format", "{{.Id}}", args.container],
                capture_output=True).stdout.strip()
    info = {
        "version": version,
        "architecture": ARCHES[args.arch]["deb"],
        "format": args.format,
        "project_commit": commit,
        "project_dirty": dirty,
        "source_date": source_date,
        "core_tag": args.core_tag,
        "core_commit": args.core_commit,
        "core_patches_sha256": core_patch_digest(),
        "api_credentials": args.api,
        "container": args.container,
        "container_image": image,
        "status": "for checking the packaging only, not for distribution" if args.api == "test" else "preview",
    }
    (doc / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    resources = stage / "usr/share/contrail"
    lines = [f"Contrail {version}",
             f"Telegram Desktop core {args.core_tag.removeprefix('v')} ({args.core_commit})",
             f"Source commit {commit}{' with local changes' if dirty else ''}"]
    if args.api == "test":
        lines.append("Built with Telegram's public test API credentials: not for distribution")
    (resources / "version").write_text("\n".join(lines) + "\n")
    # The packages installed in the build container: the exact build environment.
    listing = execute(args, ["dpkg-query", "-W", "-f=${binary:Package}\\t${Version}\\n"], capture=True).stdout
    (workspace / "build-environment.txt").write_text(
        f"# {args.container} {image}\n" + "".join(sorted(listing.splitlines(keepends=True))))
    return workspace, stage, version, info


# --- Debian package -----------------------------------------------------------------------------------------

def deb_version(version):
    # 0.1.0-preview1 -> 0.1.0~preview1-1: a preview sorts before the final 0.1.0.
    return re.sub(r"-(preview|rc|beta|alpha)", r"~\1", version) + "-1"


def write_copyright(doc):
    upstream_licenses = sorted(p.relative_to(doc).as_posix() for p in (doc / "upstream").rglob("*") if p.is_file())
    text = f"""Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Contrail
Upstream-Contact: {HOMEPAGE}/issues
Source: {HOMEPAGE}
Comment: Contrail is an unofficial Telegram client. It is not affiliated with or endorsed by Telegram.
 Telegram is a trademark of Telegram FZ-LLC. The program is Telegram Desktop
 (https://github.com/telegramdesktop/tdesktop) with Contrail's GTK interface compiled in.
 The license texts of the libraries compiled into the program are in
 /usr/share/doc/contrail/upstream (Telegram Desktop's source tree) and
 /usr/share/doc/contrail/static-libraries (tg_owt, TDLib's tde2e, rnnoise, the Rust crates and others),
 see /usr/share/doc/contrail/LICENSES.md. Corresponding sources are published with each release.

Files: *
Copyright: 2026 Vladislav Shadiuk
           2014-2026 The Telegram Desktop Authors
License: GPL-3 with OpenSSL exception
 This program is free software: you can redistribute it and/or modify it under the terms of the GNU
 General Public License, version 3, with the following special exception: the copyright holders give
 permission to link the code of portions of this program with the OpenSSL library. You must obey the GNU
 General Public License in all respects for all of the code used other than OpenSSL.
 .
 On Debian systems, the full text of the GNU General Public License version 3 can be found in
 /usr/share/common-licenses/GPL-3; it is also in /usr/share/doc/contrail/LICENSE.
"""
    if not upstream_licenses:
        raise RuntimeError("Telegram Desktop's license notices are missing from the package")
    (doc / "copyright").write_text(text)


def build_deb(args, workspace, stage, version, info):
    deb_arch = ARCHES[args.arch]["deb"]
    package_version = deb_version(version)
    doc = stage / "usr/share/doc/contrail"
    write_copyright(doc)
    date = time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime(info["source_date"]))
    changelog = (f"contrail ({package_version}) unstable; urgency=medium\n\n"
                 f"  * Preview package. See {HOMEPAGE}/releases.\n\n -- {MAINTAINER}  {date}\n")
    with gzip.GzipFile(doc / "changelog.Debian.gz", "wb", mtime=0) as stream:
        stream.write(changelog.encode())

    # Dependencies of the program, from the shlibs/symbols files of the packages in the build container.
    debian = workspace / "debian"
    debian.mkdir(exist_ok=True)
    (debian / "control").write_text(f"Source: contrail\nMaintainer: {MAINTAINER}\n\n"
                                     f"Package: contrail\nArchitecture: {deb_arch}\nDescription: Contrail\n")
    program = container_path(stage / "usr/libexec/contrail/contrail")
    env = {"DEB_HOST_ARCH": deb_arch, "DEB_HOST_MULTIARCH": ARCHES[args.arch]["triple"]}
    output = execute(args, ["dpkg-shlibdeps", "-O", "--warnings=7", f"-e{program}"],
                     cwd=workspace, capture=True, env=env).stdout
    depends = next(line.split("=", 1)[1] for line in output.splitlines() if line.startswith("shlibs:Depends="))
    # Loaded at run time, not linked: Qt's WebP plugin (Telegram's emoji). The offscreen platform plugin and
    # the TLS backend come with libqt6gui6 and libqt6network6, which the program links.
    depends += ", qt6-image-formats-plugins, hicolor-icon-theme"

    installed_size = (sum(p.stat().st_size for p in stage.rglob("*") if p.is_file()) + 1023) // 1024
    control = stage / "DEBIAN"
    control.mkdir()
    (control / "control").write_text(
        f"Package: contrail\nVersion: {package_version}\nArchitecture: {deb_arch}\n"
        f"Maintainer: {MAINTAINER}\nSection: net\nPriority: optional\n"
        f"Installed-Size: {installed_size}\nDepends: {depends}\n"
        "Recommends: xdg-desktop-portal, xdg-utils, fonts-noto-color-emoji\n"
        f"Homepage: {HOMEPAGE}\n"
        "Description: unofficial Telegram client with a GTK interface for Linux phones\n"
        " Contrail is an unofficial client that uses the Telegram API. It is built on\n"
        " the Telegram Desktop core and replaces its interface with an adaptive GTK4\n"
        " and libadwaita one, made for Linux phones (Phosh) and usable on desktops.\n"
        " .\n"
        " Contrail is not affiliated with or endorsed by Telegram. This is a preview.\n")
    sums = []
    for path in sorted(stage.rglob("*")):
        if path.is_file() and not path.is_relative_to(control):
            sums.append(f"{hashlib.md5(path.read_bytes()).hexdigest()}  {path.relative_to(stage).as_posix()}\n")
    (control / "md5sums").write_text("".join(sums))
    for path in stage.rglob("*"):
        if path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            path.chmod(0o755 if os.access(path, os.X_OK) else 0o644)

    # Keep Debian's "~" in the package version, but not in the file name (GitHub rewrites it).
    output = ROOT / "out/packages" / f"contrail_{package_version.replace('~', '-')}_{deb_arch}.deb"
    execute(args, ["dpkg-deb", "--root-owner-group", "-Zxz", "--build", container_path(stage), container_path(output)])
    print(execute(args, ["dpkg-deb", "--info", container_path(output)], capture=True).stdout)
    return output


# --- AppImage ------------------------------------------------------------------------------------------------

def download_tools():
    directory = ROOT / "out/tools"
    directory.mkdir(parents=True, exist_ok=True)
    tools = json.loads((ROOT / "packaging/appimage-tools.json").read_text())
    for name, source in tools.items():
        path = directory / name
        if not path.exists() or digest(path) != source["sha256"]:
            temporary = path.with_name(path.name + ".download")
            urllib.request.urlretrieve(source["url"], temporary)
            temporary.replace(path)
        if digest(path) != source["sha256"]:
            raise RuntimeError(f"Checksum mismatch for {path}; review the pinned tool before updating it")
        path.chmod(0o755)
    return directory


def build_appimage(args, workspace, stage, version, info):
    if args.arch != "x86_64":
        raise RuntimeError("The AppImage recipe targets x86_64 only")
    tools = download_tools()
    doc = stage / "usr/share/doc/contrail"
    shutil.copytree(ROOT / "packaging/licenses/appimage-runtime", doc / "appimage-runtime", dirs_exist_ok=True)
    shutil.copy2(ROOT / "packaging/runtime-sources.json", doc / "appimage-runtime/runtime-sources.json")
    # Resources loaded at run time (Qt and GIO plugins, loaders, schemas, icons), from the build container.
    execute(args, ["python3", container_path(ROOT / "tools/prepare_appimage.py"), "resources", container_path(stage)])
    desktop = stage / f"usr/share/applications/{APP_ID}.desktop"
    plugins = sorted(p for p in (stage / "usr/lib/qt6/plugins").rglob("*.so"))
    plugins += sorted(p for p in (stage / "usr/lib/gio/modules").glob("*.so"))
    plugins += sorted(p for p in (stage / "usr/lib/gdk-pixbuf-2.0").rglob("*.so"))
    command = [container_path(tools / "linuxdeploy-x86_64.AppImage"), "--appimage-extract-and-run",
               "--appdir", container_path(stage),
               # The real program, not the shell launcher: linuxdeploy follows ELF dependencies only.
               "--deploy-deps-only", container_path(stage / "usr/libexec/contrail/contrail"),
               "--desktop-file", container_path(desktop),
               "--icon-file", container_path(stage / f"usr/share/icons/hicolor/256x256/apps/{APP_ID}.png"),
               "--custom-apprun", container_path(ROOT / "packaging/AppRun")]
    for plugin in plugins:
        command += ["--deploy-deps-only", container_path(plugin)]
    execute(args, command)
    # Everything the program and the plugins load must be resolvable inside the AppImage (except the base
    # system libraries linuxdeploy leaves to the host on purpose).
    execute(args, ["python3", container_path(ROOT / "tools/prepare_appimage.py"), "check", container_path(stage)])
    execute(args, ["python3", container_path(ROOT / "tools/prepare_appimage.py"), "licenses", container_path(stage)])
    output = ROOT / "out/packages" / f"Contrail-{version}-x86_64.AppImage"
    execute(args, ["env", "ARCH=x86_64", "VERSION=" + version, container_path(tools / "appimagetool-x86_64.AppImage"),
                   "--appimage-extract-and-run", "--no-appstream",
                   "--runtime-file", container_path(tools / "runtime-x86_64"),
                   container_path(stage), container_path(output)])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("format", choices=["deb", "appimage"])
    parser.add_argument("--arch", choices=sorted(ARCHES), required=True)
    parser.add_argument("--container", required=True, help="the image the build ran in")
    parser.add_argument("--api", choices=["own", "test"], required=True)
    parser.add_argument("--core-tag", required=True)
    parser.add_argument("--core-commit", required=True)
    args = parser.parse_args()
    workspace, stage, version, info = prepare_stage(args)
    build = build_deb if args.format == "deb" else build_appimage
    output = build(args, workspace, stage, version, info)
    (workspace / "build-info.json").write_text(json.dumps({**info, "file": output.name}, indent=2) + "\n")
    output.with_name(output.name + ".sha256").write_text(f"{digest(output)}  {output.name}\n")
    print(f"\nCreated: {output}")
    if args.api == "test":
        print("Built with the public test API credentials: for checking the packaging only, not for release.")


if __name__ == "__main__":
    main()
