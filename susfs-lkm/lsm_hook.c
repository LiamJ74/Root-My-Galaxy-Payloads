// SPDX-License-Identifier: GPL-2.0
/*
 * The LSM layer, installed the way this project's own Samsung delta installs its hooks.
 *
 * ## Why upstream's install path cannot be used here
 *
 * Upstream publishes a `struct security_hook_list` by writing it into `security_hook_heads` (or into
 * the `selinux_hooks[]` slot it found by symbol identity) - memory that is `__lsm_ro_after_init`, so
 * read-only after boot. It makes that write land by translating the address to a physical one, mapping
 * it through a fixmap and storing through `copy_to_kernel_nofault` under `stop_machine`.
 *
 * On Samsung's kernels that write does not land. Measured on an SM-S938U1 (`6.6.127-android15-8-...-4k`,
 * SELinux enforcing):
 *
 *     lsm_hook: inode_getattr: list head found by identity at offset 0x1c8
 *     lsm_hook: failed to insert inode_getattr at the head of its list: -14
 *     sus_path: getattr hook failed -14 - nothing would be hidden, refusing to load
 *
 * `-14` is `EFAULT` out of that read-only write. This project's KernelSU delta already answers exactly
 * this, and its comment says why the answer is a kprobe:
 *
 *     the slots this feature patches live in the rodata range handed to the hypervisor at init, so
 *     ksu_patch_text() comes back with an error ... A kprobe reaches the same call site from the other
 *     side: kprobe instruction pages are whitelisted with the hypervisor, so a breakpoint is accepted
 *     where a plain write is not. Every slot therefore tries the write first and diverts the handler it
 *     points at when the write is refused.
 *
 * So this file does what `selinux_hide` does: one `struct kprobe` per member, a `pre_handler`, and a
 * redirect of the instruction pointer to a `__nocfi` trampoline. Registration is by address first and
 * by name second, which is `sel_hide_register_probe`'s rule.
 *
 * ## What a first attempt at this got wrong, and why it is written this way
 *
 * The first version of this file used **kretprobes** with per-instance data and overrode the return
 * value from the return handler. Two device results settled it: with one probe installed the module
 * refused cleanly, and with two or more the phone rebooted - including when `inode_permission` was
 * skipped, so it was the mechanism rather than any one member. A kretprobe's return handler is not a
 * place to run a filesystem decision, and it is not what this project uses anywhere.
 *
 * ## The three parts, each of them a rule taken from the delta
 *
 * 1. **A per-task recursion guard.** A replacement does its own path work, and that work calls the
 *    same function again, hitting the same probe. The delta keys the guard on the calling task rather
 *    than a global latch, so two unrelated tasks never block each other and a task that dies inside a
 *    replacement leaks one small entry instead of wedging every later call.
 * 2. **`instruction_pointer_set(regs, tramp)` and `return 1`.** The original does not run; the trampoline
 *    does, with the registers untouched.
 * 3. **`__nocfi` on both the handler and the trampoline.** The trampoline is entered by a redirected
 *    instruction pointer, not by a checked call, and it makes the one call this design needs - the
 *    declared replacement - through a pointer whose type the module cannot restate for thirteen
 *    different prototypes. `__nocfi` is what makes that call legal here, and it is why the delta marks
 *    its own tramps the same way.
 *
 * ## What it does, exactly
 *
 * Only denials are ever added, which is the whole of what upstream's insert-shaped hook was allowed to
 * do: the declared replacement is asked, and
 *
 *   - it answers 0 -> the hook is left completely alone and SELinux's own decision stands;
 *   - it answers non-zero -> the call is diverted to a trampoline that returns that value, so the
 *     denial is added and SELinux never runs - exactly as upstream's inserted node, which ran before
 *     every LSM and returned its denial without consulting one.
 *
 * Nothing here writes to the `struct ksu_lsm_hook` it is handed; the declaration belongs to `sus_path.c`
 * and the probe that carries it lives in this file's slot table.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/lsm_hooks.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include "lsm_hook.h"
#include "susfs_log.h"
#include "symbol_resolver.h"

/*
 * Diagnostics, kept from the two reboots that produced them.
 *
 * `lsm_max=N` installs at most N probes and `lsm_off=1` installs none, so the layer can be run as a
 * prefix of itself; `lsm_skip=<member>[,<member>]` registers a member and installs nothing, which is
 * the difference between a module that loads with one feature unarmed and one that refuses to load.
 */
