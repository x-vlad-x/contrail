#!/usr/bin/env python3
"""Turn a finished build into Contrail's packages. Called by tools/build-packages.sh; never installs anything.

  python3 tools/package.py --arch aarch64|x86_64 --container IMAGE --api own|test --core-tag T --core-commit C
                           [--formats deb,rpm,appimage,flatpak]

Every package of an architecture is made from the same build:
- DEB, RPM and AppImage carry the program with every library it needs except the system's base ones
  (packaging/bundle-host-libraries.txt): one file installs on many distributions (tools/bundle.py);
- the Flatpak carries what its runtime (GNOME, from Flathub) doesn't have.
Commands that need the build environment (cmake --install, strip, readelf, dpkg-shlibdeps, the AppImage tools) run
in the Debian build container, rpmbuild in a Fedora container and the Flatpak tools in a container of their own,
so the result doesn't depend on the host. The outputs land in out/packages/.
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
    "aarch64": dict(deb="arm64", triple="aarch64-linux-gnu", build="build-aarch64-trixie",
                    deps="deps-aarch64-trixie"),
    "x86_64": dict(deb="amd64", triple="x86_64-linux-gnu", build="build-x86_64", deps="deps-x86_64"),
}
FORMATS = ("deb", "rpm", "appimage", "flatpak")
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
LICENSE_RE = re.compile(r"(^|/)(LICEN[CS]E|COPYING|NOTICE|PATENTS|AUTHORS|UNLICENSE)[^/]*$", re.I)
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".rs", ".py", ".go", ".js", ".m", ".mm", ".S", ".asm")
# Where the DEB and RPM put the bundle (the launcher is linked into /usr/bin).
SYSTEM_BUNDLE = "usr/lib/contrail"
RPM_CONTAINER = ("localhost/contrail-rpm", "build-env/Containerfile.rpm")
FLATPAK_CONTAINER = ("localhost/contrail-flatpak", "build-env/Containerfile.flatpak")
FLATPAK_RUNTIME = ("org.gnome.Platform", "51")
FLATPAK_REMOTE = "https://dl.flathub.org/repo/flathub.flatpakrepo"
# The runtime's GL extension (Mesa or a vendor driver) provides these at run time.
FLATPAK_GL = ["libEGL.so.1", "libGL.so.1", "libGLX.so.0", "libGLdispatch.so.0", "libOpenGL.so.0", "libGLESv2.so.2",
              "libgbm.so.1", "libglapi.so.0"]
# Sandbox permissions: what the interface and the core use (see gtk-ui/: notifications through the shell, the
# tray icon, feedbackd's ringtone, idle inhibition during calls, logind's lock state and the proximity sensor).
FLATPAK_PERMISSIONS = [
    "--share=network", "--share=ipc", "--socket=wayland", "--socket=fallback-x11", "--socket=pulseaudio",
    "--device=all", "--filesystem=xdg-download", "--filesystem=xdg-pictures",
    "--talk-name=org.freedesktop.Notifications", "--talk-name=org.kde.StatusNotifierWatcher",
    # The tray icon's name has the process ID, which is 2 in the sandbox (gtk-ui/tray.cpp).
    "--own-name=org.kde.StatusNotifierItem-2-1", "--talk-name=org.sigxcpu.Feedback",
    "--talk-name=org.gnome.SessionManager", "--system-talk-name=org.freedesktop.login1",
    "--system-talk-name=net.hadess.SensorProxy",
    "--env=QT_PLUGIN_PATH=/app/lib/qt6/plugins",
]


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, text=True, **kwargs)


def engine():
    return os.environ.get("CONTAINER_ENGINE") or shutil.which("podman") or shutil.which("docker")


def container_path(path):
    return Path("/src") / Path(path).resolve().relative_to(ROOT)


def ensure_image(image, recipe):
    """Builds a packaging container when its recipe changed (labelled like build-packages.sh does)."""
    recipe = ROOT / recipe
    wanted = hashlib.sha256(recipe.read_bytes()).hexdigest()
    label = subprocess.run([engine(), "image", "inspect", "--format", '{{ index .Config.Labels "contrail.recipe" }}',
                            image], capture_output=True, text=True).stdout.strip()
    if label != wanted:
        run([engine(), "build", "--label", f"contrail.recipe={wanted}", "-t", image, "-f", recipe, recipe.parent])
    return image


def execute(image, command, *, cwd=ROOT, capture=False, env=None, network=False):
    """Runs a command in a container (paths under ROOT are /src there)."""
    tool = engine()
    user = (["--userns=keep-id"] if Path(tool).name == "podman"
            else ["--user", f"{os.getuid()}:{os.getgid()}"])
    options = ["run", "--rm", *user, "-e", "HOME=/tmp", "-v", f"{ROOT}:/src:z", "-w", container_path(cwd)]
    if not network:
        options.append("--network=none")
    for key, value in (env or {}).items():
        options += ["-e", f"{key}={value}"]
    return run([tool, *options, image, *command], capture_output=capture)


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


def workspace(arch):
    return ROOT / "out/packages" / ARCHES[arch]["deb"]


# --- file names ------------------------------------------------------------------------------------------------

def deb_version(version):
    # 0.1.0-preview1 -> 0.1.0~preview1-1: a preview sorts before the final 0.1.0.
    return re.sub(r"-(preview|rc|beta|alpha)", r"~\1", version) + "-1"


def rpm_version(version):
    return re.sub(r"-(preview|rc|beta|alpha)", r"~\1", version)


def package_file(kind, arch, version):
    """The published file name of a package (GitHub rewrites "~", so it never appears in one)."""
    deb_arch = ARCHES[arch]["deb"]
    if kind == "deb":
        return f"contrail_{deb_version(version).replace('~', '-')}_{deb_arch}.deb"
    if kind == "rpm":
        return f"contrail-{rpm_version(version).replace('~', '-')}-1.{arch}.rpm"
    if kind == "appimage":
        return f"Contrail-{version}-{arch}.AppImage"
    if kind == "flatpak":
        return f"Contrail-{version}-{arch}.flatpak"
    raise ValueError(kind)


# --- the installed program ---------------------------------------------------------------------------------------

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
    sources = ROOT / ARCHES[args.arch]["deps"] / "sources"
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
    """cmake --install of the Contrail component into STAGE/usr, stripped, with icons, notices and build records."""
    space = workspace(args.arch)
    stage = space / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    build = ROOT / ARCHES[args.arch]["build"]
    triple = ARCHES[args.arch]["triple"]
    execute(args.container, ["env", f"DESTDIR={container_path(stage)}", "cmake", "--install", container_path(build),
                             "--component", "Contrail", "--prefix", "/usr"])
    program = stage / "usr/libexec/contrail/contrail"
    doc = stage / "usr/share/doc/contrail"
    # The Debian packages the build used: headers, static parts, shared libraries (see LICENSES.md).
    execute(args.container, ["python3", container_path(ROOT / "tools/build_inputs.py"), container_path(build),
                             container_path(program), container_path(doc / "build-inputs.json")])
    execute(args.container, [f"{triple}-strip", "--remove-section=.comment", "--remove-section=.note",
                             container_path(program)])
    icons = stage / "usr/share/icons/hicolor"
    svg = icons / f"scalable/apps/{APP_ID}.svg"
    for size in ICON_SIZES:
        png = icons / f"{size}x{size}/apps/{APP_ID}.png"
        png.parent.mkdir(parents=True, exist_ok=True)
        execute(args.container, ["rsvg-convert", "-w", size, "-h", size, "-o", container_path(png),
                                 container_path(svg)])
    execute(args.container, ["desktop-file-validate", container_path(stage / f"usr/share/applications/{APP_ID}.desktop")])
    execute(args.container, ["appstreamcli", "validate", "--no-net", "--explain",
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
        "project_commit": commit,
        "project_dirty": dirty,
        "source_date": source_date,
        "core_tag": args.core_tag,
        "core_commit": args.core_commit,
        "core_patches_sha256": core_patch_digest(),
        "api_credentials": args.api,
        "container": args.container,
        "container_image": image,
        "status": "for checking the packaging only, not for distribution" if args.api == "test" else "release",
    }
    (doc / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    lines = [f"Contrail {version}",
             f"Telegram Desktop core {args.core_tag.removeprefix('v')} ({args.core_commit})",
             f"Source commit {commit}{' with local changes' if dirty else ''}"]
    if args.api == "test":
        lines.append("Built with Telegram's public test API credentials: not for distribution")
    (stage / "usr/share/contrail/version").write_text("\n".join(lines) + "\n")
    # The packages installed in the build container: the exact build environment.
    listing = execute(args.container, ["dpkg-query", "-W", "-f=${binary:Package}\\t${Version}\\n"], capture=True).stdout
    (space / "build-environment.txt").write_text(
        f"# {args.container} {image}\n" + "".join(sorted(listing.splitlines(keepends=True))))
    return stage, version, info


def bundle_step(args, action, root, *options):
    execute(args.container, ["python3", container_path(ROOT / "tools/bundle.py"), action, container_path(root),
                             ARCHES[args.arch]["triple"], *options])


def make_bundle(args, stage):
    """The self-contained tree the DEB, RPM and AppImage share: the program and everything it loads."""
    bundle = workspace(args.arch) / "bundle"
    if bundle.exists():
        shutil.rmtree(bundle)
    shutil.copytree(stage / "usr", bundle, symlinks=True)
    host = container_path(ROOT / "packaging/bundle-host-libraries.txt")
    bundle_step(args, "resources", bundle, "--gtk")
    bundle_step(args, "deploy", bundle, "--host", host)
    bundle_step(args, "check", bundle, "--host", host)
    bundle_step(args, "licenses", bundle)
    return bundle


def system_tree(bundle, root):
    """The DEB and RPM file tree: the bundle in /usr/lib/contrail, the desktop integration in /usr/share."""
    if root.exists():
        shutil.rmtree(root)
    target = root / SYSTEM_BUNDLE
    shutil.copytree(bundle, target, symlinks=True)
    share = root / "usr/share"
    for name in ("applications", "metainfo", "doc"):
        (share / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.move(target / "share" / name, share / name)
    for icon in sorted((target / "share/icons/hicolor").glob(f"*/apps/{APP_ID}.*")):
        destination = share / "icons/hicolor" / icon.relative_to(target / "share/icons/hicolor")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(icon, destination)
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/bin/contrail").symlink_to("../lib/contrail/bin/contrail")
    # The migration helper looks for the system's desktop entry next to its own prefix (/usr).
    shutil.move(target / "bin/contrail-migrate-launcher", root / "usr/bin/contrail-migrate-launcher")
    for path in root.rglob("*"):
        if path.is_symlink():
            continue
        if path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            path.chmod(0o755 if os.access(path, os.X_OK) else 0o644)


# --- Debian package -----------------------------------------------------------------------------------------

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
 /usr/share/doc/contrail/static-libraries (tg_owt, TDLib's tde2e, rnnoise, the Rust crates and others).
 The libraries in /usr/lib/contrail come from Debian 13; their copyright files are in
 /usr/share/doc/contrail/bundled. See /usr/share/doc/contrail/LICENSES.md. Corresponding sources are
 published with each release.

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


DESCRIPTION = """Contrail is an unofficial client that uses the Telegram API. It is built on
the Telegram Desktop core and replaces its interface with an adaptive GTK4
and libadwaita one, made for Linux phones (Phosh) and usable on desktops.
The package carries the libraries it needs.

