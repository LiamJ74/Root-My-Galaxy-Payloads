# KernelSU v3.3.0 upgrade

**Status: rebased and verified as far as this machine allows; not compiled, not device-tested.**

The Samsung patch has been re-derived against the `v3.3.0` tag, the five conflicting hunks resolved by
hand, and the result checked against a pristine checkout. It has **not** been through a compiler here:

- the kernel module needs the DDK container from [`../kernelsu/README.md`](../kernelsu/README.md), and
  there is no Docker daemon on this machine (confirmed by `docker info`);
- `cargo check` for `aarch64-linux-android` gets all the way to `ksud`'s own build script and stops
  there, because bindgen needs a `libclang.dll` this machine does not have — not in either installed
  NDK, and nothing else on the box provides one.

The published 3.2.5 pairs in [`../kernelsu/`](../kernelsu/) remain the device-tested ones. Everything
below is *the delta to carry forward*, not an artifact to publish.

Verified on this machine:

| claim | how |
| --- | --- |
| the patch applies clean to `v3.3.0` | `git apply --check` = 0 on a fresh `--depth 1 --branch v3.3.0` clone, all 15 files |
| it does not apply to `v3.2.5` | rejects on `kernel/Kbuild:64` and `kernel/core/init.c:26` — it is genuinely a v3.3.0 patch |
| the old patch still applies to `v3.2.5` | sanity check on the pair being published today |
| the dependency graph resolves | `cargo metadata --locked` = 0 after the KernelSU2 rewrite; `--locked` means the lockfile needed no change |
| the Samsung delta did not drift | the four Samsung-authored files are byte-identical between the two patches |
| the Rust calls agree | arities and imports checked at every call site (see below) |

## The dependency breakage, and the fix to use

Building `ksud` at all currently needs a dependency migration, and **it is not ours** — it is a
consequence of the `Kernel-SU` GitHub organisation being suspended. Four git dependencies in the
`v3.3.0` lockfile point at it:

```
git+https://github.com/Kernel-SU/adb_client#d97a966435bebaa55017834869dec08150826aa7
git+https://github.com/Kernel-SU/java-properties.git?branch=master#42a4aa941b70ded2dd3be9e9f892471023e70229
git+https://github.com/Kernel-SU/ksu_props?rev=6f5723105d8d4cacad31d83d343defbf032c7b33
git+https://github.com/Kernel-SU/rustix.git?rev=4a53fbc#4a53fbc7cb7a07cabe87125cc21dbc27db316259
```

Every one 404s. **Use upstream's own fix**: [`tiann/KernelSU#3723`](https://github.com/tiann/KernelSU/pull/3723)
("build: migrate dependencies to new organization", commit `cf2f620d`, by 5ec1cff), which repoints all
four at the successor org **`KernelSU2`**. That org is the official continuation — 15 repos, including
`adb_client`, `java-properties`, `ksu_props`, `rustix`, `ksuinit`, `AnyKernel3`, `binder_rs` — and it
carries all four revs we pin. The PR is still a **draft against `main`**, so the tag we build from does
not contain it and the rewrite has to be applied as a build step rather than inherited.

The rewrite, on the four manifests plus the lockfile:

```sh
sed -i 's#github\.com/Kernel-SU/#github.com/KernelSU2/#g' \
  Cargo.toml Cargo.lock userspace/ksud/Cargo.toml userspace/ksuinit/Cargo.toml
```

**Do not copy `main`'s revs while you do it.** #3723 is written against `main`, which pins
`prop-rs-android` to `ddb6ee72`; the `v3.3.0` tag pins `6f57231`, and that is the one our lockfile and
our patch are consistent with. Bump the rev only as its own decision. Every rev in the table above was
confirmed present in `KernelSU2` before writing this.

Two other git dependencies in the lock — `5ec1cff/android_bootimg` and `kstep/kernlog.rs` — are
unaffected and still resolve.

## What the rebase produced

```
kernelsu/patches/KernelSU-v3.3.0-samsung-kdp-rkp-defex.patch
```

