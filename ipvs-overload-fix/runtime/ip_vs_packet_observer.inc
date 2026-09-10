/* SPDX-License-Identifier: GPL-2.0 */
/* Read-only observation for the separate userspace packet test. */
static int ov_packet_show(struct seq_file *seq, void *v)
{
	struct netns_ipvs *ipvs = net_ipvs(&init_net);
	union nf_inet_addr vip = { .ip = htonl(0x7f000064) };
	struct ip_vs_service *svc;
	struct ip_vs_dest *dest;

	mutex_lock(&ipvs->service_mutex);
	rcu_read_lock();
	svc = ip_vs_service_find(ipvs, AF_INET, 0, IPPROTO_UDP,
				 &vip, htons(31001));
	if (svc) {
		list_for_each_entry(dest, &svc->destinations, n_list) {
			spin_lock_bh(&dest->dst_lock);
			seq_printf(seq, "total=%d upper=%u lower=%u overload=%u\n",
				   atomic_read(&dest->totalconns),
				   READ_ONCE(dest->u_threshold),
				   READ_ONCE(dest->l_threshold_val),
				   !!(READ_ONCE(dest->flags) & IP_VS_DEST_F_OVERLOAD));
			spin_unlock_bh(&dest->dst_lock);
		}
	} else {
		seq_puts(seq, "no service\n");
	}
	rcu_read_unlock();
	mutex_unlock(&ipvs->service_mutex);
	return 0;
}
DEFINE_PROC_SHOW_ATTRIBUTE(ov_packet);

static int __init ov_packet_init(void)
{
	return proc_create("ipvs_packet_state", 0400, NULL,
			   &ov_packet_proc_ops) ? 0 : -ENOMEM;
}
late_initcall(ov_packet_init);
