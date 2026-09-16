// SPDX-License-Identifier: GPL-2.0
/* Guest-only coverage counters for the set backend type conversion. */
#include <linux/atomic.h>
#include <linux/kprobes.h>
#include <linux/module.h>

static atomic_t hits[6];
static struct kprobe probes[6];

static int record_hit(struct kprobe *p, struct pt_regs *regs)
{
	atomic_inc(&hits[p - probes]);
	return 0;
}

static int counts_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d,%d,%d,%d,%d,%d\n",
		atomic_read(&hits[0]), atomic_read(&hits[1]),
		atomic_read(&hits[2]), atomic_read(&hits[3]),
		atomic_read(&hits[4]), atomic_read(&hits[5]));
}

static const struct kernel_param_ops counts_ops = { .get = counts_get };
module_param_cb(counts, &counts_ops, NULL, 0444);

static struct kprobe probes[6] = {
	{ .symbol_name = "nft_bitmap_get", .pre_handler = record_hit },
	{ .symbol_name = "nft_hash_get", .pre_handler = record_hit },
	{ .symbol_name = "nft_rhash_get", .pre_handler = record_hit },
	{ .symbol_name = "nft_rbtree_get", .pre_handler = record_hit },
	{ .symbol_name = "nft_pipapo_get", .pre_handler = record_hit },
	{ .symbol_name = "nft_rhash_update", .pre_handler = record_hit },
};

static int __init set_probe_init(void)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		err = register_kprobe(&probes[i]);
		if (err) {
			while (i--)
				unregister_kprobe(&probes[i]);
			return err;
		}
	}
	return 0;
}

static void __exit set_probe_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(probes); i++)
		unregister_kprobe(&probes[i]);
}
module_init(set_probe_init);
module_exit(set_probe_exit);
MODULE_LICENSE("GPL");
