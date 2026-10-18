// SPDX-License-Identifier: GPL-2.0-only
/*
 * patch_memory_fused.c - the one symbol the fused build has to supply itself.
 *
 * Upstream resolves the cache-flush helper at run time now: `susfs_main.c` calls
 * `ksu_patch_memory_init()` before any layer can patch, and that function lives in their
 * `kernel/patch_memory.c` - the one file this merge deliberately does not take, because the
 * KernelSU-Next tree's copy is the one with the Samsung work in it and the two define
 * `phys_from_virt` and `ksu_patch_text` between them.
 *
 * In this build the patching and the flush are KernelSU's own: every caller here goes through
 * `ksu_patch_text()`, which takes the KSU_PATCH_TEXT_FLUSH_* flags and performs the flush itself.
 * So there is nothing to resolve or store, and the initialiser reports success and says why -
 * returning a failure would make the merged module refuse to arm for a reason that is not true of
 * this arrangement.
 */

#include <linux/kernel.h>
#include <linux/module.h>

int ksu_patch_memory_init(void)
{
    pr_info("susfs_fused: patching and cache flush are KernelSU's own in this build; "
            "nothing to resolve here\n");
    return 0;
}
