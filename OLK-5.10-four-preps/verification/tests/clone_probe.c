// SPDX-License-Identifier: GPL-2.0
/* Guest-only allocation fault probe for the PIPAPO walk regression. */
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <net/netfilter/nf_tables.h>

static unsigned int arm, clones, failures, reads, updates, flushes;
static char *clone_symbol = "pipapo_clone";
module_param(clone_symbol, charp, 0444);
module_param(arm, uint, 0644);
module_param(clones, uint, 0444);
module_param(failures, uint, 0444);
module_param(reads, uint, 0444);
module_param(updates, uint, 0444);
module_param(flushes, uint, 0444);

static int clone_enter(struct kprobe *p, struct pt_regs *regs)
{
	clones++;
	if (READ_ONCE(arm)) {
		WRITE_ONCE(arm, 0);
		WRITE_ONCE(current->fail_nth, 1);
		failures++;
	}
	return 0;
}

static int walk_enter(struct kprobe *p, struct pt_regs *regs)
{
	const struct nft_set_iter *iter = (void *)regs->dx;

	switch (iter->type) {
	case NFT_ITER_UNSPEC:
		break;
	case NFT_ITER_READ:
		reads++;
		break;
	case NFT_ITER_UPDATE:
		updates++;
		break;
	case NFT_ITER_UPDATE_CLONE:
		flushes++;
		break;
	}
	return 0;
}

static struct kprobe probes[] = {
	{ .symbol_name = "pipapo_clone", .pre_handler = clone_enter },
	{ .symbol_name = "nft_pipapo_walk", .pre_handler = walk_enter },
};

static int __init clone_probe_init(void)
{
	int err;

	probes[0].symbol_name = clone_symbol;
	err = register_kprobe(&probes[0]);
	if (err)
		return err;
	err = register_kprobe(&probes[1]);
	if (err)
		unregister_kprobe(&probes[0]);
	return err;
}

static void __exit clone_probe_exit(void)
{
	unregister_kprobe(&probes[1]);
	unregister_kprobe(&probes[0]);
}
module_init(clone_probe_init);
module_exit(clone_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Guest-only PIPAPO clone/iterator fault injection probe");
