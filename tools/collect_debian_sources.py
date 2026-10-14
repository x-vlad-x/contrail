#!/usr/bin/env python3
"""Downloads the exact Debian source packages behind a package's Debian-built parts, verified with SHA-256 from
signed APT indexes. Never changes the host's APT configuration (a disposable build container is used).

  collect_debian_sources.py --container IMAGE [--snapshot 20260615T140756Z] --output DIR INVENTORY.json...

INVENTORY files are an AppImage's bundled-packages.json (every package it carries) and build-inputs.json
(packages compiled in: static archives and header-only libraries; see needed_sources()). With --snapshot,
sources come from snapshot.debian.org at that time (the FuriOS mirror has no sources); otherwise from the
container's own repositories.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
# Linux UAPI headers only declare the system call interface (GPL-2.0 WITH Linux-syscall-note): nothing of
# the kernel is compiled in.
NOT_COMPILED_IN = {"linux"}


def needed_sources(inventory):
    """Source packages whose code ends up inside the shipped files of one inventory."""
    if isinstance(inventory, list):  # bundled-packages.json: everything listed is shipped
        return {(p["source_package"], p["source_version"]) for p in inventory}
    by_binary = {p["binary_package"]: p for p in inventory["packages"]}
    source_of = lambda binary: (by_binary[binary]["source_package"], by_binary[binary]["source_version"])
    dynamic = {source_of(p) for packages in inventory["dynamic"].values() for p in packages}
    static = {source_of(p) for record in inventory["static"] for p in record.get("packages", [])}
    # Static parts of cross toolchain packages (crt objects, libgcc.a, libc_nonshared.a) were compiled from
    # the sources their Built-Using field names.
    static |= {(b["source_package"], b["source_version"]) for record in inventory["static"]
               for p in record.get("packages", []) for b in by_binary[p].get("built_using", [])}
    # Headers of a library the program links dynamically describe that library; other headers (header-only
    # libraries such as range-v3, GSL, expected) are compiled in.
    header_only = {source_of(p) for p in inventory["headers"]}
    header_only |= {(b["source_package"], b["source_version"]) for p in inventory["headers"]
                    for b in by_binary[p].get("built_using", [])}
    header_only -= dynamic
    return {s for s in static | header_only if s[0] not in NOT_COMPILED_IN}


def parse_uris(output):
    files = []
    for line in output.splitlines():
        if not line.startswith("'"):
            continue
        match = re.fullmatch(r"'([^']+)' (\S+) (\d+) SHA256:([0-9a-f]{64})", line)
        if not match:
            raise ValueError(f"APT did not provide a SHA-256 source record: {line}")
        url, name, size, digest = match.groups()
        if Path(name).name != name or name in {".", ".."}:
            raise ValueError(f"Unsafe source filename: {name}")
        files.append(dict(url=url.replace("http://", "https://", 1), name=name, size=int(size), sha256=digest))
    if not any(f["name"].endswith(".dsc") for f in files):
        raise ValueError("APT did not return a complete source package")
    return files


def index_sources(sources, destination, snapshot):
    """Runs as root inside the disposable container."""
    lists = Path("/etc/apt/sources.list.d")
    if snapshot:
        suite = re.search(r"^Suites: (\S+)", (lists / "debian.sources").read_text(), re.M).group(1)
        (lists / "snapshot-src.sources").write_text(
            f"Types: deb-src\nURIs: https://snapshot.debian.org/archive/debian/{snapshot}/\nSuites: {suite}\n"
            "Components: main contrib non-free\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n")
        Path("/etc/apt/apt.conf.d/99snapshot").write_text('Acquire::Check-Valid-Until "false";\n')
    else:
        for path in lists.glob("*.sources"):
            text = path.read_text()
            path.write_text(re.sub(r"^Types: deb$", "Types: deb deb-src", text, flags=re.M))
    subprocess.run(["apt-get", "update", "-qq"], check=True)
    records, errors = [], []
    for package, version in sorted(sources):
        result = subprocess.run(["apt-get", "--print-uris", "--download-only", "--only-source",
                                 "-o", "Acquire::ForceHash=SHA256", "source", f"{package}={version}"],
                                text=True, capture_output=True, cwd="/tmp")
        try:
            if result.returncode:
                raise ValueError(result.stderr.strip())
            records.append(dict(package=package, version=version, files=parse_uris(result.stdout)))
        except ValueError as error:
            errors.append(dict(package=package, version=version, error=str(error)))
    destination.write_text(json.dumps(dict(sources=records, errors=errors), indent=2) + "\n")
    if errors:
        raise RuntimeError(f"Exact source versions unavailable: {errors}")


def valid(path, record):
    if not path.is_file() or path.stat().st_size != record["size"]:
        return False
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() == record["sha256"]


def fetch_file(record, directory):
    target = directory / record["name"]
    if valid(target, record):
        return
    temporary = target.with_name(target.name + ".download")
    with urllib.request.urlopen(record["url"], timeout=120) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output, 1024 * 1024)
    if not valid(temporary, record):
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"Source checksum mismatch: {record['name']}")
    temporary.replace(target)


def verify_dsc(source, directory):
    records = {record["name"]: record for record in source["files"]}
    dsc = next(name for name in records if name.endswith(".dsc"))
    text = (directory / dsc).read_text(errors="replace")
    version = re.search(r"^Version: (.+)$", text, re.M)
    if not version or version.group(1) != source["version"]:
        raise ValueError(f"Wrong source version inside {dsc}")
    checksums = re.search(r"^Checksums-Sha256:\n((?: [^\n]+\n)+)", text, re.M)
    if not checksums:
        raise ValueError(f"Source package has no SHA-256 file list: {dsc}")
    names = set()
    for line in checksums.group(1).splitlines():
        digest, size, name = line.split()
        record = records.get(name)
        if not record or (digest, int(size)) != (record["sha256"], record["size"]):
            raise ValueError(f"Incomplete or conflicting source package: {dsc}: {name}")
        names.add(name)
    if names != set(records) - {dsc}:
        raise ValueError(f"Unexpected files in source package: {dsc}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inventory", type=Path, nargs="+")
    parser.add_argument("--container", required=True)
    parser.add_argument("--snapshot", help="snapshot.debian.org timestamp, e.g. 20260615T140756Z")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        index_sources({tuple(s) for s in json.loads(args.worker.read_text())}, args.output, args.snapshot)
        return
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    sources = set()
    for inventory in args.inventory:
        sources |= needed_sources(json.loads(inventory.read_text()))
    wanted = directory / "wanted.json"
    wanted.write_text(json.dumps(sorted(sources), indent=1) + "\n")
    inside = lambda p: str(Path("/src") / Path(p).resolve().relative_to(ROOT))
    engine = shutil.which("podman") or shutil.which("docker")
    command = [engine, "run", "--rm", "-v", f"{ROOT}:/src:z", "-w", "/src", args.container, "python3",
               inside(Path(__file__)), "--container", args.container, "--worker", inside(wanted),
               "--output", inside(directory / "sources.json"), "unused"]
    if args.snapshot:
        command[-1:-1] = ["--snapshot", args.snapshot]
    subprocess.run(command, check=True)
    metadata = json.loads((directory / "sources.json").read_text())
    files = {}
    for source in metadata["sources"]:
        for record in source["files"]:
            previous = files.setdefault(record["name"], record)
            if previous["sha256"] != record["sha256"]:
                raise RuntimeError(f"Conflicting source files: {record['name']}")
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda f: fetch_file(f, directory), files.values()))
    for source in metadata["sources"]:
        verify_dsc(source, directory)
    (directory / "SHA256SUMS").write_text("".join(
        f"{record['sha256']}  {name}\n" for name, record in sorted(files.items())))
    total = sum(r["size"] for r in files.values())
    print(f"Verified {len(files)} files ({total / 2**20:.0f} MiB) for {len(metadata['sources'])} source packages")


if __name__ == "__main__":
    main()
