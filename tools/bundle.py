#!/usr/bin/env python3
"""Self-contained packages: puts the shared libraries and run-time resources of a finished build into a bundle
tree. Runs inside the build container (Containerfile.desktop for x86_64, Containerfile.arm64 for aarch64),
called by tools/package.py. ELF files are only read (readelf), so an ARM64 bundle is made
on x86_64 the same way; only gio-querymodules runs, through qemu-user when cross-building.

  bundle.py resources ROOT TRIPLE [--gtk] [--pactl]   files loaded at run time that aren't ELF dependencies:
                                                      Qt plugins; with --gtk also GIO modules, gdk-pixbuf loaders,
                                                      GSettings schemas, icon themes, MIME data, translations;
                                                      with --pactl also PulseAudio's pactl (the Flatpak runtime
                                                      has none)
  bundle.py deploy ROOT TRIPLE --host FILE [--own DIR] copies every shared library the ELF files in ROOT need,
                                                      except those listed in FILE (the host's or the Flatpak
                                                      runtime's), into ROOT/lib; libraries built by build-deps.sh
                                                      (DIR, its prefix's lib/: the small FFmpeg) come first
  bundle.py check ROOT TRIPLE --host FILE             every ELF resolves inside ROOT or to FILE; records what the
                                                      host must provide (share/doc/contrail/host-libraries.txt)
  bundle.py licenses ROOT TRIPLE [--own DIR]          the copyright files and an inventory of the bundled Debian
                                                      packages (share/doc/contrail/bundled/); the libraries built
                                                      from a Debian source package by build-deps.sh are listed with
                                                      that source package

ROOT is the bundle's prefix: AppDir/usr in the AppImage, /usr/lib/contrail in the DEB and RPM, /app in the
Flatpak. It has bin/, libexec/contrail/ (the program), lib/ (the bundled libraries) and share/.

The bundled libraries are Debian's files as they are: the program finds them through its DT_RPATH
($ORIGIN/../../lib, set when it is linked), which also applies to every library it loads (the ones without a
RUNPATH of their own). Rewriting libraries with patchelf can move sections that code refers to by address
(libadwaita's resources on ARM64), so only a RUNPATH that points into the system is replaced, in place.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

DOC = Path("share/doc/contrail")
ORIGINS = DOC / "bundled/origins.json"
# Qt runs offscreen; Telegram Desktop needs the image formats (WebP emoji) and the TLS backend.
QT_PLUGINS = ["platforms/libqoffscreen.so", "imageformats", "tls", "networkinformation"]
GIO_MODULES = ["libdconfsettings.so", "libgiognutls.so", "libgiolibproxy.so"]
LOCALE_DOMAINS = {"gtk40", "libadwaita", "glib20", "gdk-pixbuf"}
# e_machine of each target.
MACHINES = {"x86_64-linux-gnu": 62, "aarch64-linux-gnu": 183}
# Files of the package itself, not taken from Debian.
OWN = ("libexec/", "share/doc/", "share/contrail/", "share/applications/", "share/metainfo/")
OWN_FILES = {"bin/contrail", "bin/contrail-migrate-launcher"}
APP_ID = "io.github.x_vlad_x.Contrail"


def run(command, **kwargs):
    return subprocess.run([str(c) for c in command], check=True, text=True, **kwargs)


def is_elf(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def machine(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    return int.from_bytes(header[18:20], "little")


def elf_files(root):
    return [path for path in sorted(root.rglob("*")) if is_elf(path)]


def dynamic(path):
    """NEEDED entries and RUNPATH/RPATH directories of an ELF file."""
    output = run(["readelf", "-dW", path], capture_output=True).stdout
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[(.+?)\]", output)
    paths = []
    for value in re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library r(?:un)?path: \[(.*?)\]", output):
        paths += [p for p in value.split(":") if p]
    return needed, paths


def library_dirs(triple):
    """Where the container's dynamic loader finds libraries, for the target architecture."""
    dirs = [f"/usr/lib/{triple}", f"/lib/{triple}"]
    for conf in sorted(Path("/etc/ld.so.conf.d").glob("*.conf")):
        for line in conf.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line and not line.startswith("include"):
                dirs.append(line)
    dirs += ["/usr/lib", "/lib"]
    return list(dict.fromkeys(dirs))


def read_host(path):
    names = set()
    for line in Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line.split()[0])
    return names