static int lsm_max = -1;
module_param(lsm_max, int, 0644);
MODULE_PARM_DESC(lsm_max, "install at most this many probes (-1 installs every hook the caller declares)");

static bool lsm_off;
module_param(lsm_off, bool, 0644);
MODULE_PARM_DESC(lsm_off, "install no probes at all (diagnostic)");

static char lsm_skip[128] = "";
module_param_string(lsm_skip, lsm_skip, sizeof(lsm_skip), 0644);
MODULE_PARM_DESC(lsm_skip, "comma-separated members not to probe, e.g. inode_permission");

/* The verdict a diverted call returns. Per CPU, because the redirect and the trampoline it jumps to
 * are the same call on the same CPU - no lock and no lookup in between. */
static DEFINE_PER_CPU(int, susfs_verdict);

/* One member, its probe, and what it needs to answer. */
struct susfs_slot {
	const char *member;
	const char *symbol;
	struct kprobe probe;
	const struct ksu_lsm_hook *hook;
};

/*
 * The guard, verbatim in shape from `selinux_hide`: keyed on the calling task, entered before a
 * replacement runs and left before it returns. A nested entry - the replacement's own path work hitting
 * the same probe - finds the entry and is answered by the kernel's own code instead.
 */
static DEFINE_XARRAY(susfs_probe_guard);

static bool susfs_guard_enter(void)
{
	void *old;

	if (xa_load(&susfs_probe_guard, (unsigned long)current))
		return false;
	old = xa_store(&susfs_probe_guard, (unsigned long)current, current, GFP_KERNEL);
	return !IS_ERR(old);
}

static void susfs_guard_exit(void)
{
	xa_erase(&susfs_probe_guard, (unsigned long)current);
}

/* The trampoline a denial diverts to: the registers are untouched and the verdict is the answer. No
 * indirect call happens here, which is why this one needs no `__nocfi` reasoning at all. */
static int __nocfi susfs_tramp_deny(void)
{
	int verdict = this_cpu_read(susfs_verdict);

	susfs_guard_exit();
	return verdict;
}

/*
 * The handler every member shares. It is `__nocfi` because it calls the declared replacement through a
 * pointer whose prototype it cannot restate for thirteen different hooks - the callee is the module's own
 * function with the kernel's hook prototype, and a checked call through a restated type would trap.
 */
static int __nocfi susfs_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct susfs_slot *slot = container_of(p, struct susfs_slot, probe);
	const struct ksu_lsm_hook *hook = slot->hook;
	int verdict;

	if (!hook || !hook->replacement)
		return 0;
	if (!susfs_guard_enter())
		return 0;

	verdict = ((int (*)(unsigned long, unsigned long, unsigned long, unsigned long,
			    unsigned long, unsigned long, unsigned long, unsigned long))(hook->replacement))(
		regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3], regs->regs[4], regs->regs[5],
		regs->regs[6], regs->regs[7]);

	if (!verdict) {
		/* Pass-through: the guard goes and the kernel's own code runs as if this probe were not
		 * here. Answering 0 is what upstream's insert path did on the same path. */
		susfs_guard_exit();
		return 0;
	}

	this_cpu_write(susfs_verdict, verdict);
	instruction_pointer_set(regs, (unsigned long)susfs_tramp_deny);
	return 1;
}

/* The members `sus_path.c` declares, and the SELinux implementation each one decides. The symbol is
 * resolved to an address and the probe is registered on that address first - the rule this project's
 * delta settled on, because the breakpoint page is accepted where a write is not. */
#define SUSFS_SLOT(member_)                    \
	{                                      \
		.member = #member_,            \
		.symbol = "selinux_" #member_, \
		.probe = { .pre_handler = susfs_pre_handler, }, \
	}

