#!/usr/bin/env python3
"""What a pair has to be, checked once more immediately before it is published.

Two things, both of which a device's loader will decide again for itself, and both of which are
worth failing here instead of on somebody's phone:

- **The module names the target's release.** `vermagic` is what the loader compares, and these
  kernels run `CONFIG_MODULE_FORCE_LOAD=n`, so a module naming anything else is refused rather
  than merely discouraged. The pair job substitutes the release before building and asserts the
  result; this re-reads the file that is about to be committed.
- **The daemon is an AArch64 binary.** It is what the app downloads and runs on the device, so a
  wrong architecture is worth catching here, where the message is legible.

What is deliberately *not* checked is that the module appears verbatim inside the daemon. It does
not: the daemon carries it as a build asset rather than as a blob. The pairs running on devices
today fail that check too, which is how this was found.
"""

from __future__ import annotations

import argparse
import struct

ELF_MAGIC = b"\x7fELF"
EM_AARCH64 = 0xB7


def vermagic(module: str) -> str:
    """The release a module claims, from the `vermagic=` string it carries."""
    with open(module, "rb") as handle:
        blob = handle.read()
    marker = b"vermagic="
    start = blob.find(marker)
    if start < 0:
        return ""
    return blob[start + len(marker):].split(b" ")[0].decode("ascii", "replace")


def is_aarch64_elf(path: str) -> bool:
    with open(path, "rb") as handle:
        head = handle.read(20)
    if len(head) < 20 or head[:4] != ELF_MAGIC:
        return False
    # e_machine is the third half-word, little-endian for both ends of this pipeline.
    return struct.unpack("<H", head[18:20])[0] == EM_AARCH64


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", required=True)
    parser.add_argument("--daemon", required=True)
    parser.add_argument("--release", required=True)
    arguments = parser.parse_args()

    found = vermagic(arguments.module)
    print(f"module vermagic: {found or '(none)'}")
    if not found.startswith(arguments.release):
        raise SystemExit(f"the module claims {found!r}, which does not start with {arguments.release!r}")

    if not is_aarch64_elf(arguments.daemon):
        raise SystemExit(f"{arguments.daemon} is not an AArch64 ELF")

    print("daemon: an AArch64 ELF")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
