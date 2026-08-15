// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal reproducer for the qdisc_pkt_len_segs_init use-after-free model.
 *
 * The real bug is triggered when:
 *   1. An skb has GSO metadata (gso_type = SKB_GSO_TCPV4).
 *   2. skb_transport_offset() is negative because transport_header was set
 *      before some skb_pull() and never updated.
 *   3. qdisc_pkt_len_segs_init() uses skb->data + hdr_len, where the negative
 *      offset has been cast to unsigned int, causing an out-of-bounds/UAF read.
 *
 * This module reproduces the exact arithmetic and dereference that happens in
 * qdisc_pkt_len_segs_init(), without relying on the full TUN/HSR/GRE topology.
 *
 * Module parameter:
 *   simulate_fix=1  -- reset negative transport_header before the vulnerable
 *                      dereference, mimicking the suggested safety net.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/netdevice.h>
#include <linux/kasan.h>
#include <linux/slab.h>

static bool simulate_fix;
module_param(simulate_fix, bool, 0644);
MODULE_PARM_DESC(simulate_fix, "Reset negative transport_header before dereference");

static int __init qdisc_uaf_repro_init(void)
{
	struct sk_buff *skb;
	struct skb_shared_info *shinfo;
	unsigned int hdr_len;
	const struct tcphdr *th;

	pr_info("qdisc_uaf_repro: loading (simulate_fix=%d)\n", simulate_fix);

	/*
	 * Allocate a small linear skb.  Give it enough headroom and tailroom
	 * so that the skb is valid, but keep the data region small.
	 */
	skb = alloc_skb(128, GFP_KERNEL);
	if (!skb) {
		pr_err("qdisc_uaf_repro: alloc_skb failed\n");
		return -ENOMEM;
	}

	/* Reserve some headroom and put 40 bytes of payload. */
	skb_reserve(skb, 32);
	skb_put(skb, 40);

	/*
	 * Simulate what the network stack + TUN does:
	 *   - network_header and transport_header were probed.
	 *   - Then some path (e.g. GRE decap) pulled skb->data forward without
	 *     updating transport_header.
	 *
	 * We set transport_header to a location *before* data, giving a negative
	 * skb_transport_offset().  In the real bug the negative offset is usually
	 * around -14 (inner ETH_HLEN) or -4 (GRE header).
	 */
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, 0);
	skb_set_transport_header(skb, -14);

	pr_info("qdisc_uaf_repro: data=%px transport_header=%px offset=%d\n",
		skb->data, skb->head + skb->transport_header,
		skb_transport_offset(skb));

	/*
	 * Simulate the GSO metadata that TUN injects via virtio_net_hdr.
	 */
	shinfo = skb_shinfo(skb);
	shinfo->gso_size = 4;
	shinfo->gso_segs = 10;
	shinfo->gso_type = SKB_GSO_TCPV4;

	/*
	 * Reproduce the relevant fragment of qdisc_pkt_len_segs_init():
	 *
	 *     if (!skb->encapsulation) {
	 *         if (unlikely(!skb_transport_header_was_set(skb)))
	 *             return SKB_NOT_DROPPED_YET;
	 *         hdr_len = skb_transport_offset(skb);
	 *     }
	 *     ...
	 *     if (likely(shinfo->gso_type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6))) {
	 *         if (!pskb_may_pull(skb, hdr_len + sizeof(struct tcphdr)))
	 *             return SKB_DROP_REASON_SKB_BAD_GSO;
	 *         th = (const struct tcphdr *)(skb->data + hdr_len);
	 *         tlen = __tcp_hdrlen(th);
	 *         ...
	 *     }
	 */
	if (skb->encapsulation) {
		pr_info("qdisc_uaf_repro: skb->encapsulation is set, aborting\n");
		goto out;
	}

	if (!skb_transport_header_was_set(skb)) {
		pr_info("qdisc_uaf_repro: transport header not set, aborting\n");
		goto out;
	}

	hdr_len = skb_transport_offset(skb);
	pr_info("qdisc_uaf_repro: hdr_len (unsigned) = 0x%x\n", hdr_len);

	if (shinfo->gso_type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) {
		/*
		 * pskb_may_pull(skb, hdr_len + sizeof(struct tcphdr)) is a no-op
		 * because hdr_len is huge; the addition wraps and the resulting
		 * length is tiny, so it trivially succeeds.
		 */
		pr_info("qdisc_uaf_repro: about to dereference skb->data + 0x%x\n",
			hdr_len);

		if (simulate_fix) {
			/*
			 * This mimics the safety net suggested in mailist.patch:
			 * if transport_header offset is negative, reset it and skip
			 * the GSO header parsing (equivalent to returning
			 * SKB_NOT_DROPPED_YET from qdisc_pkt_len_segs_init).
			 */
			if ((int)hdr_len < 0) {
				pr_info("qdisc_uaf_repro: simulate_fix: stale transport_header detected, aborting vulnerable path\n");
				skb_reset_transport_header(skb);
				goto out;
			}
		}

		th = (const struct tcphdr *)(skb->data + hdr_len);
		pr_info("qdisc_uaf_repro: th=%px\n", th);

		/*
		 * This read is what triggers KASAN.  In the real bug the address
		 * happened to land in a freed page; here KASAN will report an
		 * out-of-bounds or use-after-free access depending on the shadow
		 * state around the wrapped address.
		 */
		pr_info("qdisc_uaf_repro: __tcp_hdrlen(th) = %u\n",
			__tcp_hdrlen(th));
	}

out:
	pr_info("qdisc_uaf_repro: survived (KASAN may have already fired)\n");
	kfree_skb(skb);
	return 0;
}

static void __exit qdisc_uaf_repro_exit(void)
{
	pr_info("qdisc_uaf_repro: unloaded\n");
}

module_init(qdisc_uaf_repro_init);
module_exit(qdisc_uaf_repro_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal qdisc_pkt_len_segs_init UAF reproducer");
MODULE_AUTHOR("OpenCode");
