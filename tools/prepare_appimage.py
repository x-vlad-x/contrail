#!/usr/bin/env python3
"""AppImage helpers, run inside the x86_64 build container (Containerfile.desktop) by tools/package.py.

  prepare_appimage.py resources APPDIR   copy the files GTK, GIO and Qt load at run time (not ELF dependencies)
  prepare_appimage.py check APPDIR       every bundled ELF must resolve; record what the host must provide
  prepare_appimage.py licenses APPDIR    copyright files and an inventory of the bundled Debian packages
"""
from pathlib import Path
import json
import re
import shutil
import subprocess
import sys

LIB = Path("/usr/lib/x86_64-linux-gnu")
DOC = Path("usr/share/doc/contrail")
# Qt runs offscreen; Telegram Desktop needs the image formats (WebP emoji) and the TLS backend.
QT_PLUGINS = ["platforms/libqoffscreen.so", "imageformats", "tls", "networkinformation"]
GIO_MODULES = ["libdconfsettings.so", "libgiognutls.so", "libgiolibproxy.so"]
LOCALE_DOMAINS = {"gtk40", "libadwaita", "glib20", "gdk-pixbuf"}


def copy_tree(source, destination):
    if not source.exists():
        raise RuntimeError(f"Missing in the build container: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=False)


def resources(appdir):
    plugins = appdir / "usr/lib/qt6/plugins"
    for name in QT_PLUGINS:
        source = LIB / "qt6/plugins" / name
        if source.is_dir():
            copy_tree(source, plugins / name)
        else:
            (plugins / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, plugins / name)
    modules = appdir / "usr/lib/gio/modules"
    modules.mkdir(parents=True, exist_ok=True)
    for name in GIO_MODULES:
        shutil.copy2(LIB / "gio/modules" / name, modules / name)
    subprocess.run(["gio-querymodules", modules], check=True)
    # GTK loads SVG icons through gdk-pixbuf's librsvg loader; AppRun regenerates the cache (absolute paths).
    copy_tree(LIB / "gdk-pixbuf-2.0/2.10.0/loaders", appdir / "usr/lib/gdk-pixbuf-2.0/2.10.0/loaders")
    (appdir / "usr/bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(LIB / "gdk-pixbuf-2.0/gdk-pixbuf-query-loaders", appdir / "usr/bin/gdk-pixbuf-query-loaders")
    schemas = appdir / "usr/share/glib-2.0/schemas"
    copy_tree(Path("/usr/share/glib-2.0/schemas"), schemas)
    subprocess.run(["glib-compile-schemas", schemas], check=True)
    for theme in ("Adwaita", "hicolor"):
        copy_tree(Path("/usr/share/icons") / theme, appdir / "usr/share/icons" / theme)
    copy_tree(Path("/usr/share/mime"), appdir / "usr/share/mime")
    for catalog in Path("/usr/share/locale").glob("*/LC_MESSAGES/*.mo"):
        if catalog.stem in LOCALE_DOMAINS:
            target = appdir / catalog.relative_to("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(catalog, target)


def elf_files(appdir):
    for path in sorted(appdir.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    yield path


def check(appdir):
    """Resolves every bundled ELF; what comes from outside the AppDir must be a base system library."""
    host, problems, glibc, glibcxx = {}, [], (2, 0), 0
    for path in elf_files(appdir):
        result = subprocess.run(["ldd", path], capture_output=True, text=True)
        if "not a dynamic executable" in result.stdout + result.stderr:
            continue
        for line in result.stdout.splitlines():
            line = line.strip()
            if "not found" in line:
                problems.append(f"{path.relative_to(appdir)}: {line}")
            match = re.match(r"(\S+) => (/\S+)", line)
            if match and not Path(match.group(2)).resolve().is_relative_to(appdir.resolve()):
                host.setdefault(match.group(1), set()).add(path.relative_to(appdir).as_posix())
        symbols = subprocess.run(["objdump", "-T", path], capture_output=True, text=True).stdout
        for major, minor in re.findall(r"GLIBC_(\d+)\.(\d+)", symbols):
            glibc = max(glibc, (int(major), int(minor)))
        for version in re.findall(r"GLIBCXX_3\.4\.(\d+)", symbols):
            glibcxx = max(glibcxx, int(version))
    if problems:
        raise RuntimeError("Unresolved libraries in the AppImage:\n" + "\n".join(problems))
    # linuxdeploy leaves these to the host on purpose (glibc, graphics drivers, fontconfig...).
    lines = [f"glibc {glibc[0]}.{glibc[1]} or newer", f"libstdc++ with GLIBCXX_3.4.{glibcxx} or newer", "",
             "Directly or through other host libraries (the count is of bundled files that end up using it):"]
    lines += [f"{name}\t(used by {len(users)} bundled files)" for name, users in sorted(host.items())]
    target = appdir / DOC / "host-libraries.txt"
    target.write_text("Libraries the AppImage takes from the host system:\n" + "\n".join(lines) + "\n")
    print(target.read_text())


def licenses(appdir):
    """Each bundled file is traced to the Debian package that installed it in the build container."""
    paths, names = {}, {}
    for listing in Path("/var/lib/dpkg/info").glob("*.list"):
        package = listing.name.removesuffix(".list")
        for name in listing.read_text().splitlines():
            paths.setdefault(name, set()).add(package)
            names.setdefault(Path(name).name, set()).add(package)
    owners = {}
    for path in sorted((appdir / "usr").rglob("*")):
        relative = path.relative_to(appdir).as_posix()
        if not path.is_file() or relative.startswith(("usr/libexec/", "usr/share/doc/", "usr/share/contrail/")):
            continue
        if relative in {"usr/bin/contrail", "usr/bin/contrail-migrate-launcher"} or \
                ("/hicolor/" in relative and "io.github.x_vlad_x.Contrail" in relative):
            continue
        if relative.startswith(("usr/share/applications/", "usr/share/metainfo/")):
            continue
        with path.open("rb") as stream:
            elf = stream.read(4) == b"\x7fELF"
        original = "/" + relative
        candidates = (paths.get(original) or paths.get(original.replace("/usr/lib/", f"{LIB}/", 1))
                      or paths.get(original.replace("/usr/lib/", "/lib/x86_64-linux-gnu/", 1)))
        if not candidates and elf:
            candidates = names.get(path.name)
        if not candidates:
            if elf:
                raise RuntimeError(f"Bundled ELF has no Debian package: {relative}")
            # Generated in the AppDir (schema and module caches).
            if path.name not in {"gschemas.compiled", "giomodule.cache", "icon-theme.cache"} and \
                    not relative.startswith("usr/share/mime/"):
                raise RuntimeError(f"Bundled file has no Debian package: {relative}")
            continue
        for package in candidates:
            owners.setdefault(package, []).append(relative)
    target = appdir / DOC / "bundled"
    target.mkdir(parents=True, exist_ok=True)
    inventory = []
    fields = "${binary:Package}\t${Version}\t${source:Package}\t${source:Version}"
    for package, files in sorted(owners.items()):
        name = package.split(":", 1)[0]
        source = Path("/usr/share/doc") / name / "copyright"
        if not source.exists():
            raise RuntimeError(f"Bundled package has no copyright file: {package}")
        shutil.copy2(source, target / f"{name}.copyright")
        result = subprocess.run(["dpkg-query", "-W", "-f=" + fields, package], check=True,
                                capture_output=True, text=True)
        binary, version, source_name, source_version = result.stdout.split("\t")
        inventory.append(dict(binary_package=binary, binary_version=version, source_package=source_name,
                              source_version=source_version, files=files))
    copy_tree(Path("/usr/share/common-licenses"), target / "common-licenses")
    (target / "bundled-packages.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print(f"Bundled files come from {len(inventory)} Debian packages")


if __name__ == "__main__":
    action, appdir = sys.argv[1], Path(sys.argv[2])
    {"resources": resources, "check": check, "licenses": licenses}[action](appdir)