static struct susfs_slot susfs_slots[] = {
	SUSFS_SLOT(inode_permission),
	SUSFS_SLOT(inode_getattr),
	SUSFS_SLOT(inode_unlink),
	SUSFS_SLOT(inode_rmdir),
	SUSFS_SLOT(inode_rename),
	SUSFS_SLOT(inode_link),
	SUSFS_SLOT(sb_statfs),
	SUSFS_SLOT(inode_setattr),
	SUSFS_SLOT(inode_getxattr),
	SUSFS_SLOT(inode_listxattr),
	SUSFS_SLOT(inode_setxattr),
	SUSFS_SLOT(inode_removexattr),
	SUSFS_SLOT(path_notify),
};

static DEFINE_MUTEX(susfs_slots_lock);
static int susfs_installed;

static struct susfs_slot *susfs_slot_for(const char *member)
{
	size_t i;

	if (!member)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(susfs_slots); i++) {
		if (!strcmp(susfs_slots[i].member, member))
			return &susfs_slots[i];
	}
	return NULL;
}

/* Whole names only: an entry for `getattr` must not skip `inode_getattr`, and a prefix match would. */
static bool susfs_skip_named(const char *member)
{
	const char *cursor = lsm_skip;
	size_t length;

	if (!member || !lsm_skip[0])
		return false;
	length = strlen(member);
	while (*cursor) {
		const char *end = strchrnul(cursor, ',');
		size_t span = (size_t)(end - cursor);

		while (span && (*cursor == ' ' || *cursor == '\t')) {
			cursor++;
			span--;
		}
		while (span && (cursor[span - 1] == ' ' || cursor[span - 1] == '\t'))
			span--;
		if (span == length && !strncmp(cursor, member, length))
			return true;
		cursor = *end ? end + 1 : end;
	}
	return false;
}

/* Address first, name second - `sel_hide_register_probe`'s rule, and the reason it exists: a kernel
 * that refuses a probe placed by raw address accepts the same probe named through kallsyms. */
static int susfs_register_probe(struct susfs_slot *slot)
{
	unsigned long target = find_kernel_symbol_exact(slot->symbol);
	int ret;

	if (!target) {
		pr_err("lsm_hook: %s: no address for %s, so this hook cannot be reached\n", slot->member,
		       slot->symbol);
		return -ENOENT;
	}

	slot->probe.addr = (kprobe_opcode_t *)target;
	ret = register_kprobe(&slot->probe);
	if (ret == -EINVAL) {
		pr_warn("lsm_hook: %s: probe on %s by address refused (%d), retrying by name\n",
			slot->member, slot->symbol, ret);
		slot->probe.addr = NULL;
		slot->probe.symbol_name = slot->symbol;
		ret = register_kprobe(&slot->probe);
		slot->probe.symbol_name = NULL;
	}
	if (ret) {
		slot->probe.addr = NULL;
		return ret;
	}
	return 0;
}

