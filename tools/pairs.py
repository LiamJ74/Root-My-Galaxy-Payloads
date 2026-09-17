#!/usr/bin/env python3
"""What a KernelSU version bump has to rebuild, derived from what is already published.

A pair is the kernel module plus the `ksud` that embeds it, and the feed entry that serves
them. Rebuilding one for a new KernelSU tag needs four things this repository already knows:
which target it is, which KMI it was built for, which flavour it belongs to, and the kernel
release its module has to claim.

The release is the only input that cannot be invented, because the pair job substitutes it
into the build and then asserts the module's `vermagic` starts with it. This script does not
have to invent it: the pair already published for that target carries it in its own
`vermagic`. Copying that value reproduces exactly the release users are running today, so a
regenerated pair claims what the working one claims, and nothing about the loader's view of
it changes.

Nothing here is maintained by hand. A device port that adds a pair and a feed entry is picked
up by the next run. What cannot be derived is reported rather than guessed at: an entry whose
daemon has no module beside it, and an entry whose module claims a build tree rather than a
device release (the DDK's own `-dirty` release, which the pair job refuses to publish because
substituting the device's release into it is what the build is for).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

# `6.6.98-android15-8-pd6ff1cd-abogkiS938USQSCCZF9-4k SMP preempt ...` -> the release is
# everything before the first space; the rest is the kernel's own configuration trailer.
VERMAGIC = re.compile(rb"vermagic=([^ \x00]+)")

# `<kmi>_kernelsu[-next]-<target>-kdp.ko`, plus the older names that stop at the target.
MODULE = re.compile(
    r"^(?P<kmi>android\d+-\d+\.\d+(?:\.\d+)?)_kernelsu(?P<suffix>-next)?-(?P<target>.+?)(?:-kdp)?\.ko$"
)
# `ksud[-next]-<target>-kdp`.
DAEMON = re.compile(r"^ksud(?P<suffix>-next)?-(?P<target>.+?)-kdp$")
# The version a payload id carries: `pa3q-S938USQSCCZF9-ksu330`.
VERSIONED_ID = re.compile(r"^(?P<prefix>.+?)-(?P<flavour>ksun?)(?P<version>\d+)$")
# A build tree's own release rather than a device's: `6.6.127-4k-g46a034eca005-dirty`.
BUILD_TREE = re.compile(r"-g[0-9a-f]{7,}(-dirty)?$")

FLAVOURS = {"": "kernelsu", "-next": "kernelsu-next"}
# `S938USQSCCZF9`: the build id in a payload id or a target id.
BUILD_ID = re.compile(r"^[a-z0-9]+-[A-Z][A-Z0-9]{6,}$")


def version_code(tag: str) -> str | None:
    """`v3.3.0` -> `330`, the form a payload id carries.

    KernelSU's own version code (32601 for 3.3.0) is a build counter the manager prints, not
    something the tag states, so an id uses the tag's own numbers. That is the only shape the
    published ids follow: `ksu330` for v3.3.0.
    """
    parts = re.findall(r"\d+", tag)
    if len(parts) < 2:
        return None
    major, minor, patch = (parts + ["0", "0"])[:3]
    return str(int(major) * 100 + int(minor) * 10 + int(patch))


RELEASE_TEXT = re.compile(r"\b(\d+\.\d+\.\d+-android\d+-[\w.-]+)")


def module_release(path: str) -> str | None:
    """The kernel release a built module claims, or None when it carries no vermagic."""
    try:
        with open(path, "rb") as handle:
            match = VERMAGIC.search(handle.read())
    except OSError:
        return None
    return match.group(1).decode("ascii", "replace") if match else None


def documented_release(repo: str, build: str) -> str | None:
    """The release a device port recorded, from `docs/<MODEL>-<BUILD>.md`.

    A port document states what the device reports, which is the one input a pair build cannot
    infer. It is used only where no module is published to copy the value from - that is, where
    the entries share a hand-built pair and a per-target one has to be built instead.
    """
    if not build:
        return None
    suffix = f"-{build}.md"
    for name in sorted(os.listdir(os.path.join(repo, "docs"))):
        if not name.endswith(suffix):
            continue
        with open(os.path.join(repo, "docs", name), encoding="utf-8", errors="replace") as handle:
            found = RELEASE_TEXT.search(handle.read())
        if found:
            return found.group(1)
    return None


def _url(entry: dict, key: str) -> str:
    value = entry.get(key)
    return value.get("url", "") if isinstance(value, dict) else ""


def plan(repo: str, feed: str = "support/targets-v3.json") -> dict:
    """Every pair the feed serves, and what a rebuild of it would use."""
    with open(os.path.join(repo, feed), encoding="utf-8") as handle:
        manifest = json.load(handle)

    artifacts = os.path.join(repo, "kernelsu")
    published: dict[tuple[str, str], dict] = {}
    skipped: list[dict] = []
    migrations: list[dict] = []
    entries: list[dict] = []

    for entry in manifest.get("payloads", []):
        payload_id = entry.get("payloadId", "")
        daemon_name = os.path.basename(_url(entry, "kernelsu"))
        daemon = DAEMON.match(daemon_name)
        # An optional group that did not participate is None rather than empty, so the suffix is
        # normalised before it is used as a key.
        suffix = (daemon.group("suffix") or "") if daemon else None
        flavour = FLAVOURS.get(suffix) if suffix is not None else None

        if daemon is None or flavour is None:
            skipped.append({"payloadId": payload_id, "artifact": daemon_name, "reason": "daemon is not named for a target"})
            continue

        target = daemon.group("target")
        entries.append({"payloadId": payload_id, "target": target, "flavor": flavour, "daemon": daemon_name})

        key = (target, flavour)
        if key in published:
            continue

        module_name = None
        kmi = None
        for name in sorted(os.listdir(artifacts)):
            module = MODULE.match(name)
            if not module or module.group("target") != target:
                continue
            if (module.group("suffix") or "") != suffix:
                continue
            kmi, module_name = module.group("kmi"), name
            break

        release = module_release(os.path.join(artifacts, module_name)) if module_name else None
        if release is not None and not BUILD_TREE.search(release):
            # The normal case: the pair already published carries the release to reproduce.
            published[key] = {
                "targetId": target,
                "kmi": kmi,
                "release": release,
                "flavor": flavour,
                "module": module_name,
                "daemon": daemon_name,
                "payloadIds": [],
            }
            continue

        # Nothing published to copy a release from - either there is no module beside the daemon,
        # or the only one is the build tree's own. Two things can still name the build: the
        # daemon's own target, or the payload id, which for a shared hand-built pair is
        # `<device>-<build>` and is therefore the only place that build is written down.
        # The version a payload id may carry is a suffix, not part of the build it names.
        without_version = re.sub(r"-(ksu|ksun)\d+$", "", payload_id)
        from_id = without_version if BUILD_ID.match(without_version) else ""
        candidate_target = target if "-" in target else from_id
        build = candidate_target.split("-", 1)[1] if "-" in candidate_target else ""
        documented = documented_release(repo, build)
        kmi_from_release = None
        if documented:
            android = re.search(r"-android(\d+)-", documented)
            version = documented.split("-")[0]
            if android:
                candidate = f"android{android.group(1)}-{'.'.join(version.split('.')[:2])}"
                if any(name.startswith(candidate + "_") for name in os.listdir(artifacts)):
                    kmi_from_release = candidate

        if documented and kmi_from_release:
            migrations.append(
                {
                    "targetId": candidate_target,
                    "kmi": kmi_from_release,
                    "release": documented,
                    "flavor": flavour,
                    # The daemon this entry has to move to, which the build produces: a shared pair
                    # cannot be replaced in place, because the artifact is the one four other
                    # entries are still served by.
                    "daemon": daemon_name,
                    "target_daemon": f"ksud{'-next' if suffix else ''}-{candidate_target}-kdp",
                    "payloadIds": [payload_id],
                    "module": None,
                    "source": "a device port document",
                }
            )
            continue

        if module_name is None:
            skipped.append({"payloadId": payload_id, "artifact": daemon_name, "reason": "no module beside the daemon"})
            continue
        if release is None:
            skipped.append({"payloadId": payload_id, "artifact": module_name, "reason": "module carries no vermagic"})
            continue
        skipped.append(
            {
                "payloadId": payload_id,
                "artifact": module_name,
                "reason": "a shared hand-built pair, and no device port document names its build"
                if not documented
                else f"a port document gives {documented}, which no published module builds",
            }
        )
        continue

    for item in entries:
        pair = published.get((item["target"], item["flavor"]))
        if pair is not None:
            pair["payloadIds"].append(item["payloadId"])

    def version_of(payload_id: str) -> str | None:
        match = VERSIONED_ID.match(payload_id)
        return match.group("version") if match else None

    for group in (published.values(), migrations):
        for pair in group:
            pair["versions"] = sorted({version for version in map(version_of, pair["payloadIds"]) if version})

    key = lambda item: (item["targetId"], item["flavor"])
    return {
        "pairs": sorted(published.values(), key=key),
        "migrations": sorted(migrations, key=key),
        "skipped": skipped,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="payload repository root")
    parser.add_argument("--feed", default="support/targets-v3.json")
    parser.add_argument("--json", action="store_true", help="print the whole plan as JSON")
    parser.add_argument("--github-matrix", metavar="FLAVOUR", help="print a build matrix for one flavour")
    parser.add_argument("--matrix", action="store_true", help="print the whole build matrix")
    parser.add_argument("--with-migrations", action="store_true", help="include entries ready for a pair of their own")
    parser.add_argument("--write", metavar="PATH", help="write the plan where the workflow can read it")
    arguments = parser.parse_args()

    derived = plan(arguments.repo, arguments.feed)

    if arguments.write:
        with open(arguments.write, "w", encoding="utf-8") as handle:
            json.dump(derived, handle, indent=2)
            handle.write("\n")
        print(f"wrote {arguments.write}: {len(derived['pairs'])} pairs, {len(derived['skipped'])} skipped")
        return 0

    if arguments.matrix:
        rows = []
        for pair in derived["pairs"] + (derived["migrations"] if arguments.with_migrations else []):
            rows.append(
                {
                    "targetId": pair["targetId"],
                    "kmi": pair["kmi"],
                    "release": pair["release"],
                    "flavor": pair["flavor"],
                    # Empty unless this run is moving the entries onto the new pair, because moving
                    # one changes which artifact a device downloads.
                    "migrate": ",".join(pair["payloadIds"]) if pair in derived["migrations"] else "",
                }
            )
        print(json.dumps(rows, separators=(",", ":")))
        return 0

    if arguments.github_matrix:
        rows = [
            {
                "target_id": pair["targetId"],
                "target_release": pair["release"],
                "kmi": pair["kmi"],
                "flavor": pair["flavor"],
                "daemon": pair["daemon"],
                "payload_ids": ",".join(pair["payloadIds"]),
            }
            for pair in derived["pairs"]
            if pair["flavor"] == arguments.github_matrix
        ]
        print(json.dumps(rows, separators=(",", ":")))
        return 0

    if arguments.json:
        print(json.dumps(derived, indent=2))
        return 0

    print(f"pairs the feed serves: {len(derived['pairs'])}")
    for pair in derived["pairs"]:
        served = len(pair["payloadIds"])
        print(
            f"  {pair['flavor']:14s} {pair['targetId']:24s} {pair['kmi']:18s} "
            f"{pair['release']:56s} serves {served} entr{'y' if served == 1 else 'ies'}"
        )
    if derived["migrations"]:
        print(f"ready for a pair of their own, once migrated: {len(derived['migrations'])}")
        for item in derived["migrations"]:
            print(
                f"  {', '.join(item['payloadIds']):34s} {item['kmi']:14s} {item['release']} "
                f"-> {item['target_daemon']} ({item['source']})"
            )
    if derived["skipped"]:
        print(f"not rebuildable from here: {len(derived['skipped'])}")
        for item in derived["skipped"]:
            print(f"  {item['payloadId']:34s} {item['reason']} ({item['artifact']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
