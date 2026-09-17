#!/usr/bin/env python3
"""Point the feed at a pair that has just been built.

The last manual step of a KernelSU bump is editing `support/targets-v3.json`: the new daemon's
size and digest, and the version its payload id and display name carry. That is mechanical, so
this does it, and refuses rather than guesses when the entry it would edit is ambiguous.

Two shapes of change, and the difference matters:

- **Republish**: the entry already names the daemon that was rebuilt (`ksud-<target>-kdp`), so
  its URL keeps its directory and only the file changes - plus the size and digest.
- **Migrate**: several entries can be served by one hand-built pair (`ksud-s25u-kdp`). Those
  cannot be republished in place, because that artifact is what the other entries are still
  served by; each gets a pair of its own instead, and its URL moves with it. This only happens
  when the target and its release were derived from a device port document, and the entry is
  named for exactly one build.

A URL is never rewritten from scratch: the existing one supplies its own prefix, which is how
the app's allowed-repository rule is satisfied, and the file name is the only part replaced.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pairs import VERSIONED_ID, version_code  # noqa: E402  (the tools directory is the module path)


def _suffix_of(payload_id: str) -> tuple[str, str] | None:
    """`pa3q-S938USQSCCZF9-ksu330` -> (`ksu`, `330`)."""
    match = VERSIONED_ID.match(payload_id)
    return (match.group("flavour"), match.group("version")) if match else None


def _version_text(code: str) -> str | None:
    """`330` -> `3.3.0`, for the display name that spells the version out."""
    if len(code) < 2:
        return None
    digits = code if len(code) >= 3 else code + "0"
    major, minor, patch = digits[-3], digits[-2], digits[-1]
    return f"{int(digits[:-2])}.{minor}.{patch}" if len(digits) > 3 else f"{major}.{minor}.{patch}"


def _digest(path: str) -> tuple[int, str]:
    import hashlib

    size = os.path.getsize(path)
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return size, digest.hexdigest()


def _entry_spans(text: str) -> list[tuple[int, int, dict]]:
    """Every entry in the file, with the byte range it occupies.

    The file is edited as text rather than re-serialised. A round trip through `json.dumps`
    reformats it - short arrays get exploded onto their own lines - and a feed that is
    installed from deserves a commit whose diff shows the two lines that changed.
    """
    decoder = json.JSONDecoder()
    position = text.index("[", text.index('"payloads"')) + 1
    spans: list[tuple[int, int, dict]] = []
    while True:
        while text[position] in " \t\r\n,":
            position += 1
        if text[position] == "]":
            return spans
        entry, end = decoder.raw_decode(text, position)
        spans.append((position, end, entry))
        position = end


def _rewrite_entry(body: str, entry: dict, daemon: str, size: int, sha256: str, url_prefix: str | None) -> str:
    """One entry, with only the values that changed replaced."""
    artifact = entry["kernelsu"]
    old_url = artifact["url"]
    names = [old_url.rsplit("/", 1)[-1], daemon]

    if url_prefix:
        new_url = f"{url_prefix.rstrip('/')}/kernelsu/{daemon}"
    else:
        new_url = old_url.replace(names[0], daemon)

    # The artifact block, so a size or digest belonging to the exploit is never touched.
    block = body.index('"kernelsu"')
    head, tail = body[:block], body[block:]
    tail = tail.replace(f'"url": "{old_url}"', f'"url": "{new_url}"', 1)
    tail = re.sub(r'("size":\s*)' + str(artifact["size"]), r"\g<1>" + str(size), tail, count=1)
    if '"sha256"' in tail:
        tail = re.sub(r'("sha256":\s*")[0-9a-f]*(")', r"\g<1>" + sha256 + r"\g<2>", tail, count=1)
    else:
        # Written on its own line at the same indentation as the size it follows. The size carries
        # a comma only when something comes after it, and the digest is written last here, so both
        # shapes are handled rather than assumed.
        digest = tail
        match = re.search(r'(?P<line>\n(?P<indent>\s*)"size":\s*' + str(size) + r')(?P<comma>,?)', digest)
        if match:
            indent = match.group("indent")
            line = match.group("line")
            if match.group("comma"):
                digest = digest.replace(
                    line + ",",
                    line + f',\n{indent}"sha256": "{sha256}",',
                    1,
                )
            else:
                digest = digest.replace(
                    line,
                    line + f',\n{indent}"sha256": "{sha256}"',
                    1,
                )
        tail = digest
    return head + tail


def apply(
    repo: str,
    feed: str,
    daemon: str,
    size: int,
    sha256: str,
    version: str | None,
    payload_ids: list[str],
    migrate: bool,
    dry_run: bool,
    url_prefix: str | None = None,
) -> list[dict]:
    path = os.path.join(repo, feed)
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    manifest = json.loads(text)

    targets = set(payload_ids)
    changes: list[dict] = []
    rebuilt: list[str] = []
    cursor = 0

    for start, end, entry in _entry_spans(text):
        artifact = entry.get("kernelsu")
        if not isinstance(artifact, dict):
            continue
        url = artifact.get("url", "")
        current = os.path.basename(url)

        republish = current == daemon
        # Moving off a shared pair: only where the entry is named for the build it serves,
        # because that is the only reason to believe the derived release belongs to it.
        if not republish and not (migrate and entry.get("payloadId") in targets):
            continue

        body = _rewrite_entry(text[start:end], entry, daemon, size, sha256, url_prefix)
        payload_id = entry.get("payloadId", "")
        after_id = payload_id
        display = entry.get("displayName", "")
        after_display = display

        suffix = _suffix_of(payload_id)
        if suffix and version:
            _flavour, old_code = suffix
            new_code = version_code(version)
            if new_code and new_code != old_code:
                after_id = f"{payload_id[: -len(old_code)]}{new_code}"
                body = body.replace(f'"payloadId": "{payload_id}"', f'"payloadId": "{after_id}"', 1)
                old_text = _version_text(old_code)
                if old_text and old_text in display:
                    after_display = display.replace(old_text, version.lstrip("vV"))
                    body = body.replace(f'"displayName": "{display}"', f'"displayName": "{after_display}"', 1)

        rebuilt.append(text[cursor:start])
        rebuilt.append(body)
        cursor = end
        changes.append(
            {
                "before": {"payloadId": payload_id, "url": url, "size": artifact.get("size")},
                "after": {
                    "payloadId": after_id,
                    "displayName": after_display,
                    "url": f"{url_prefix.rstrip('/')}/kernelsu/{daemon}" if url_prefix else url.replace(current, daemon),
                    "size": size,
                },
            }
        )

    if not changes:
        raise SystemExit(f"nothing in {feed} serves {daemon}")

    rebuilt.append(text[cursor:])
    updated = "".join(rebuilt)
    result = json.loads(updated)

    # Two entries for one model, kernel version and flavour both match a run, and only the first
    # is used - so a duplicate is a feed that cannot say which pair it means.
    seen: dict[tuple, str] = {}
    for entry in result.get("payloads", []):
        key = (
            entry.get("flavor", "kernelsu"),
            tuple(entry.get("models", [])),
            tuple(entry.get("kernelVersions", [])),
        )
        other = seen.get(key)
        if other is not None:
            raise SystemExit(f"{entry.get('payloadId')} and {other} would both match the same devices")
        seen[key] = entry.get("payloadId", "")

    # Every artifact the app will ask for has to be reachable, and a relayed value is the one
    # mistake that would only show up as a failed run on a user's phone.
    for entry in result.get("payloads", []):
        artifact = entry["kernelsu"]
        if os.path.basename(artifact["url"]) == daemon and artifact["size"] != size:
            raise SystemExit(f"{entry.get('payloadId')} still declares size {artifact['size']}")

    if not dry_run:
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(updated)

    return changes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="payload repository root")
    parser.add_argument("--feed", default="support/targets-v3.json")
    parser.add_argument("--daemon", required=True, help="file name of the daemon that was built")
    parser.add_argument("--artifact", help="path to the built daemon, to read its size and digest")
    parser.add_argument("--size", type=int)
    parser.add_argument("--sha256")
    parser.add_argument("--version", help="the KernelSU tag the pair was built from, e.g. v3.4.0")
    parser.add_argument("--payload-id", action="append", default=[], help="entries to migrate")
    parser.add_argument("--migrate", action="store_true", help="move named entries onto a new daemon")
    parser.add_argument("--url-prefix", help="repository raw-URL prefix this run publishes under")
    parser.add_argument("--dry-run", action="store_true")
    arguments = parser.parse_args()

    size, sha256 = arguments.size, arguments.sha256
    if arguments.artifact:
        size, sha256 = _digest(arguments.artifact)
    if size is None or sha256 is None:
        parser.error("give --artifact, or both --size and --sha256")

    changes = apply(
        repo=arguments.repo,
        feed=arguments.feed,
        daemon=arguments.daemon,
        size=size,
        sha256=sha256,
        version=arguments.version,
        payload_ids=arguments.payload_id,
        migrate=arguments.migrate,
        dry_run=arguments.dry_run,
        url_prefix=arguments.url_prefix,
    )

    for change in changes:
        before, after = change["before"], change["after"]
        print(f"{before['payloadId']} -> {after['payloadId']}")
        print(f"  url  {before['url']}")
        print(f"   ->  {after['url']}")
        print(f"  size {before['size']} -> {after['size']}")
        if before["payloadId"] != after["payloadId"]:
            print(f"  name {after['displayName']}")
    print(f"{len(changes)} feed entr{'y' if len(changes) == 1 else 'ies'} updated{' (dry run)' if arguments.dry_run else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