def copy_tree(source, destination):
    if not source.exists():
        raise RuntimeError(f"Missing in the build container: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=False)


def target_tool(triple, path):
    """A program of the target architecture, run through qemu-user when cross-building."""
    if os.uname().machine == triple.split("-")[0]:
        return [path]
    return [f"/usr/bin/qemu-{triple.split('-')[0]}", path]


def resources(root, triple, gtk, pactl):
    lib = Path(f"/usr/lib/{triple}")
    plugins = root / "lib/qt6/plugins"
    for name in QT_PLUGINS:
        source = lib / "qt6/plugins" / name
        if source.is_dir():
            copy_tree(source, plugins / name)
        else:
            (plugins / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, plugins / name)
    if pactl:
        (root / "bin").mkdir(parents=True, exist_ok=True)
        if machine(Path("/usr/bin/pactl")) != MACHINES[triple]:
            raise RuntimeError("/usr/bin/pactl is not built for the target")
        shutil.copy2("/usr/bin/pactl", root / "bin/pactl")
    if not gtk:
        return
    modules = root / "lib/gio/modules"
    modules.mkdir(parents=True, exist_ok=True)
    for name in GIO_MODULES:
        shutil.copy2(lib / "gio/modules" / name, modules / name)
    run([*target_tool(triple, lib / "glib-2.0/gio-querymodules"), modules])
    # GTK loads SVG icons through gdk-pixbuf's librsvg loader; the launcher writes the loader cache (it holds
    # absolute paths).
    copy_tree(lib / "gdk-pixbuf-2.0/2.10.0/loaders", root / "lib/gdk-pixbuf-2.0/2.10.0/loaders")
    shutil.copy2(lib / "gdk-pixbuf-2.0/gdk-pixbuf-query-loaders", root / "lib/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders")
    schemas = root / "share/glib-2.0/schemas"
    copy_tree(Path("/usr/share/glib-2.0/schemas"), schemas)
    run(["glib-compile-schemas", schemas])
    for theme in ("Adwaita", "hicolor"):
        copy_tree(Path("/usr/share/icons") / theme, root / "share/icons" / theme)
    copy_tree(Path("/usr/share/mime"), root / "share/mime")
    for catalog in Path("/usr/share/locale").glob("*/LC_MESSAGES/*.mo"):
        if catalog.stem in LOCALE_DOMAINS:
            target = root / "share" / catalog.relative_to("/usr/share")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(catalog, target)


def layout(path):
    """Section names, addresses and offsets: what must not change when a RUNPATH is replaced."""
    output = run(["readelf", "-SW", path], capture_output=True).stdout
    return [line.split()[1:5] for line in output.splitlines() if re.match(r"\s+\[\s*\d+\]", line)]


def set_runpath_in_place(path, value):
    before = layout(path)
    # patchelf writes a RUNPATH by default: an RPATH stays one (it also applies to the libraries loaded).
    rpath = "(RPATH)" in run(["readelf", "-dW", path], capture_output=True).stdout
    run(["patchelf", *(["--force-rpath"] if rpath else []), "--set-rpath", value, path])
    if layout(path) != before:
        raise RuntimeError(f"patchelf moved sections of {path.name}; it can't be used as it is")


def own_libraries(own):
    """The libraries build-deps.sh built from a Debian source package (DIR/../share/contrail/own-libraries.json)."""
    if not own:
        return {}
    listing = Path(own).parent / "share/contrail/own-libraries.json"
    return json.loads(listing.read_text()) if listing.exists() else {}


def deploy(root, triple, host, own=None):
    """Copies the libraries the ELF files in ROOT need (recursively) into ROOT/lib, leaving the host's to the
    host, then sets every ELF's RUNPATH to ROOT/lib. Libraries of OWN, build-deps.sh's own builds, are taken
    before the container's, and never left to the host."""
    target_machine = MACHINES[triple]
    default_dirs = ([str(own)] if own else []) + library_dirs(triple)
    built = own_libraries(own)
    host = host - set(built)
    origins_path = root / ORIGINS
    origins = json.loads(origins_path.read_text()) if origins_path.exists() else {}
    lib = root / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    # Where each ELF in ROOT came from: its own RUNPATH ($ORIGIN) refers to that place.
    queue = [(path, origins.get(path.relative_to(root).as_posix(), str(path))) for path in elf_files(root)]
    seen, missing = set(), []
    while queue:
        path, original = queue.pop()
        if machine(path) != target_machine:
            raise RuntimeError(f"Not built for {triple}: {path.relative_to(root)}")
        needed, paths = dynamic(path)
        search = [p.replace("$ORIGIN", str(Path(original).parent)) for p in paths] + default_dirs
        for name in needed:
            if name in host or name in seen:
                continue
            seen.add(name)
            if (lib / name).exists():
                continue
            found = None
            for directory in search:
                candidate = Path(directory) / name
                if candidate.exists() and machine(candidate.resolve()) == target_machine:
                    found = candidate.resolve()
                    break
            if not found:
                missing.append(f"{name} (needed by {path.relative_to(root)})")
                continue
            shutil.copy2(found, lib / name)
            (lib / name).chmod(0o644)
            origins[f"lib/{name}"] = str(Path(directory) / name)
            queue.append((lib / name, str(found)))
    if missing:
        raise RuntimeError("Libraries not found in the build container:\n" + "\n".join(missing))
    for path in elf_files(root):
        paths = dynamic(path)[1]
        if paths and any(not p.startswith("$ORIGIN") for p in paths):
            relative = os.path.relpath(lib, path.parent)
            set_runpath_in_place(path, "$ORIGIN" if relative == "." else f"$ORIGIN/{relative}")
    origins_path.parent.mkdir(parents=True, exist_ok=True)
    origins_path.write_text(json.dumps(dict(sorted(origins.items())), indent=2) + "\n")
    print(f"Bundled {len(list(lib.glob('*.so*')))} libraries in {lib}")


def version_needs(path):
    """The symbol versions an ELF file needs, by library."""
    output = run(["readelf", "-VW", path], capture_output=True).stdout
    needs, current = {}, None
    for line in output.splitlines():
        match = re.search(r"File: (\S+)\s+Cnt:", line)
        if match:
            current = match.group(1)
            continue
        match = re.search(r"Name: (\S+)\s+Flags:", line)
        if match and current:
            needs.setdefault(current, set()).add(match.group(1))
    return needs


def check(root, triple, host):
    host_users, problems = {}, []
    glibc, glibcxx = (2, 0), 0
    program = root / "libexec/contrail/contrail"
    if f"$ORIGIN/{os.path.relpath(root / 'lib', program.parent)}" not in dynamic(program)[1]:
        raise RuntimeError("The program has no RPATH to the bundled libraries (CONTRAIL_BUNDLE_RPATH)")
    # A RUNPATH would only serve the program's own dependencies, not the libraries they need in turn.
    if "(RUNPATH)" in run(["readelf", "-dW", program], capture_output=True).stdout:
        raise RuntimeError("The program has a RUNPATH instead of an RPATH: bundled libraries' dependencies won't load")
    for path in elf_files(root):
        needed, paths = dynamic(path)
        dirs = [Path(p.replace("$ORIGIN", str(path.parent))) for p in paths]
        if not paths or path.parent == root / "lib":
            dirs.append(root / "lib")
        for name in needed:
            if any((d / name).exists() and (d / name).resolve().is_relative_to(root.resolve()) for d in dirs):
                continue
            if name in host:
                host_users.setdefault(name, set()).add(path.relative_to(root).as_posix())
            else:
                problems.append(f"{path.relative_to(root)}: {name}")
        for library, versions in version_needs(path).items():
            for version in versions:
                match = re.fullmatch(r"GLIBC_(\d+)\.(\d+)(?:\.\d+)?", version)
                if match and library in host:
                    glibc = max(glibc, (int(match.group(1)), int(match.group(2))))
                match = re.fullmatch(r"GLIBCXX_3\.4\.(\d+)", version)
                if match and library in host:
                    glibcxx = max(glibcxx, int(match.group(1)))
    if problems:
        raise RuntimeError("Unresolved libraries in the bundle:\n" + "\n".join(problems))
    lines = [f"glibc {glibc[0]}.{glibc[1]} or newer"]
    if glibcxx:
        lines.append(f"libstdc++ with GLIBCXX_3.4.{glibcxx} or newer")
    lines += ["", "Libraries the bundled files link (the count is of bundled files that link each one):"]
    lines += [f"{name}\t({len(users)})" for name, users in sorted(host_users.items())]
    target = root / DOC / "host-libraries.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("What the package takes from the system:\n" + "\n".join(lines) + "\n")
    (root / DOC / "host-libraries.json").write_text(json.dumps(dict(
        glibc=f"{glibc[0]}.{glibc[1]}", glibcxx=glibcxx, libraries=sorted(host_users)), indent=2) + "\n")
    print(target.read_text())


def licenses(root, triple, own=None):
    """Each bundled file is traced to the Debian package that installed it in the build container, or, for the
    libraries build-deps.sh built from a Debian source package, to that source package."""
    built = own_libraries(own)
    paths, names = {}, {}
    for listing in Path("/var/lib/dpkg/info").glob("*.list"):
        package = listing.name.removesuffix(".list")
        for name in listing.read_text().splitlines():
            paths.setdefault(name, set()).add(package)
            names.setdefault(Path(name).name, set()).add(package)
    origins_path = root / ORIGINS
    origins = json.loads(origins_path.read_text()) if origins_path.exists() else {}

    def originals(relative):
        if relative in origins:
            yield origins[relative]
        if relative.startswith("lib/"):
            rest = relative.removeprefix("lib/")
            yield f"/usr/lib/{triple}/{rest}"
            yield f"/lib/{triple}/{rest}"
            yield f"/usr/lib/{rest}"
        else:
            yield f"/usr/{relative}"

    owners, rebuilt = {}, {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if not path.is_file() or relative.startswith(OWN) or relative in OWN_FILES:
            continue
        origin = origins.get(relative, "")
        if own and origin.startswith(str(own) + "/") and path.name in built:
            rebuilt.setdefault(path.name, built[path.name])
            continue
        if "/hicolor/" in relative and APP_ID in relative:
            continue
        candidates = set()
        for original in originals(relative):
            candidates = paths.get(original) or paths.get(str(Path(original).resolve())) or set()
            if candidates:
                break
        elf = is_elf(path)
        if not candidates and elf:
            candidates = {p for p in names.get(path.name, set()) if p.endswith(f":{triple_arch(triple)}") or ":" not in p}
        if not candidates:
            if elf:
                raise RuntimeError(f"Bundled ELF has no Debian package: {relative}")
            # Generated in the bundle (schema and module caches, the MIME database).
            if path.name not in {"gschemas.compiled", "giomodule.cache", "icon-theme.cache"} and \
                    not relative.startswith("share/mime/"):
                raise RuntimeError(f"Bundled file has no Debian package: {relative}")
            continue
        for package in candidates:
            owners.setdefault(package, []).append(relative)
    target = root / DOC / "bundled"
    target.mkdir(parents=True, exist_ok=True)
    inventory = []
    fields = "${binary:Package}\t${Version}\t${source:Package}\t${source:Version}"
    for package, files in sorted(owners.items()):
        name = package.split(":", 1)[0]
        source = Path("/usr/share/doc") / name / "copyright"
        if not source.exists():
            raise RuntimeError(f"Bundled package has no copyright file: {package}")
        shutil.copy2(source, target / f"{name}.copyright")
        result = run(["dpkg-query", "-W", "-f=" + fields, package], capture_output=True)
        binary, version, source_name, source_version = result.stdout.split("\t")
        inventory.append(dict(binary_package=binary, binary_version=version, source_package=source_name,
                              source_version=source_version, files=files))
    for name, record in sorted(rebuilt.items()):
        shutil.copy2(Path(own).parent / "share/contrail" / record["copyright"], target / record["copyright"])
        inventory.append(dict(binary_package=f"{name} (built by Contrail)", binary_version=record["source_version"],
                              source_package=record["source_package"], source_version=record["source_version"],
                              files=[f"lib/{name}"], built_by=record["built_by"],
                              configuration=record["configuration"]))
    copy_tree(Path("/usr/share/common-licenses"), target / "common-licenses")
    (target / "bundled-packages.json").write_text(json.dumps(inventory, indent=2) + "\n")
    print(f"Bundled files come from {len(inventory)} Debian packages")


def triple_arch(triple):
    return {"x86_64-linux-gnu": "amd64", "aarch64-linux-gnu": "arm64"}[triple]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["resources", "deploy", "check", "licenses"])
    parser.add_argument("root", type=Path)
    parser.add_argument("triple", choices=sorted(MACHINES))
    parser.add_argument("--host", type=Path, help="sonames left to the host or the Flatpak runtime")
    parser.add_argument("--gtk", action="store_true")
    parser.add_argument("--pactl", action="store_true")
    parser.add_argument("--own", type=Path, help="lib/ of build-deps.sh's prefix, its libraries come first")
    args = parser.parse_args()
    root = args.root.resolve()
    own = args.own.resolve() if args.own and args.own.is_dir() else None
    if args.action == "resources":
        resources(root, args.triple, args.gtk, args.pactl)
    elif args.action == "licenses":
        licenses(root, args.triple, own)
    else:
        if not args.host:
            parser.error("--host is required")
        if args.action == "deploy":
            deploy(root, args.triple, read_host(args.host), own)
        else:
            check(root, args.triple, read_host(args.host) - set(own_libraries(own)))


if __name__ == "__main__":
    main()
