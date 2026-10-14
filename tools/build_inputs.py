#!/usr/bin/env python3
"""Lists the Debian packages a finished build used, from the build's own records (run in the build container).

  build_inputs.py BUILD_DIR PROGRAM OUTPUT.json

- headers: every header the compiler read (ninja's dependency log), by package;
- static: archives and objects linked into PROGRAM (the link command, the compiler's runtime objects);
- dynamic: the shared libraries PROGRAM links (ELF NEEDED), by package.
Packages are recorded with their source package and version, so their sources can be fetched exactly.
"""
from pathlib import Path
import json
import re
import shlex
import subprocess
import sys


def dpkg_index():
    owners = {}
    for listing in Path("/var/lib/dpkg/info").glob("*.list"):
        package = listing.name.removesuffix(".list")
        for name in listing.read_text().splitlines():
            owners.setdefault(name, set()).add(package)
    return owners


def owner(owners, path):
    path = Path(path)
    for candidate in (path, path.resolve()):
        found = owners.get(str(candidate))
        if found:
            return found
        # /lib is a symlink to /usr/lib on merged-/usr systems; dpkg may list either.
        text = str(candidate)
        alternative = text.removeprefix("/usr") if text.startswith("/usr/lib") else "/usr" + text
        found = owners.get(alternative)
        if found:
            return found
    return set()


def describe(packages):
    if not packages:
        return []
    fields = "${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\t${Built-Using}\n"
    output = subprocess.run(["dpkg-query", "-W", "-f=" + fields, *sorted(packages)], check=True,
                            capture_output=True, text=True).stdout
    result = []
    for line in output.splitlines():
        binary, version, source, source_version, built_using = line.split("\t")
        record = dict(binary_package=binary, binary_version=version, source_package=source,
                      source_version=source_version)
        # Cross toolchain packages are rebuilt from other sources (glibc, gcc): Built-Using names them.
        if built_using:
            record["built_using"] = [dict(source_package=name, source_version=version)
                                     for name, version in re.findall(r"([a-z0-9][a-z0-9+.-]*) \(= ([^)]+)\)", built_using)]
        result.append(record)
    return result


def main():
    build, program, output = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    owners = dpkg_index()
    unowned = set()

    def deps(*target):
        output = subprocess.run(["ninja", "-C", build, "-t", "deps", *target], check=True, capture_output=True,
                                text=True).stdout
        return [line.strip() for line in output.splitlines() if line.startswith("    ")]

    headers = {}
    for path in deps():
        # Only what the compiler read: headers (C++ standard headers have no suffix), not the CMake files,
        # programs and libraries the log also lists.
        if not path.startswith("/") or path.startswith(("/src/", str(build.resolve()))) or \
                not ("/include/" in path or re.search(r"\.(h|hh|hpp|hxx|inl|ipp|tcc)$", path)):
            continue
        packages = owner(owners, path)
        if not packages:
            unowned.add(path)
        for package in packages:
            headers[package] = headers.get(package, 0) + 1

    # The linker's own record of its inputs (ninja keeps it from -Wl,--dependency-file).
    linked = deps("bin/Telegram")
    if not linked:
        raise RuntimeError("The build has no record of the program's link inputs")
    static_records = []
    for path in sorted(set(linked)):
        if not path.startswith("/") or not re.search(r"\.(a|o)$", path):
            continue
        if path.startswith("/src/"):
            static_records.append(dict(file=path, origin="built by build-deps.sh (see static-libraries)"))
            continue
        packages = owner(owners, path)
        if not packages:
            unowned.add(path)
        static_records.append(dict(file=path, packages=sorted(packages)))
    command = subprocess.run(["ninja", "-C", build, "-t", "commands", "bin/Telegram"], check=True,
                             capture_output=True, text=True).stdout.splitlines()[-1]
    compiler = next(word for word in shlex.split(command) if re.search(r"(g\+\+|c\+\+|clang\+\+)$", word))

    triple = subprocess.run([compiler, "-dumpmachine"], check=True, capture_output=True, text=True).stdout.strip()
    needed = re.findall(r"\(NEEDED\).*\[(.+)\]", subprocess.run([f"{triple}-readelf", "-d", program], check=True,
                                                                   capture_output=True, text=True).stdout)
    dynamic = {}
    for name in needed:
        candidates = [Path(d) / name for d in (f"/usr/lib/{triple}", f"/lib/{triple}", "/usr/lib", "/lib")]
        path = next((c for c in candidates if c.exists()), None)
        packages = owner(owners, path) if path else set()
        if not packages:
            raise RuntimeError(f"Linked library without a package: {name}")
        dynamic[name] = sorted(packages)

    all_packages = set(headers) | {p for r in static_records for p in r.get("packages", [])} | \
        {p for ps in dynamic.values() for p in ps}
    result = dict(
        compiler=compiler,
        packages=describe(all_packages),
        headers={package: count for package, count in sorted(headers.items())},
        static=static_records,
        dynamic=dynamic,
        unowned=sorted(unowned),
    )
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{len(result['packages'])} packages; {len(headers)} provided headers, {len(static_records)} static inputs, "
          f"{len(dynamic)} shared libraries; {len(unowned)} files without a package")


if __name__ == "__main__":
    main()
