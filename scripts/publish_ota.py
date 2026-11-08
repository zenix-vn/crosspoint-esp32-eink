#!/usr/bin/env python3
"""Build the Zenix firmware for each reader and publish it to the self-hosted OTA feed.

The device's OTA updater (src/network/OtaUpdater.cpp) polls
`[crosspoint] ota_feed_url` for GitHub-release-shaped JSON and installs
`crosspoint-<tag>-<board>.bin` when <tag>'s X.Y.Z is higher than its own
version. One release carries an image per board (BOARDS):

    firmware/crosspoint-<version>-zenix-x4pro.bin    X4 Pro
    firmware/crosspoint-<version>-zenix-x4c.bin      X4 Classic
    firmware/crosspoint-<version>-zenix-x3-x4.bin    X3 and the original X4 (one ESP32-C3 image)
    ... and -metalio_eink4, -picco, -lilygo: Metalio E-Ink 4, BOOX Picco, LilyGo T5 S3
    firmware/latest.json        stable channel (beta.json for --channel beta),
                                 written last, so the feed never points at a
                                 file that is not there yet

A stable release also moves beta.json forward when beta is behind it, so the
beta channel is never older than stable. Publishing some boards under the tag
already live keeps the other boards' assets, so a board can join a release
after the fact. Release notes go in the manifest's "body" and are shown on the
device before it installs.

Usage:
    python scripts/publish_ota.py --bump --notes "Fix X; add Y"     # stable, all boards
    python scripts/publish_ota.py --bump --channel beta --notes-file notes.txt
    python scripts/publish_ota.py --boards x4c,x3-x4                 # add boards to the live release
    python scripts/publish_ota.py --skip-build                      # republish the existing builds
    python scripts/publish_ota.py --dry-run                         # everything except the upload
"""

import argparse
import configparser
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

# Asset suffix -> PlatformIO env. The suffix is what OtaUpdater.cpp asks for.
BOARDS = {"x4pro": "x4pro", "x4c": "x4c", "x3-x4": "x3x4", "metalio_eink4": "metalio_eink4",
          "picco": "picco", "lilygo": "lilygo"}
VERSION_SUFFIX = "-zenix"
SSH_HOST = "zenix-server"  # ~/.ssh/config alias for 107.155.65.224
SSH_PORT = "8430"
REMOTE_DIR = "/usr/local/lsws/crosspoint.zenix.vn/html/firmware"
KEEP_RELEASES = 8

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INI = os.path.join(ROOT, "platformio.ini")


def read_settings():
    config = configparser.ConfigParser(interpolation=None)
    config.read(INI, encoding="utf-8")
    version = config.get("crosspoint", "zenix_version")
    feed_url = config.get("crosspoint", "ota_feed_url")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit(f"zenix_version must be X.Y.Z, got {version!r}")
    return version, feed_url


def bump_patch(version):
    major, minor, patch = (int(p) for p in version.split("."))
    new_version = f"{major}.{minor}.{patch + 1}"
    with open(INI, encoding="utf-8", newline="") as f:
        text = f.read()
    text, count = re.subn(r"(?m)^zenix_version = .*$", f"zenix_version = {new_version}", text)
    if count != 1:
        sys.exit("could not find a single 'zenix_version = ' line in platformio.ini")
    with open(INI, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"Bumped zenix_version {version} -> {new_version}")
    return new_version


def find_pio():
    found = shutil.which("pio")
    if found:
        return found
    for name in ("pio.exe", "pio"):
        candidate = os.path.join(os.path.expanduser("~"), ".platformio", "penv",
                                 "Scripts" if os.name == "nt" else "bin", name)
        if os.path.exists(candidate):
            return candidate
    sys.exit("PlatformIO (pio) not found")


def version_key(tag):
    match = re.match(r"(\d+)\.(\d+)\.(\d+)", tag or "")
    return tuple(int(x) for x in match.groups()) if match else (-1, -1, -1)


