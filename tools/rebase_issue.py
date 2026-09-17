#!/usr/bin/env python3
"""File the issue a failed rebase deserves, once.

A new KernelSU release that the Samsung patch will not apply to is the one thing this pipeline
cannot do itself, and it is also the thing that stops every pair from being rebuilt. So it is
reported where a person will see it, with the rebase's own output rather than a summary of it,
and updated rather than reopened when the same release fails again the next night.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

UPGRADE_DOC = "docs/KERNELSU-3.3.0-UPGRADE.md"


def body(flavor: str, tag: str, upstream: str, previous: str, patch: str, log: str) -> str:
    return "\n".join(
        [
            f"The Samsung patch does not apply to `{tag}` of `{upstream}`.",
            "",
            f"The newest patch for this flavour (`{previous}`) was tried against the new tag and the",
            "hunks below did not land. Nothing was published: every pair in the feed is unchanged,",
            f"and no pair can be rebuilt against `{tag}` until a new patch exists at `{patch}`.",
            "",
            "This is the part of a KernelSU bump that is not mechanical. The five conflicts the",
            f"v3.3.0 rebase hit, and how each was resolved, are written up in `{UPGRADE_DOC}` - a",
            "release that touches the code the Samsung delta rewrites is the normal case, not a",
            "surprise.",
            "",
            "Once the patch is written and committed, this workflow rebuilds and republishes every",
            "pair on its next run. A manual dispatch runs it immediately.",
            "",
            f"<details><summary>git apply --check output for {flavor}</summary>",
            "",
            "```",
            log.strip()[:20000],
            "```",
            "",
            "</details>",
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="owner/name the issue belongs to")
    parser.add_argument("--flavor", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--upstream", required=True)
    parser.add_argument("--previous", required=True, help="the patch that was tried")
    parser.add_argument("--patch", required=True, help="the patch that has to be written")
    parser.add_argument("--log", required=True, help="file holding the apply output")
    parser.add_argument("--dry-run", action="store_true", help="print instead of filing")
    arguments = parser.parse_args()

    try:
        with open(arguments.log, encoding="utf-8", errors="replace") as handle:
            log = handle.read()
    except OSError:
        log = "(no output was captured)"

    text = body(arguments.flavor, arguments.tag, arguments.upstream, arguments.previous, arguments.patch, log)
    title = f"KernelSU {arguments.tag} needs a patch for {arguments.flavor}"

    if arguments.dry_run:
        print(f"# {title}\n\n{text}")
        return 0

    listed = subprocess.run(
        ["gh", "issue", "list", "--repo", arguments.repo, "--state", "open", "--search", f"{arguments.tag} in:title", "--limit", "20", "--json", "number,title"],
        capture_output=True,
        text=True,
        check=True,
    )
    existing = next((item for item in json.loads(listed.stdout) if item["title"] == title), None)

    handle = open("/tmp/rebase-issue.md", "w", encoding="utf-8", newline="\n")
    handle.write(text)
    handle.close()

    if existing:
        # Commented rather than ignored: a rebase that fails for a week would otherwise look
        # identical to one that failed once.
        subprocess.run(
            ["gh", "issue", "comment", str(existing["number"]), "--repo", arguments.repo, "--body-file", "/tmp/rebase-issue.md"],
            check=True,
        )
        print(f"Updated issue #{existing['number']}")
    else:
        subprocess.run(
            ["gh", "issue", "create", "--repo", arguments.repo, "--title", title, "--body-file", "/tmp/rebase-issue.md"],
            check=True,
        )
        print(f"Filed: {title}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
