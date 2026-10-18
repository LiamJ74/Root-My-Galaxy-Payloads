/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SUSFS_TRY_UMOUNT_H
#define __SUSFS_TRY_UMOUNT_H

#include <linux/types.h>
#include <linux/list.h>

/* SUSFS_MAX_LEN_PATHNAME: included here rather than left to the consumer, so this header is
 * self-sufficient - it is inserted into a file whose include order is not ours. */
#include "susfs_abi.h"

/*
 * SUSFS's own try_umount (upstream CONFIG_KSU_SUSFS_TRY_UMOUNT), for the merged daemon build.
 *
 * Upstream keeps a list of paths built by CMD_SUSFS_ADD_TRY_UMOUNT and unmounts every one of them
 * inside KernelSU's setuid handler, immediately before KernelSU's own umounts - their comment says
 * why the order is that way: "susfs come first, and lastly umount by ksu, make sure umount in
 * reversed order".  This layer is that list and that unmount; the call site is a compile-time call
 * inserted into KernelSU-Next's setuid work by tools/wire_try_umount.py, which is possible only in
 * the build where these sources are compiled into KernelSU (ksu-susfs.yml) - an LKM has no such
 * call site and would need a second probe shape on that path instead.
 *
 * The ABI is upstream's, deliberately: id 0x5555a, a 260-byte
 * `struct { char target_pathname[256]; int mnt_mode; }` in the command's third argument and the
 * error in its fifth, because the caller is the prebuilt `ksu_susfs` tool the SUSFS module ships.
 * Our own tree's older id for the same command (0x55580, marked deprecated) is accepted too, and
 * for that one the error is written into the struct the way every other command in this module
 * does it - see susfs_try_umount.c.
 */

/* Upstream's command id, and the one the shipped helper actually sends. */
#define SUSFS_CMD_ADD_TRY_UMOUNT_UPSTREAM	0x5555a

/*
 * One registered path, in upstream's layout: 260 bytes, `mnt_mode` 0 for a plain unmount and 1 for
 * MNT_DETACH.  Deliberately no `err` field, which is where this command differs from every other one
 * in this tree - upstream passes the error back in the command's fifth argument instead, and the
 * prebuilt helper the SUSFS module ships reads it there.
 */
struct st_susfs_try_umount {
	char target_pathname[SUSFS_MAX_LEN_PATHNAME];
	int mnt_mode;
};

/*
 * What the caller's object looks like for the deprecated id (0x55580), i.e. the shipped helper's
 * layout: the same two fields with the error after them, which is this tree's convention for every
 * other command.  Measured on the phone rather than assumed - the helper reported
 * "CMD: '0x55580', SUSFS operation not supported" because its pre-seeded 126 stayed in this field;
 * it pre-seeds and reads the answer from the same place, so this offset is the ABI.
 */
struct st_susfs_try_umount_reply {
	char target_pathname[SUSFS_MAX_LEN_PATHNAME];
	int mnt_mode;
	int err;
};

/* The list node, this tree's idiom for every other rule table (sus_path, sus_kstat, open_redirect). */
struct st_susfs_try_umount_list {
	struct list_head list;
	struct st_susfs_try_umount info;
};

/* Whether the layer is installed: false hides the feature from the enabled-features list, which is
 * the honest answer for a layer whose symbol did not resolve - advertising a feature whose
 * registration failed is exactly the inconsistency a detector probes for. */
bool susfs_try_umount_active(void);

/* The supercall. `arg` is the command's payload pointer from the transport, `err_ptr` the fifth
 * argument when the caller passed one (NULL when the helper did not). */
void susfs_try_umount_supercall(void __user **arg, void __user *err_ptr);

/* Unmounts every registered path in the calling process's mount namespace.  Called from KernelSU's
 * setuid work, so it runs in the app process that is being spawned. */
void susfs_try_umount(uid_t target_uid);

int susfs_try_umount_init(void);
void susfs_try_umount_exit(void);

#endif /* __SUSFS_TRY_UMOUNT_H */