def fetch_json(url):
    try:
        with urllib.request.urlopen(url, timeout=20) as response:
            return json.load(response)
    except Exception:  # missing feed file on a first release
        return None


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    # PlatformIO prints Unicode; on Windows a redirected stdout defaults to
    # cp1252 and the build dies with UnicodeEncodeError mid-way.
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
    subprocess.run(cmd, check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bump", action="store_true", help="increment the patch version before building")
    parser.add_argument("--skip-build", action="store_true", help="publish the existing .pio builds")
    parser.add_argument("--dry-run", action="store_true", help="build and stage, but do not upload")
    parser.add_argument("--channel", choices=("stable", "beta"), default="stable", help="feed to publish to")
    parser.add_argument("--boards", default=",".join(BOARDS),
                        help=f"comma-separated boards to build and publish (default: {','.join(BOARDS)})")
    notes = parser.add_mutually_exclusive_group()
    notes.add_argument("--notes", help="release notes shown on the device")
    notes.add_argument("--notes-file", help="file holding the release notes")
    args = parser.parse_args()

    body = args.notes or ""
    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as f:
            body = f.read().strip()
    if len(body.encode("utf-8")) > 590:
        print("warning: the device shows only the first ~590 bytes of the notes")

    boards = [b.strip() for b in args.boards.split(",") if b.strip()]
    unknown = [b for b in boards if b not in BOARDS]
    if unknown or not boards:
        sys.exit(f"unknown board(s) {unknown}; choose from {', '.join(BOARDS)}")

    version, feed_url = read_settings()
    if args.bump:
        if args.skip_build:
            sys.exit("--bump needs a build; drop --skip-build")
        version = bump_patch(version)

    tag = version + VERSION_SUFFIX
    images = {}  # asset name -> image bytes
    for board in boards:
        firmware = os.path.join(ROOT, ".pio", "build", BOARDS[board], "firmware.bin")
        if not args.skip_build:
            run([find_pio(), "run", "-e", BOARDS[board]])
        if not os.path.exists(firmware):
            sys.exit(f"missing {firmware}")
        with open(firmware, "rb") as f:
            data = f.read()
        # The version is baked into the image; a stale build would publish under the
        # wrong tag and the device would install it forever after.
        if tag.encode() not in data:
            sys.exit(f"{firmware} does not contain version {tag!r}; rebuild without --skip-build")
        images[f"crosspoint-{tag}-{board}.bin"] = data

    base_url = feed_url.rsplit("/", 1)[0]
    new_assets = [{
        "name": name,
        "browser_download_url": f"{base_url}/{name}",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    } for name, data in images.items()]

    feeds = ["beta.json"] if args.channel == "beta" else ["latest.json"]
    if args.channel == "stable":
        beta = fetch_json(f"{base_url}/beta.json")
        if beta is None or version_key(beta.get("tag_name")) < version_key(tag):
            feeds.append("beta.json")
    # The same tag already live: keep the boards this run does not rebuild.
    live = fetch_json(f"{base_url}/{feeds[0]}") or {}
    same_tag = live.get("tag_name") == tag
    kept = [a for a in live.get("assets", []) if same_tag and a.get("name") not in images]
    manifest = {
        # tag_name must precede assets: the device picks the asset name from it.
        "tag_name": tag,
        "name": f"CrossPoint {tag}",
        "published_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "body": body or (live.get("body", "") if same_tag else ""),
        "assets": kept + new_assets,
    }

    stage = tempfile.mkdtemp(prefix="crosspoint-ota-")
    for name, data in images.items():
        with open(os.path.join(stage, name), "wb") as f:
            f.write(data)
    for feed in feeds:
        with open(os.path.join(stage, feed), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"Staged {', '.join(images)} and {', '.join(feeds)} in {stage}")

    if args.dry_run:
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return

    ssh = ["ssh", "-p", SSH_PORT, "-o", "BatchMode=yes", SSH_HOST]
    scp = ["scp", "-P", SSH_PORT, "-o", "BatchMode=yes"]
    run(ssh + [f"mkdir -p {REMOTE_DIR}"])
    for name in images:
        run(scp + [os.path.join(stage, name), f"{SSH_HOST}:{REMOTE_DIR}/{name}.part"])
        run(ssh + [f"mv -f {REMOTE_DIR}/{name}.part {REMOTE_DIR}/{name}"])
    for feed in feeds:
        run(scp + [os.path.join(stage, feed), f"{SSH_HOST}:{REMOTE_DIR}/{feed}.part"])
        run(ssh + [f"mv -f {REMOTE_DIR}/{feed}.part {REMOTE_DIR}/{feed}"])
    # Keep the newest few images of each board for rollback; drop the rest.
    prune = " && ".join(f"(ls -1t {REMOTE_DIR}/crosspoint-*-{board}.bin | tail -n +{KEEP_RELEASES + 1} | xargs -r rm -f)"
                        for board in boards)
    run(ssh + [f"chown nobody:nobody {REMOTE_DIR}/* && {prune}"])
    shutil.rmtree(stage, ignore_errors=True)

    for feed in feeds:
        served = fetch_json(f"{base_url}/{feed}") or {}
        if served.get("tag_name") != tag:
            sys.exit(f"{feed} still reports {served.get('tag_name')!r}, expected {tag!r}")
    for asset in new_assets:
        request = urllib.request.Request(asset["browser_download_url"], method="HEAD")
        with urllib.request.urlopen(request, timeout=20) as response:
            length = int(response.headers.get("Content-Length", "-1"))
        if length != asset["size"]:
            sys.exit(f"served {asset['name']} is {length} bytes, expected {asset['size']}")
    print(f"Published {tag} ({', '.join(boards)}) to {', '.join(feeds)}: {base_url}/")


if __name__ == "__main__":
    main()