Contrail is not affiliated with or endorsed by Telegram."""


# libepoxy (GTK's GL loader) opens these with dlopen(), outside the ELF dependencies.
DLOPENED_DEB = ["libegl1", "libgl1", "libgles2", "libopengl0"]
DLOPENED_RPM = ["libEGL.so.1()(64bit)", "libGL.so.1()(64bit)", "libGLESv2.so.2()(64bit)", "libOpenGL.so.0()(64bit)"]


def host_depends(args, root, depends):
    """dpkg-shlibdeps also names the Debian packages of the bundled libraries (it finds their symbols files by
    soname): keep only the packages of the libraries the bundle takes from the system, with the versions
    dpkg-shlibdeps worked out for them."""
    triple = ARCHES[args.arch]["triple"]
    linked = json.loads((root / "usr/share/doc/contrail/host-libraries.json").read_text())["libraries"]
    script = ("for name; do for dir in /usr/lib/$TRIPLE /lib/$TRIPLE /usr/lib /lib; do "
              "[ -e $dir/$name ] && { dpkg-query -S $dir/$name || dpkg-query -S $(readlink -f $dir/$name); } && break; "
              "done; done")
    output = execute(args.container, ["sh", "-c", script, "sh", *linked], capture=True,
                     env={"TRIPLE": triple}).stdout
    packages = {line.split(":", 1)[0].split(",")[0].strip() for line in output.splitlines() if ":" in line}
    if len(packages) < 1 or not {"libc6"} <= packages:
        raise RuntimeError(f"Could not find the packages of the system libraries: {output}")
    kept, named = [], set()
    for entry in (e.strip() for e in depends.split(",")):
        names = {alternative.split()[0] for alternative in entry.split("|")}
        if names & packages:
            kept.append(entry)
            named |= names
    kept += sorted(packages - named)
    # Loaded at run time, not linked: GTK's libepoxy opens the system's GL libraries itself.
    kept += [name for name in DLOPENED_DEB if name not in named | packages]
    return kept


def build_deb(args, bundle, version, info):
    space = workspace(args.arch)
    deb_arch = ARCHES[args.arch]["deb"]
    package_version = deb_version(version)
    root = space / "deb/root"
    system_tree(bundle, root)
    doc = root / "usr/share/doc/contrail"
    write_copyright(doc)
    date = time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime(info["source_date"]))
    changelog = (f"contrail ({package_version}) unstable; urgency=medium\n\n"
                 f"  * Contrail {version}. See {HOMEPAGE}/releases.\n\n -- {MAINTAINER}  {date}\n")
    with gzip.GzipFile(doc / "changelog.Debian.gz", "wb", mtime=0) as stream:
        stream.write(changelog.encode())

    # Dependencies on the system's libraries (those the bundle leaves to the host), from the shlibs/symbols files of
    # the build container's packages. The bundled libraries belong to no package and are skipped.
    debian = space / "deb/debian"
    debian.mkdir(parents=True, exist_ok=True)
    (debian / "control").write_text(f"Source: contrail\nMaintainer: {MAINTAINER}\n\n"
                                     f"Package: contrail\nArchitecture: {deb_arch}\nDescription: Contrail\n")
    elfs = [p for p in sorted((root / SYSTEM_BUNDLE).rglob("*")) if p.is_file() and not p.is_symlink()
            and p.read_bytes()[:4] == b"\x7fELF"]
    env = {"DEB_HOST_ARCH": deb_arch, "DEB_HOST_MULTIARCH": ARCHES[args.arch]["triple"]}
    output = execute(args.container, ["dpkg-shlibdeps", "-O", "--warnings=0", "--ignore-missing-info",
                                      f"-l{container_path(root / SYSTEM_BUNDLE / 'lib')}",
                                      *[f"-e{container_path(p)}" for p in elfs]],
                     cwd=space / "deb", capture=True, env=env).stdout
    depends = next(line.split("=", 1)[1] for line in output.splitlines() if line.startswith("shlibs:Depends="))
    depends = ", ".join(host_depends(args, root, depends))

    installed_size = (sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink())
                      + 1023) // 1024
    control = root / "DEBIAN"
    control.mkdir()
    (control / "control").write_text(
        f"Package: contrail\nVersion: {package_version}\nArchitecture: {deb_arch}\n"
        f"Maintainer: {MAINTAINER}\nSection: net\nPriority: optional\n"
        f"Installed-Size: {installed_size}\nDepends: {depends}\n"
        # pactl switches the call audio between the earpiece, the loudspeaker and headphones.
        "Recommends: pulseaudio-utils, xdg-desktop-portal, xdg-utils, fonts-noto-color-emoji\n"
        f"Homepage: {HOMEPAGE}\n"
        "Description: unofficial Telegram client with a GTK interface for Linux phones\n"
        + "".join(f" {line}\n" if line else " .\n" for line in DESCRIPTION.splitlines()))
    sums = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink() and not path.is_relative_to(control):
            sums.append(f"{hashlib.md5(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}\n")
    (control / "md5sums").write_text("".join(sums))
    output = ROOT / "out/packages" / package_file("deb", args.arch, version)
    execute(args.container, ["dpkg-deb", "--root-owner-group", "-Zxz", "--build", container_path(root),
                             container_path(output)])
    print(execute(args.container, ["dpkg-deb", "--info", container_path(output)], capture=True).stdout)
    return output


# --- RPM ---------------------------------------------------------------------------------------------------------

SPEC = """%global debug_package %{nil}
# The files are built and stripped already: no rebuilding, stripping or build-id links here.
%global __os_install_post %{nil}
%global _build_id_links none
# The bundled libraries are private: they provide nothing to the system, and nothing has to provide them.
%global __provides_exclude_from ^/usr/lib/contrail/.*$
%global __requires_exclude ^(@BUNDLED@)
%global _binary_payload w19T0.zstdio

