// SPDX-License-Identifier: GPL-2.0
/* Test-only: arm the kernel's allocator fault injection after field insertion. */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>

static unsigned int arm, hits, fired;
module_param(arm, uint, 0644);
module_param(hits, uint, 0444);
module_param(fired, uint, 0444);

static int fail_scratch(struct kprobe *p, struct pt_regs *regs)
{
	unsigned int remaining = READ_ONCE(arm);

	hits++;
	if (remaining) {
		WRITE_ONCE(arm, remaining - 1);
		if (remaining == 1) {
			WRITE_ONCE(current->fail_nth, 1);
			fired++;
		}
	}
	return 0;
}

static struct kprobe probe = {
	.symbol_name = "pipapo_realloc_scratch",
	.pre_handler = fail_scratch,
};

static int __init pipapo_fail_init(void)
{
	return register_kprobe(&probe);
}

static void __exit pipapo_fail_exit(void)
{
	unregister_kprobe(&probe);
}
module_init(pipapo_fail_init);
module_exit(pipapo_fail_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CVE-2026-72252 targeted allocator fault injection, guest only");