1173 lines against v3.2.5's 1157, over the same 15 files: eleven modified, four added whole
(`kernel/compat/samsung_kdp.c`, `kernel/compat/samsung_defex.c`, `kernel/compat/samsung_defex.h`,
`kernel/include/ksu_samsung_kdp.h`). Based on tag `v3.3.0` = `932014a`
("build: bump ddk to 20260828, support android17-6.18", released 2026-08-28); the 3.2.5 tree was
`b0bc817`, the commit the existing patch and README already name.

### How much actually changed

Measured by comparing the two patch files, not by impression:

- **both touch the same 15 files** — no file was added to or dropped from the delta;
- **the four Samsung-authored files are byte-identical** between the two revisions. The KDP credential
  accounting, the RKP syscall-table behaviour, the DEFEX credential sync and the allow path are not
  re-derived and not modified by this upgrade — the Samsung logic did not drift, only its anchor moved;
- **only 35 added lines differ**, and they are three things: the `patch_memory.c` no-patch-text guard
  relocating (identical text, moved because upstream inserted `scan_call_to()` after it), upstream's new
  `data_path` parameter threaded through `finish_install`/`install`, and the doc comments written for the
  split.

So the semantic delta of this whole upgrade is **one extra parameter on two functions and their call
sites**. That is the thing to review; everything else in the diff is upstream's.

## Why it is not just a version bump

v3.3.0 changes the code the Samsung patch rewrites. The commits that matter, from the release's own list:

| Upstream change | Why it reaches us |
| --- | --- |
| `kernel: add syscall dispatcher patching for x64` (#3562) | our patch is about the dispatcher: it records a table hook only when the RKP-protected write succeeds, and falls back to kretprobe/kprobe `sucompat` when it does not |
| `refactor(kernel): Align signature block parsing with AOSP` (#3613), `refactor(kernel): Drop v1 signature verification` (#3628) | the module verifies the **Manager APK's** signing block, so module and manager APK are a pair: 32525 shipped with 3.2.5, 3.3.0's manager is 32601 |
| `ksud: ensure spawn_sulogd don't return in child; execute /proc/self/exe` (#3583) | it solves part of our staging problem in a weaker way, and changes the process/security-context transition our staging exists for |
| `ksud: expose module env vars` (#3522), `Run boot receiver in separated process` (#3567), `ksud: update resetprop` (#3545) | userspace behaviour around the daemon we stage |
| `build: bump ddk to 20260828, support android17-6.18` (#3663) | the DDK image tag moves and the KMI list grows — see the build section |

## The five conflicts, and how each was resolved

Ten of the fifteen files applied cleanly: every one added whole, and six of the eleven modified —
`kernel/feature/sucompat.c`, `kernel/hook/arm64/syscall_hook.c`, `kernel/hook/syscall_hook.h`,
`kernel/hook/syscall_hook_manager.c`, `kernel/hook/tp_marker.c`, `kernel/policy/app_profile.c`.
Eight hunks in the other five were rejected:

### 1. `kernel/Kbuild` (2 hunks) — textual collision

Upstream added `CONFIG_KSU_X86_PATCH_SYSCALL_DISPATCHER` to the same `ccflags` block our
`CONFIG_KSU_SAMSUNG_*` flags go in, and rewrote the `KSU_KERNEL_DIR` block for 6.18's `srcroot`.

**Resolution:** our four Samsung `ccflags` go after upstream's new x86 block, inside the same
`ifdef KBUILD_EXTMOD`; the extra `-I$(objtree)/security/selinux/include` and `-I$(KSU_KERNEL_DIR)/..`
are kept. No semantic conflict.

### 2. `kernel/core/init.c` (2 hunks) — textual collision

Upstream gated the x86 checks on the new config and added `ksu_app_profile_init()`; our patch adds the
DEFEX include and an `int ret;` for the KDP/DEFEX init sequence.

**Resolution:** both, as they were. The ordering is load-bearing and unchanged — symbol resolver, then
`ksu_samsung_kdp_init()`, then `prepare_creds()`, then `ksu_samsung_defex_init()`, with the KDP teardown
on each failure path — and it sits above upstream's unrelated `ksu_app_profile_init()`. This is the one
place to look first if a first build fails.

### 3. `kernel/hook/arm64/patch_memory.c` (1 hunk) — placement only

Upstream added `scan_call_to()` between the end of `ksu_patch_text()` and the file's closing
`#endif /* __aarch64__ */`, which is where our no-patch-text guard's `#endif` used to go.

**Resolution:** the guard closes immediately after `stop_machine(...)`, leaving `scan_call_to()` outside
it. It scans for a BL target; it does not patch anything, so the no-patch-text build has no reason to
lose it.

### 4. `userspace/ksud/src/late_load.rs` (1 hunk) — signature change

Upstream's call site became `utils::install(None, None)`; ours is the post-load half of the split,
`utils::finish_install(None)`.

**Resolution:** `utils::finish_install(None, None)` — the late-load path must still *not* re-copy the
daemon, because ours was staged before the module changed this process's security context. The staging
call at the top of `late_load::run` applied cleanly.

### 5. `userspace/ksud/src/utils.rs` (2 hunks) — the only real one

Upstream solved part of the same problem we solved, differently and less far. #3583 changed the copy
source from `std::env::current_exe()` to the literal `"/proc/self/exe"`, commented *"DO NOT resolve the
real path / So that if someone execute /data/adb/ksud install, ksud won't be removed unexpectedly"* —
the same failure `stage_daemon()` guards. What upstream did not address is the Samsung failure this split
exists for: writing `/data/adb/ksud` from the loader's *changed* security context is refused and leaves a
zero-byte file where a working daemon was.

**Resolution:** the split stays, re-derived on the new signature:

```rust
stage_daemon()                            // copy /proc/self/exe -> /data/adb/ksud, before the load
stage_daemon_from(staged_exe)             // rename a staged copy into place, chown root:root, 0755
finish_install(libadbroot, data_path)     // label, assets, /system/bin link, boot-backup move
install(libadbroot, data_path)            // = stage_daemon() + finish_install(...), for cli.rs
```

Checked at every call site rather than assumed:

- `cli.rs:630 utils::install(libadbroot, data_path)` — matches the new two-parameter `install`, unchanged;
- `late_load.rs:43 stage_daemon_from("/data/local/tmp/.ksud-stage")`, `:83 finish_install(None, None)`;
- the patch deletes `pub fn daemonize`, whose only caller was `late_load.rs`; the surviving
  `daemonize_with` is still called from `init_event.rs:255`, so nothing dangles;
- `use std::process::Command` is removed from `late_load.rs` and `Command` no longer appears in that
  file, so the import is correctly dropped rather than left unused;
- the widened imports (`rustix::fs::chown`, `rustix::thread::{Gid, Uid}`) are **identical to the shipped
  3.2.5 patch**, i.e. already proven by the build that produced the published `ksud` binaries.

## What did not change

- **The exploit payload.** CVE-2026-43499 and its per-profile offsets are independent of the KernelSU tag.
- **The feed schema.** Each artifact stays a single entry; entries need new `size` and `sha256` for
  whatever `ksud-*` is published, at a new commit for clients to pin.
- **The app**, except one line — below.

## Building it: the part that has not been done

The recipe in [`../kernelsu/README.md`](../kernelsu/README.md) still applies per KMI, with these changes
and traps:

1. **Apply the KernelSU2 rewrite first** (above), or `cargo` cannot resolve anything.
2. **The DDK image tag moves to `20260828`**: `ghcr.io/ylarod/ddk-min:<kmi>-20260828`, e.g.
   `android15-6.6-20260828` for the S25U 6.6 profile and `android14-6.1-20260828` for 6.1. Upstream's LKM
   workflow defaults to that release, and its KMI list now includes `android16-6.12` and `android17-6.18`
   — no profile of ours uses those yet.
3. **Substitute the exact target release, as before.** The DDK's own `kernel.release` is not the target's;
   the module must report the target's exact `UTS_RELEASE` or `modprobe`'s version check refuses it.
4. **`ksud` needs an AArch64 assembler and libclang.** Two build-script dependencies that are easy to
   miss, both observed here:
   - `userspace/ksud/build.rs` assembles `src/lkm_image_bootstrap.S` at build time and looks for
     `aarch64-linux-gnu-gcc`, `clang`, or `llvm-mc` on `PATH` — or takes `KSU_LKM_BOOTSTRAP_OBJECT` /
     `KSU_LKM_BOOTSTRAP_CC`. Putting the NDK's `llvm/prebuilt/<host>/bin` on `PATH` satisfies it.
   - bindgen needs `LIBCLANG_PATH` pointing at a directory containing `libclang.dll` (or `libclang.so`).
     **Neither installed NDK ships one on Windows** — Android Studio's NDK provides `clang.exe`,
     `llvm-ar`, `llvm-mc`, but not the libclang shared library. A full LLVM install, or the CI Linux
     image, is what supplies it.
5. **Build inside the git checkout, with the tags present.** `build.rs` derives `VERSION_CODE` from
   `git describe`; outside a repo it warns and falls back to **`VERSION_CODE=0`, `VERSION_NAME=0.0.0`**.
   A `ksud` stamped `0` would misreport itself to the manager, which is exactly the pairing this upgrade
   is about. The rebase trees used here are not git checkouts, so the numbers seen during checking were
   the fallback, not a real build's.

Then, per profile, in the order the README already sets out: build the module, run `kernel/check_symbol`
against the recovered target `vmlinux` (zero missing symbols; a manual relocation audit expects *every*
undefined import present; zero CRC mismatches), strip with `llvm-strip -d`, copy to
`userspace/ksud/bin/aarch64/<kmi>_kernelsu.ko`, build `ksud` for `aarch64-linux-android`, and publish the
two as one pair.

## Validation before any of it is published

The 3.2.5 artifacts are device-tested across nine profiles; a rebase re-opens all of that, so the bar is
the same one they met, per profile:

- late-load succeeds and the loader ends in `u:r:ksu:s0` — the security-context transition is what the
  KDP/DEFEX deltas exist for;
- Manager reports `Working <LKM> [Jailbreak mode]` with a version code of **33000**-and-up, against the
  **3.3.0 manager APK**; a 32525 manager against a 3.3.0 module is the mismatch signature-block parsing
  changed underneath;
- `su` is granted and survives SELinux enforcing;
- `ksud --help` still lists `late-load`, and lists `soft-reboot` if the app is to offer it;
- the app's full chain: exploit, late-load, `su`, and the userspace restart that loads modules.

The 6.6 profile (`galaxy-s25-series-*`, the S25U target) first: it is what this app is developed against,
and its failure mode — a panic in `ksu_mark_running_process_locked` from a generic `put_cred()` inside
KDP-protected memory — is the origin of the whole Samsung delta.

## The app side is one line

```kotlin
// Root-My-Galaxy: MainActivity.kt
private const val KERNEL_SU_MANAGER_URL =
    "https://github.com/tiann/KernelSU/releases/download/v3.2.5/KernelSU_v3.2.5_32525-release.apk"
// -> .../download/v3.3.0/KernelSU_v3.3.0_32601-release.apk
```

Changed **last**, when a 3.3.0 module is actually published: the app hands that URL to the user, and a
manager newer than the module is the direction that goes wrong.

## Deliberate non-goals

- **Not rebasing onto `main`.** `main` carries the x64 LKM and 6.18 work in flux, and its dependency revs
  differ from the tag's; a tag is what the patch keys off and what a published artifact is reproducible
  from.
- **Not adopting upstream's `/proc/self/exe` copy source in place of the split.** It fixes one of the two
  failures `/data/adb/ksud` has on Samsung; the split fixes the other, and merging them is how the second
  comes back.
- **Not folding the KernelSU2 rewrite into our patch.** It is upstream's migration, not our delta; mixing
  it in would make our patch disagree with upstream's own fix and have to be undone when it lands.
- **Not publishing anything.** See the status line at the top.
