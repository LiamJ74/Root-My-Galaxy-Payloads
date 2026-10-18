// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_try_umount.c - SUSFS's own try_umount, for the merged daemon build.
 *
 * A port of upstream's feature of the same name (simonpunk/susfs4ksu,
 * kernel_patches/fs/susfs.c: susfs_add_try_umount(), try_umount(), susfs_try_umount()), which our
 * fork never carried: its header has only the deprecated id
 * (`CMD_SUSFS_ADD_TRY_UMOUNT`, 0x55580, marked deprecated in susfs_abi.h), so every module that offers
 * "try umount" without CONFIG_KSU_SUSFS_TRY_UMOUNT falls back to KernelSU's own umount list
 * (`ksud kernel umount add <path> --flags 2`) instead of SUSFS's.  This is that mechanism, and the
 * feature list reports it, so such a module takes the SUSFS route again.
 *
 * ## Why this is a daemon-side file rather than an LKM one
 *
 * Upstream's feature has two halves: the list, and the *call site*.  The call site is a line inside
 * KernelSU's setuid handler (their KernelSU patch: `susfs_try_umount(new_uid.val);` with the comment
 * "susfs come first, and lastly umount by ksu, make sure umount in reversed order"), and only a build
 * where these sources are compiled into KernelSU can have it - tools/wire_try_umount.py inserts it
 * into KernelSU-Next's setuid work for ksu-susfs.yml.  In the LKM tier there is no such call site,
 * and the substitute would be a second probe shape on that path, which this project measured as a
 * reset on the S938U1.  So: daemon only, and `susfs_try_umount_active()` is false in a tree where
 * the call site was not inserted - see `susfs_try_umount_call_site_wired()`, which only the wiring
 * script defines.
 *
 * ## The ABI, and why it is not this tree's usual shape
 *
 * The caller is the prebuilt `ksu_susfs` tool that the SUSFS module ships, so the layout is
 * upstream's, not ours:
 *
 *   id             0x5555a                  (upstream; our own 0x55580, deprecated, still accepted)
 *   argument 3     struct { char target_pathname[256]; int mnt_mode; }   (260 bytes, no err field)
 *   argument 5     int err                  (upstream writes the error *here* for this command)
 *
 * That is the one place this file departs from the rest of the module, where every command carries
 * `int err` inside its own struct (sus_path, sus_map, uname, ...).  Both are handled: a caller that
 * used the deprecated id gets the answer written into its struct instead, and a caller that passed
 * no fifth argument at all gets the answer in the kernel log alone rather than a false success.
 *
 * The unmount itself is `path_umount()`, which is not exported, so it is resolved by name once at
 * load exactly as ksu_umount_gate.c resolves KernelSU's predicate.  A tree where that symbol cannot
 * be found reports the feature as inactive rather than pretending to hide.
 *
 * Not verified on a device, and not yet compiled: this file has never been through a build.  It is
 * wired into ksu-susfs.yml by tools/wire_try_umount.py (which asserts every anchor it edits), and
 * the checks that decide whether it works are on the phone: `ksu_susfs show enabled_features` must
 * list CONFIG_KSU_SUSFS_TRY_UMOUNT, the module must log `[try_umount (SUSFS)]` instead of
 * `[try_umount (KSUD)]`, and a registered path must be gone inside an app's mount namespace while
 * still visible to root.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/mount.h>	/* MNT_DETACH, from include/uapi/linux/mount.h */

/* The fork uses no umount flag anywhere, so this constant is the one identifier here that no
 * existing file proves is reachable: it is a uapi value (2) that <linux/mount.h> should bring in,
 * and the guard keeps a header layout this file cannot compile-check from breaking the build. */
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "susfs_abi.h"		/* SUSFS_MAX_LEN_PATHNAME */
#include "susfs_log.h"
#include "symbol_resolver.h"	/* ksu_kallsyms_lookup_name, for path_umount (measured: the first CI run
				 * named it, "call to undeclared function") */
#include "susfs_try_umount.h"

/* Written once by the init below, read by every unmount; non-NULL exactly while the feature is
 * active, which is the invariant the caller relies on instead of testing the pointer on each call. */
static int (*path_umount_fn)(struct path *path, int flags);

/* KernelSU-Next's own root credential, declared here rather than included so this file does not
 * depend on an include path it cannot check from where it is edited: kernel/include/ksu.h:11
 * declares the same thing, it is set up in kernel/core/init.c before any hook can run, and this
 * source is compiled into the same module. */