Name:           contrail
Version:        @VERSION@
Release:        1
Summary:        Unofficial Telegram client with a GTK interface for Linux phones
License:        GPL-3.0-only WITH LicenseRef-OpenSSL-exception AND LicenseRef-Bundled-libraries
URL:            @HOMEPAGE@
# libepoxy (GTK's GL loader) opens the system's GL libraries with dlopen(), outside the ELF dependencies.
@DLOPENED@
# pactl switches the call audio between the earpiece, the loudspeaker and headphones.
Recommends:     pulseaudio-utils
Recommends:     xdg-desktop-portal
Recommends:     xdg-utils

%description
@DESCRIPTION@

%install
cp -a @TREE@/. %{buildroot}/

%files
%defattr(-,root,root,-)
/usr/bin/contrail
/usr/bin/contrail-migrate-launcher
/usr/lib/contrail/
/usr/share/applications/@APP_ID@.desktop
/usr/share/icons/hicolor/*/apps/@APP_ID@.*
/usr/share/metainfo/@APP_ID@.metainfo.xml
%doc /usr/share/doc/contrail/

%changelog
* @DATE@ @MAINTAINER@ - @VERSION@-1
- Contrail @RELEASE@. See @HOMEPAGE@/releases.
"""


def build_rpm(args, bundle, version, info):
    space = workspace(args.arch) / "rpm"
    if space.exists():
        shutil.rmtree(space)
    tree = space / "tree"
    system_tree(bundle, tree)
    host = set((ROOT / "packaging/bundle-host-libraries.txt").read_text().split())
    bundled = sorted(p.name for p in (tree / SYSTEM_BUNDLE / "lib").glob("*.so*") if p.name not in host)
    spec = space / "contrail.spec"
    date = time.strftime("%a %b %d %Y", time.gmtime(info["source_date"]))
    replacements = {
        "@BUNDLED@": "|".join(re.escape(name) for name in bundled),
        "@VERSION@": rpm_version(version), "@RELEASE@": version, "@HOMEPAGE@": HOMEPAGE, "@APP_ID@": APP_ID,
        "@DESCRIPTION@": DESCRIPTION, "@TREE@": str(container_path(tree)), "@DATE@": date,
        "@MAINTAINER@": MAINTAINER,
        "@DLOPENED@": "\n".join(f"Requires:       {name}" for name in DLOPENED_RPM),
    }
    text = SPEC
    for key, value in replacements.items():
        text = text.replace(key, value)
    spec.write_text(text)
    image = ensure_image(*RPM_CONTAINER)
    output = ROOT / "out/packages" / package_file("rpm", args.arch, version)
    execute(image, ["rpmbuild", "-bb", "--target", f"{args.arch}-linux", "--define", f"_topdir {container_path(space)}",
                    "--define", f"_rpmdir {container_path(space / 'RPMS')}",
                    "--define", "_build_name_fmt %{NAME}.rpm", "--define", "_buildhost contrail-build",
                    "--define", "use_source_date_epoch_as_buildtime 1",
                    "--define", "clamp_mtime_to_source_date_epoch 1",
                    container_path(spec)], env={"SOURCE_DATE_EPOCH": str(info["source_date"])})
    shutil.move(space / "RPMS/contrail.rpm", output)
    print(execute(image, ["rpm", "-qp", "--info", "--requires", "--recommends", container_path(output)],
                  capture=True).stdout)
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


def build_appimage(args, bundle, version, info):
    tools = download_tools()
    appdir = workspace(args.arch) / "appimage/Contrail.AppDir"
    if appdir.parent.exists():
        shutil.rmtree(appdir.parent)
    appdir.mkdir(parents=True)
    shutil.copytree(bundle, appdir / "usr", symlinks=True)
    doc = appdir / "usr/share/doc/contrail"
    shutil.copytree(ROOT / "packaging/licenses/appimage-runtime", doc / "appimage-runtime", dirs_exist_ok=True)
    shutil.copy2(ROOT / "packaging/runtime-sources.json", doc / "appimage-runtime/runtime-sources.json")
    shutil.copy2(ROOT / "packaging/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    shutil.copy2(appdir / f"usr/share/applications/{APP_ID}.desktop", appdir / f"{APP_ID}.desktop")
    shutil.copy2(appdir / f"usr/share/icons/hicolor/256x256/apps/{APP_ID}.png", appdir / f"{APP_ID}.png")
    (appdir / ".DirIcon").symlink_to(f"{APP_ID}.png")
    output = ROOT / "out/packages" / package_file("appimage", args.arch, version)
    # appimagetool (x86_64) only packs the tree: the runtime prepended to it is the target's.
    execute(args.container, ["env", f"ARCH={args.arch}", "VERSION=" + version,
                             container_path(tools / "appimagetool-x86_64.AppImage"), "--appimage-extract-and-run",
                             "--no-appstream", "--runtime-file", container_path(tools / f"runtime-{args.arch}"),
                             container_path(appdir), container_path(output)])
    return output


# --- Flatpak -------------------------------------------------------------------------------------------------

def flatpak_runtime(args, image):
    """Downloads the runtime into out/flatpak/ (once) and lists the libraries it provides."""
    runtime, branch = FLATPAK_RUNTIME
    env = {"FLATPAK_USER_DIR": str(container_path(ROOT / "out/flatpak/user"))}
    (ROOT / "out/flatpak/user").mkdir(parents=True, exist_ok=True)
    ref = f"{runtime}/{args.arch}/{branch}"
    script = (f"flatpak remote-add --user --if-not-exists flathub {FLATPAK_REMOTE} && "
              f"(flatpak info --user {ref} >/dev/null 2>&1 || "
              f"flatpak install --user -y --noninteractive --no-related --arch={args.arch} flathub {ref}) && "
              f"flatpak info --user --show-commit {ref} && flatpak info --user --show-location {ref}")
    output = execute(image, ["sh", "-c", script], env=env, network=True, capture=True).stdout.split()
    commit, location = output[-2], output[-1]
    files = ROOT / "out/flatpak/user" / Path(location).relative_to(container_path(ROOT / "out/flatpak/user")) / "files"
    triple = ARCHES[args.arch]["triple"]
    names = set(FLATPAK_GL)
    for directory in (files / "lib" / triple, files / "lib"):
        for path in directory.glob("*.so*"):
            names.add(path.name)
    listing = workspace(args.arch) / "flatpak/runtime-libraries.txt"
    listing.parent.mkdir(parents=True, exist_ok=True)
    listing.write_text(f"# {ref} {commit} and its GL extension: left to the runtime by the Flatpak\n"
                       + "".join(f"{name}\n" for name in sorted(names)))
    return f"{runtime}/{args.arch}/{branch}", commit, listing


def build_flatpak(args, stage, version, info):
    space = workspace(args.arch) / "flatpak"
    image = ensure_image(*FLATPAK_CONTAINER)
    ref, commit, listing = flatpak_runtime(args, image)
    build = space / "build"
    for path in (build, space / "repo"):
        if path.exists():
            shutil.rmtree(path)
    files = build / "files"
    shutil.copytree(stage / "usr", files, symlinks=True)
    (files / "bin/contrail-migrate-launcher").unlink()  # nothing to migrate inside a sandbox
    bundle_step(args, "resources", files, "--pactl")
    bundle_step(args, "deploy", files, "--host", container_path(listing))
    bundle_step(args, "check", files, "--host", container_path(listing))
    bundle_step(args, "licenses", files)
    doc = files / "share/doc/contrail"
    (doc / "flatpak-runtime.txt").write_text(f"{ref} {commit}\n{FLATPAK_REMOTE}\n")
    sdk = ref.replace("Platform", "Sdk")
    (build / "metadata").write_text(f"[Application]\nname={APP_ID}\nruntime={ref}\nsdk={sdk}\ncommand=contrail\n")
    output = ROOT / "out/packages" / package_file("flatpak", args.arch, version)
    repo = container_path(space / "repo")
    script = " && ".join([
        f"flatpak build-finish {' '.join(FLATPAK_PERMISSIONS)} --command=contrail {container_path(build)}",
        # The icon validator runs unsandboxed: bwrap can't mount /proc in an unprivileged container (the icons
        # are the package's own).
        f"flatpak build-export --disable-sandbox --arch={args.arch} --subject='Contrail {version}' {repo} "
        f"{container_path(build)} stable",
        f"flatpak build-bundle --arch={args.arch} --runtime-repo={FLATPAK_REMOTE} {repo} {container_path(output)} "
        f"{APP_ID} stable",
    ])
    execute(image, ["sh", "-c", script])
    print((build / "metadata").read_text())
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", choices=sorted(ARCHES), required=True)
    parser.add_argument("--container", required=True, help="the image the build ran in")
    parser.add_argument("--api", choices=["own", "test"], required=True)
    parser.add_argument("--core-tag", required=True)
    parser.add_argument("--core-commit", required=True)
    parser.add_argument("--formats", default=",".join(FORMATS))
    args = parser.parse_args()
    formats = [f for f in args.formats.split(",") if f]
    if unknown := set(formats) - set(FORMATS):
        parser.error(f"unknown formats: {sorted(unknown)}")
    stage, version, info = prepare_stage(args)
    outputs = {}
    if {"deb", "rpm", "appimage"} & set(formats):
        bundle = make_bundle(args, stage)
        for kind, build in (("deb", build_deb), ("rpm", build_rpm), ("appimage", build_appimage)):
            if kind in formats:
                outputs[kind] = build(args, bundle, version, info)
    if "flatpak" in formats:
        outputs["flatpak"] = build_flatpak(args, stage, version, info)
    for path in outputs.values():
        path.with_name(path.name + ".sha256").write_text(f"{digest(path)}  {path.name}\n")
    (workspace(args.arch) / "build-info.json").write_text(
        json.dumps({**info, "files": {kind: path.name for kind, path in outputs.items()}}, indent=2) + "\n")
    for path in outputs.values():
        print(f"Created: {path} ({path.stat().st_size / 2**20:.1f} MiB)")
    if args.api == "test":
        print("Built with the public test API credentials: for checking the packaging only, not for release.")


if __name__ == "__main__":
    main()