int ksu_register_lsm_hook(struct ksu_lsm_hook *hook)
{
	struct susfs_slot *slot;

	if (!hook || !hook->replacement) {
		pr_err("lsm_hook: nothing to install\n");
		return -EINVAL;
	}

	/* A swap needs the read-only write this file exists to avoid. Refused with the reason, because a
	 * hook that installs and does nothing is worse than one that says it cannot run here. */
	if (!hook->insert) {
		pr_err("lsm_hook: %s: replace-shaped hooks need a write into read-only kernel memory, "
		       "which this kernel refuses; declare it with KSU_LSM_HOOK_INSERT instead\n",
		       hook->head_name ?: "unknown");
		return -EOPNOTSUPP;
	}

	slot = susfs_slot_for(hook->head_name);
	if (!slot) {
		pr_err("lsm_hook: %s: no SELinux implementation is known for this member; refusing to "
		       "install it against something else\n", hook->head_name ?: "unknown");
		return -ENOENT;
	}

	/* A member this kernel cannot carry a probe for: registered, installed, and said out loud. */
	if (susfs_skip_named(hook->head_name)) {
		SUSFS_LOGI("lsm_hook: %s: skipped by lsm_skip - this hook is not armed and the features it "
			   "gates stay off\n", hook->head_name ?: "unknown");
		return 0;
	}

	if (lsm_off || (lsm_max >= 0 && susfs_installed >= lsm_max)) {
		pr_warn("lsm_hook: %s: not installed (lsm_off=%d lsm_max=%d, %d already installed)\n",
			hook->head_name ?: "unknown", lsm_off, lsm_max, susfs_installed);
		return -EOPNOTSUPP;
	}

	mutex_lock(&susfs_slots_lock);
	if (slot->hook) {
		mutex_unlock(&susfs_slots_lock);
		pr_err("lsm_hook: %s: already installed\n", hook->head_name ?: "unknown");
		return -EEXIST;
	}
	slot->hook = hook;
	if (susfs_register_probe(slot)) {
		slot->hook = NULL;
		mutex_unlock(&susfs_slots_lock);
		pr_err("lsm_hook: %s: probe on %s refused\n", hook->head_name ?: "unknown", slot->symbol);
		return -ENOENT;
	}
	/*
	 * The `entry` field, used as the flag it is everywhere outside upstream's own install path: this
	 * module's teardown runs **only** for a hook whose `entry` is non-NULL
	 * (`if (sus_path_perm_hook.entry) ... ksu_unregister_lsm_hook(...)`, sus_path.c), and
	 * `sus_path_lsm_active()` is `getattr.entry && perm.entry`. A kprobe route inserts no node, so
	 * leaving this NULL meant two faults at once: the layer read as unarmed (`lsm=0`, so `/proc/susfs_*`
	 * was never created and the supercall reported SUS_PATH unsupported), and an unload skipped our
	 * `unregister_kprobe()` - leaving thirteen probes registered into module memory the kernel was
	 * about to free, which is a reset on the next syscall through any of them.
	 *
	 * The sentinel is a flag, never a node: nothing in this build dereferences it. Upstream's own
	 * dereferences live in `lsm_hook.c`, the file this one replaces.
	 */
	hook->entry = (struct security_hook_list *)hook;
	susfs_installed++;
	mutex_unlock(&susfs_slots_lock);

	SUSFS_LOGI("lsm_hook: %s: kprobe armed on %s\n", hook->head_name ?: "unknown", slot->symbol);
	return 0;
}

void ksu_unregister_lsm_hook(struct ksu_lsm_hook *hook)
{
	struct susfs_slot *slot;

	if (!hook)
		return;

	slot = susfs_slot_for(hook->head_name);
	if (!slot)
		return;

	mutex_lock(&susfs_slots_lock);
	if (slot->hook != hook) {
		mutex_unlock(&susfs_slots_lock);
		pr_warn("lsm_hook: %s: nothing was installed under this declaration\n",
			hook->head_name ?: "unknown");
		return;
	}
	/* Cleared before the call, so a re-register cannot find a probe being taken down.
	 * `unregister_kprobe()` waits for the handlers already running. */
	slot->hook = NULL;
	unregister_kprobe(&slot->probe);
	/* Cleared with the probe, so the guards that decide whether there is anything left to unregister
	 * read the same state this file does. */
	hook->entry = NULL;
	if (susfs_installed > 0)
		susfs_installed--;
	mutex_unlock(&susfs_slots_lock);

	SUSFS_LOGI("lsm_hook: %s: kprobe removed\n", hook->head_name ?: "unknown");
}

/* The two entry points the header carries that this install path has no separate use for: an
 * insert-shaped registration *is* the hook. Kept so the API the rest of the tree links against is
 * unchanged. */
int ksu_lsm_hook(struct ksu_lsm_hook *hook)
{
	return ksu_register_lsm_hook(hook);
}

void ksu_lsm_unhook(struct ksu_lsm_hook *hook)
{
	ksu_unregister_lsm_hook(hook);
}

void ksu_lsm_hook_init(void)
{
	SUSFS_LOGI("lsm_hook: install path is kprobe-based - security_hook_heads is read-only here, and a "
		   "breakpoint is accepted where a write is not\n");
}

void ksu_lsm_hook_exit(void)
{
	SUSFS_LOGI("lsm_hook: exit (probes are owned by the hooks that registered them)\n");
}