extern struct cred *ksu_cred;

static LIST_HEAD(try_umount_list);
static DEFINE_MUTEX(try_umount_lock);

/* The kernel's own function is not exported, so the call goes through the resolved address.  The
 * `__nocfi noinline` pair is this tree's idiom for that (ksu_umount_gate.c): CFI checks indirect
 * call sites, and LTO would otherwise inline the wrapper back into an unmarked caller. */
static __nocfi noinline int do_path_umount(struct path *path, int flags)
{
	return path_umount_fn(path, flags);
}

bool susfs_try_umount_active(void)
{
	return path_umount_fn != NULL;
}

/*
 * One unmount, upstream's shape: the path must resolve to a mount root, and a path that no longer
 * resolves is not an error (something else unmounted it, or the app never had it).
 *
 * `mnt_mode` is upstream's: 0 unmounts plainly, 1 asks for MNT_DETACH.
 */
/*
 * Every branch says so in the log, which is what the first two phone tests needed and did not get: a
 * registered path that stayed visible could have been unresolvable, not a mount root, or refused by
 * path_umount, and a silent return made those three indistinguishable from "the feature never ran".
 * `err 0` from the last line is the success case.
 */
static void try_umount_one(const char *mnt, int mnt_mode)
{
	struct path path;
	int err;

	err = kern_path(mnt, 0, &path);
	if (err) {
		SUSFS_LOGI("try_umount: '%s' not resolved, err %d (a path the calling process cannot "
			   "reach, which for an app is a SELinux refusal more often than a permission bit)\n",
			   mnt, err);
		return;
	}

	if (path.dentry != path.mnt->mnt_root) {
		SUSFS_LOGI("try_umount: '%s' is not a mount root, nothing to unmount\n", mnt);
		path_put(&path);
		return;
	}

	/* No path_put() after this call, and that is not an oversight.  path_umount() consumes the
	 * caller's reference on every path it takes once past its own mount-root check - which this
	 * function has already done - so dropping it again is a double put: the phone's dmesg showed
	 * exactly that, a WARN in mntput_no_expire called from path_put in this function.  KernelSU-Next's
	 * own try_umount() puts only on the not-a-mount-root path above and never here, which is what this
	 * now matches. */
	err = do_path_umount(&path, mnt_mode == 1 ? MNT_DETACH : 0);
	SUSFS_LOGI("try_umount: '%s' unmounted, err %d\n", mnt, err);
}

/*
 * Unmount every registered path in the calling task's mount namespace.
 *
 * Called from KernelSU's setuid work, so the "current" mount namespace is the app process being
 * spawned - that is the whole trick, and it is upstream's: the list is global, the namespace is not.
 * The uid is carried for the log line only, exactly as upstream carries `target_uid` without using
 * it for a decision.
 */
void susfs_try_umount(uid_t target_uid)
{
	struct st_susfs_try_umount_list *cursor, *temp;
	const struct cred *saved;

	if (!path_umount_fn)
		return;

	/* No gate of ours: the call site is inside KernelSU-Next's own umount path, where the new uid has
	 * already been checked to be an app (or an isolated or webview-zygote one), `ksu_uid_should_umount()`
	 * has said yes, and the caller has been checked to be a zygote child - their guard against an su app
	 * that setuid'd to an app uid while sitting in the global mount namespace.  A gate here reading
	 * current_uid() would see the *old* uid at that point and refuse everything, which is what an
	 * earlier version of this file did.
	 *
	 * path_umount() still needs CAP_SYS_ADMIN in the mount namespace's user namespace.  KernelSU-Next
	 * swaps in ksu_cred around its own walk, and this repeats it rather than relying on that: with the
	 * call in the setuid work - before this file was moved - every unmount returned -1 (EPERM) on the
	 * phone, because the credentials were already the app's. */
	saved = ksu_cred ? override_creds(ksu_cred) : NULL;

	mutex_lock(&try_umount_lock);
	list_for_each_entry_safe(cursor, temp, &try_umount_list, list) {
		SUSFS_LOGI("try_umount: '%s' (mnt_mode %d) for uid %u\n",
			   cursor->info.target_pathname, cursor->info.mnt_mode, target_uid);
		try_umount_one(cursor->info.target_pathname, cursor->info.mnt_mode);
	}
	mutex_unlock(&try_umount_lock);

	if (saved)
		revert_creds(saved);
}

/*
 * CMD_SUSFS_ADD_TRY_UMOUNT.
 *
 * A duplicate path is refused rather than added twice, which is upstream's behaviour and also what
 * keeps a caller from growing the list with repeated runs.
 */
