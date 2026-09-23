#!/usr/bin/env python3
"""Downloads and extracts an archive that contains Xbox 360 shader binaries.

The goal of this script is to make the "paste a link and recompile" workflow usable
from GitHub Actions: it accepts the kind of links people actually share (direct file
links, Google Drive, MediaFire, GitHub release assets, local paths), extracts archives
including nested ones, and verifies that shader containers were actually found before
the recompiler is started.

Usage:
    fetch_shader_archive.py <url or path> <output directory> [options]

Options:
    --keep-archive           Keep the downloaded archive inside the output directory.
    --max-depth <n>          Maximum depth of nested archives to extract (default 2).
    --no-verify              Do not fail when no shader containers are found.

Exits with a non-zero status and a readable message when the archive cannot be
downloaded, extracted, or does not contain any shaders.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request
import zipfile

SHADER_CONTAINER_MAGIC = b"\x10\x2a\x11\x00"
SHADER_CONTAINER_MAGIC_VERTEX = b"\x10\x2a\x11\x01"
ARCHIVE_EXTENSIONS = (".zip", ".7z", ".rar", ".tar", ".gz", ".tgz", ".tar.gz")
NESTED_ARCHIVE_EXTENSIONS = (".zip", ".7z", ".rar", ".tar", ".gz", ".tgz")

USER_AGENT = "XenosRecomp-shader-fetcher/1.0 (+https://github.com/Player124413/XenosRecomp)"


class FetchError(Exception):
    pass


def log(message):
    print(message, flush=True)


def human_size(size):
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024 or unit == "TiB":
            if unit == "B":
                return "{} B".format(int(value))
            return "{:.1f} {}".format(value, unit)
        value /= 1024.0


def google_drive_url(url):
    """Turns a Google Drive share link into a direct download link, if it is one."""
    if "drive.google.com" not in url and "docs.google.com" not in url:
        return None

    file_id = None
    match = re.search(r"/file/d/([A-Za-z0-9_-]+)", url)
    if match:
        file_id = match.group(1)
    else:
        query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        if "id" in query:
            file_id = query["id"][0]

    if not file_id:
        return None

    return "https://drive.usercontent.google.com/download?id={}&export=download&confirm=t".format(file_id)


def mediafire_url(url):
    """Best effort lookup of the direct download link on a MediaFire page."""
    if "mediafire.com" not in url:
        return None

    try:
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=60) as response:
            page = response.read().decode("utf-8", "replace")
    except Exception as error:
        raise FetchError("Could not open the MediaFire page: {}".format(error))

    match = re.search(r'href="(https://download[^"]+)"', page)
    if not match:
        raise FetchError("Could not find a download link on the MediaFire page. "
                         "Please use a direct link to the archive instead.")

    return match.group(1)


def download(url, destination, retries=3):
    resolved = mediafire_url(url) or google_drive_url(url) or url

    log("Downloading {}".format(resolved))

    last_error = None
    for attempt in range(1, retries + 1):
        try:
            request = urllib.request.Request(resolved, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=120) as response:
                content_type = response.headers.get("Content-Type", "")
                length = response.headers.get("Content-Length")
                total = int(length) if length and length.isdigit() else 0

                written = 0
                with open(destination, "wb") as output:
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break

                        output.write(chunk)
                        written += len(chunk)

                        if total and written % (16 * 1024 * 1024) < 1024 * 1024:
                            log("  {} / {} ({:.0f}%)".format(human_size(written), human_size(total),
                                                             written * 100.0 / total))

            if "text/html" in content_type and not is_archive_file(destination):
                raise FetchError(
                    "The link returned a web page instead of an archive. Links to file sharing "
                    "landing pages are not supported, use a direct download link instead.")

            log("Downloaded {} ({})".format(human_size(os.path.getsize(destination)), os.path.basename(destination)))
            return destination
        except FetchError:
            raise
        except Exception as error:
            last_error = error
            log("  attempt {} failed: {}".format(attempt, error))
            if attempt != retries:
                time.sleep(2 * attempt)

    raise FetchError("Failed to download '{}': {}".format(url, last_error))


def is_archive_file(path):
    """Checks the file signature to tell archives apart from error pages."""
    with open(path, "rb") as f:
        header = f.read(8)

    if header.startswith(b"PK\x03\x04") or header.startswith(b"PK\x05\x06"):
        return True
    if header.startswith(b"7z\xbc\xaf\x27\x1c"):
        return True
    if header.startswith(b"Rar!"):
        return True
    if header.startswith(b"\x1f\x8b"):
        return True
    if len(header) >= 262 and header[257:262] == b"ustar":
        return True

    return False


def safe_join(directory, member):
    """Extracts member paths without allowing them to escape the output directory."""
    target = os.path.normpath(os.path.join(directory, member))
    if not target.startswith(os.path.normpath(directory) + os.sep):
        raise FetchError("Archive contains an unsafe path: {}".format(member))

    return target


def extract_zip(path, directory):
    with zipfile.ZipFile(path) as archive:
        for member in archive.infolist():
            safe_join(directory, member.filename)

        archive.extractall(directory)


def extract_with_7z(path, directory):
    seven_zip = shutil.which("7z") or shutil.which("7za") or shutil.which("7zr")
    if not seven_zip:
        raise FetchError("'{}' needs 7-Zip to be extracted, which is not available on this machine."
                         .format(os.path.basename(path)))

    result = subprocess.run([seven_zip, "x", "-y", "-o" + directory, path],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise FetchError("Failed to extract '{}' with 7-Zip:\n{}".format(path, result.stdout.decode("utf-8", "replace")))


def extract_tar(path, directory):
    with tarfile.open(path) as archive:
        for member in archive.getmembers():
            safe_join(directory, member.name)

        try:
            archive.extractall(directory, filter="data")
        except TypeError:
            # The filter argument was added in Python 3.12.
            archive.extractall(directory)


def extract_archive(path, directory, keep=True):
    os.makedirs(directory, exist_ok=True)
    log("Extracting {}".format(os.path.basename(path)))

    try:
        if zipfile.is_zipfile(path):
            extract_zip(path, directory)
        elif path.lower().endswith((".7z", ".rar")):
            extract_with_7z(path, directory)
        elif tarfile.is_tarfile(path):
            extract_tar(path, directory)
        else:
            raise FetchError("Unsupported archive format: {}".format(os.path.basename(path)))
    except FetchError:
        raise
    except Exception as error:
        raise FetchError("Failed to extract '{}': {}".format(os.path.basename(path), error))

    if not keep:
        os.remove(path)


def extract_nested_archives(directory, max_depth):
    """Extracts archives that are stored inside the downloaded archive."""
    for depth in range(max_depth):
        nested = []
        for root, _, files in os.walk(directory):
            for name in files:
                if name.lower().endswith(NESTED_ARCHIVE_EXTENSIONS):
                    nested.append(os.path.join(root, name))

        if not nested:
            return

        log("Extracting {} nested archive(s) (level {})".format(len(nested), depth + 1))
        for path in nested:
            target = path + "_extracted"
            extract_archive(path, target, keep=False)


def count_shader_containers(directory):
    """Counts shader containers by scanning for the container magic."""
    count = 0
    files_scanned = 0
    bytes_scanned = 0

    for root, _, files in os.walk(directory):
        for name in files:
            path = os.path.join(root, name)
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError:
                continue

            files_scanned += 1
            bytes_scanned += len(data)

            for magic in (SHADER_CONTAINER_MAGIC, SHADER_CONTAINER_MAGIC_VERTEX):
                count += data.count(magic)

    return count, files_scanned, bytes_scanned


def fetch(url_or_path, output_directory, keep_archive=True, max_depth=2, verify=True):
    os.makedirs(output_directory, exist_ok=True)

    local = os.path.isfile(url_or_path)
    if local:
        log("Using local archive {}".format(url_or_path))
        archive_path = os.path.join(output_directory, os.path.basename(url_or_path))
        if os.path.abspath(archive_path) != os.path.abspath(url_or_path):
            shutil.copyfile(url_or_path, archive_path)
    else:
        if not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", url_or_path):
            raise FetchError("'{}' is neither an existing file nor a link.".format(url_or_path))

        name = os.path.basename(urllib.parse.urlparse(url_or_path).path) or "shaders.archive"
        if not name.lower().endswith(ARCHIVE_EXTENSIONS):
            name += ".zip"

        archive_path = os.path.join(output_directory, name)
        download(url_or_path, archive_path)

    if not is_archive_file(archive_path):
        with open(archive_path, "rb") as f:
            header = f.read(512)

        raise FetchError("'{}' does not look like an archive (it starts with {!r}). "
                         "Make sure the link points directly to the archive file."
                         .format(os.path.basename(archive_path), header[:32]))

    extract_directory = os.path.join(output_directory, "extracted")
    extract_archive(archive_path, extract_directory, keep=True)
    extract_nested_archives(extract_directory, max_depth)

    count, files_scanned, bytes_scanned = count_shader_containers(extract_directory)
    log("Scanned {} file(s) ({}): found {} shader container(s)".format(
        files_scanned, human_size(bytes_scanned), count))

    if verify and count == 0:
        raise FetchError(
            "No shader containers were found in the archive. The archive should contain the "
            "game files with the compiled Xbox 360 shaders (for example the .ar archive or the "
            "extracted game data). If the files are stored inside another archive format, unpack "
            "them and share a plain .zip instead.")

    if not keep_archive and os.path.exists(archive_path):
        os.remove(archive_path)

    print("RESULT_DIR={}".format(extract_directory), flush=True)
    return extract_directory


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url", help="Direct link to the archive, a supported file sharing link, or a local path")
    parser.add_argument("output", help="Directory to download and extract into")
    parser.add_argument("--keep-archive", action="store_true", help="Keep the downloaded archive in the output directory")
    parser.add_argument("--max-depth", type=int, default=2, help="Maximum depth of nested archives to extract")
    parser.add_argument("--no-verify", action="store_true", help="Do not fail when no shader containers are found")
    args = parser.parse_args()

    try:
        fetch(args.url, args.output, keep_archive=args.keep_archive, max_depth=args.max_depth,
              verify=not args.no_verify)
    except FetchError as error:
        print("error: {}".format(error), file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        return 130

    return 0


if __name__ == "__main__":
    sys.exit(main())