static int add_try_umount(const char *pathname, int mnt_mode)
{
	struct st_susfs_try_umount_list *entry;

	mutex_lock(&try_umount_lock);
	list_for_each_entry(entry, &try_umount_list, list) {
		if (unlikely(!strcmp(pathname, entry->info.target_pathname))) {
			mutex_unlock(&try_umount_lock);
			/* SUSFS_LOGI and pr_warn are the two the fork's own sources use; there is no
			 * SUSFS_LOGE in susfs_log.h, which is what this said first. */
			SUSFS_LOGI("try_umount: '%s' is already registered\n", pathname);
			return -EEXIST;
		}
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		mutex_unlock(&try_umount_lock);
		pr_warn_ratelimited("susfs_guard_lkm: try_umount: no memory for '%s'\n", pathname);
		return -ENOMEM;
	}

	strscpy(entry->info.target_pathname, pathname, SUSFS_MAX_LEN_PATHNAME);
	entry->info.mnt_mode = mnt_mode;
	INIT_LIST_HEAD(&entry->list);
	list_add_tail(&entry->list, &try_umount_list);
	mutex_unlock(&try_umount_lock);

	SUSFS_LOGI("try_umount: '%s' (mnt_mode %d) added\n", pathname, mnt_mode);
	return 0;
}

void susfs_try_umount_supercall(void __user **arg, void __user *err_ptr)
{
	struct st_susfs_try_umount info = {0};
	int err;

	if (copy_from_user(&info, (void __user *)*arg, sizeof(info))) {
		err = -EFAULT;
		goto out;
	}

	/* The pathname is a userspace string: refuse one that does not end inside the field rather
	 * than letting strlen()/kern_path() walk past the struct, which lives on our stack. */
	if (!memchr(info.target_pathname, '\0', sizeof(info.target_pathname))) {
		err = -EINVAL;
		goto out;
	}
	if (info.target_pathname[0] != '/') {
		err = -EINVAL;
		goto out;
	}

	err = add_try_umount(info.target_pathname, info.mnt_mode);

out:
	/* Both channels, deliberately.  The caller's layout is not knowable from here: the shipped helper
	 * sends our fork's id (0x55580) and pre-seeds an `err` field in the object it passed, while
	 * upstream's shape answers in the command's fifth argument.  The phone caught the cost of
	 * choosing one - a helper that reads the field we did not write reports the feature unsupported
	 * even when the rule was added - so upstream's own habit of writing both is what this does.
	 *
	 * The struct write is safe because the caller passed that object.  The pointer write is guarded by
	 * access_ok, again as upstream guards it, and the whole supercall is root-only (reboot_pre returns
	 * early for any other uid), so an argument register that was never an error pointer cannot be
	 * used to write into a kernel address. */
	if (copy_to_user(&((struct st_susfs_try_umount_reply __user *)*arg)->err, &err, sizeof(err)))
		pr_warn("try_umount supercall: copy_to_user(struct err) failed, err %d lost\n", err);

	if (err_ptr && access_ok(err_ptr, sizeof(err))) {
		if (copy_to_user(err_ptr, &err, sizeof(err)))
			pr_warn("try_umount supercall: copy_to_user(err_ptr) failed, err %d lost\n", err);
	}
}

int susfs_try_umount_init(void)
{
	unsigned long addr = ksu_kallsyms_lookup_name("path_umount");

	if (!addr) {
		/* Not fatal: the list still exists and the command is still answered, so a caller
		 * gets -ENOSYS-ish behaviour with a reason in the log instead of a silent no-op. */
		pr_warn("susfs_guard_lkm: path_umount is not in kallsyms - try_umount is NOT active, "
			"so CONFIG_KSU_SUSFS_TRY_UMOUNT is not advertised and the module keeps using "
			"KernelSU's umount list\n");
		return 0;
	}

	path_umount_fn = (int (*)(struct path *, int))addr;
	SUSFS_LOGI("try_umount: resolved path_umount at %px\n", (void *)addr);
	return 0;
}

void susfs_try_umount_exit(void)
{
	struct st_susfs_try_umount_list *cursor, *temp;

	mutex_lock(&try_umount_lock);
	list_for_each_entry_safe(cursor, temp, &try_umount_list, list) {
		list_del(&cursor->list);
		kfree(cursor);
	}
	mutex_unlock(&try_umount_lock);

	path_umount_fn = NULL;
}
