/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test-only code included at the end of ip_vs_ctl.c in the validation tree.
 * Calls the real edit/new/put/scheduler functions without changing them.
 * No counter or destination flag is assigned by this test. UDP one-packet
 * connections allow synchronous release through the real expiry path.
 * This is an in-kernel API concurrency test, not a packet-driven reproducer.
 */
#include <linux/kthread.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/ktime.h>

struct ov_gate {
	u32 value;
} ____cacheline_aligned_in_smp;

struct ov_test {
	struct ov_gate epoch;
	struct ov_gate edited;
	struct ov_gate accounted;
	struct netns_ipvs *ipvs;
	struct ip_vs_service *svc;
	struct ip_vs_dest *dest;
	struct ip_vs_dest_user_kern dest_user;
	struct ip_vs_conn *base[99];
	struct ip_vs_conn *racing;
	unsigned int base_count;
	unsigned int iterations;
	unsigned int delay_max;
	unsigned int completed;
	unsigned int misses;
	unsigned int wrong_schedule;
	unsigned int persistent_error;
	unsigned int error;
	unsigned int lower;
	u64 elapsed_ns;
};

static DEFINE_MUTEX(ov_test_mutex);
static char ov_result[768] = "not run\n";

static int ov_edit(struct ov_test *t, u32 upper, u32 lower)
{
	struct ip_vs_dest_user_kern u = t->dest_user;
	int ret;

	u.u_threshold = upper;
	u.l_threshold = lower;
	mutex_lock(&t->ipvs->service_mutex);
	ret = ip_vs_edit_dest(t->svc, &u);
	mutex_unlock(&t->ipvs->service_mutex);
	return ret;
}

static struct ip_vs_conn *ov_new_conn(struct ov_test *t)
{
	union nf_inet_addr client = { .ip = htonl(0xc6120003) };
	struct ip_vs_conn_param p;
	struct ip_vs_conn *cp;

	ip_vs_conn_fill_param(t->ipvs, AF_INET, IPPROTO_UDP, &client,
			      htons(30000), &t->svc->addr, t->svc->port, &p);
	rcu_read_lock_bh();
	cp = ip_vs_conn_new(&p, AF_INET, &t->dest->addr, t->dest->port,
			    IP_VS_CONN_F_ONE_PACKET, t->dest, 0);
	rcu_read_unlock_bh();
	return cp;
}

static void ov_put_conn(struct ip_vs_conn *cp)
{
	if (cp) {
		rcu_read_lock_bh();
		ip_vs_conn_put(cp);
		rcu_read_unlock_bh();
	}
}

static bool ov_selected(struct ov_test *t)
{
	struct ip_vs_iphdr iph = { .protocol = IPPROTO_UDP };
	struct ip_vs_scheduler *sched;
	struct ip_vs_dest *selected;

	rcu_read_lock();
	sched = rcu_dereference(t->svc->scheduler);
	selected = sched->schedule(t->svc, NULL, &iph);
	rcu_read_unlock();
	return selected == t->dest;
}

static bool ov_wait(struct ov_gate *gate, unsigned int n)
{
	unsigned int spins = 0;

	while (smp_load_acquire(&gate->value) != n) {
		if (kthread_should_stop())
			return false;
		cpu_relax();
		if (!(++spins & 4095))
			cond_resched();
	}
	return true;
}

static int ov_editor(void *arg)
{
	struct ov_test *t = arg;
	unsigned int n;

	for (n = 1; n <= t->iterations; n++) {
		if (!ov_wait(&t->epoch, n))
			break;
		if (ov_edit(t, 100, t->lower ? 75 : 0))
			WRITE_ONCE(t->error, 1);
		smp_store_release(&t->edited.value, n);
		cond_resched();
	}
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 10 + 1);
	}
	return 0;
}

static int ov_accountant(void *arg)
{
	struct ov_test *t = arg;
	unsigned int n, delay, i;

	for (n = 1; n <= t->iterations; n++) {
		if (!ov_wait(&t->epoch, n))
			break;
		delay = t->delay_max ? (n * 2654435761U) % t->delay_max : 0;
		/* Vary arrival time before entering the original connection code. */
		for (i = 0; i < delay; i++)
			cpu_relax();
		if (t->lower) {
			ov_put_conn(t->racing);
			t->racing = NULL;
		} else {
			t->racing = ov_new_conn(t);
			if (!t->racing)
				WRITE_ONCE(t->error, 2);
		}
		smp_store_release(&t->accounted.value, n);
		cond_resched();
	}
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(HZ / 10 + 1);
	}
	return 0;
}

static int ov_fixture(struct ov_test *t)
{
	struct ip_vs_service_user_kern u = {
		.af = AF_INET,
		.protocol = IPPROTO_UDP,
		.addr.ip = htonl(0xc6120001),
		.port = htons(30001),
		.sched_name = "rr",
		.netmask = htonl(0xffffffff),
	};
	unsigned int i;
	int ret;

	t->ipvs = net_ipvs(&init_net);
	t->dest_user.af = AF_INET;
	t->dest_user.addr.ip = htonl(0xc6120002);
	t->dest_user.port = htons(30001);
	t->dest_user.weight = 1;
	t->dest_user.conn_flags = IP_VS_CONN_F_DROUTE | IP_VS_CONN_F_ONE_PACKET;
	t->dest_user.u_threshold = 200;
	mutex_lock(&t->ipvs->service_mutex);
	ret = ip_vs_add_service(t->ipvs, &u, &t->svc);
	if (!ret) {
		ret = ip_vs_add_dest(t->svc, &t->dest_user);
		if (!ret)
			t->dest = list_first_entry(&t->svc->destinations,
					  struct ip_vs_dest, n_list);
	}
	mutex_unlock(&t->ipvs->service_mutex);
	if (ret)
		return ret;
	t->base_count = t->lower ? 74 : 99;
	for (i = 0; i < t->base_count; i++) {
		t->base[i] = ov_new_conn(t);
		if (!t->base[i])
			return -ENOMEM;
	}
	return 0;
}

static void ov_check_persistence(struct ov_test *t)
{
	unsigned int i;
	struct ip_vs_conn *extra;
	int count;
	unsigned int flags;
	bool selected;

	if (t->lower) {
		for (i = 0; i < t->base_count; i++) {
			ov_put_conn(t->base[i]);
			t->base[i] = NULL;
		}
		count = atomic_read(&t->dest->totalconns);
		flags = READ_ONCE(t->dest->flags);
		selected = ov_selected(t);
		t->persistent_error = !count && (flags & IP_VS_DEST_F_OVERLOAD) &&
				      !selected;
		pr_info("OVTEST drained total=%d overload=%u selected=%u\n",
			count, !!(flags & IP_VS_DEST_F_OVERLOAD), selected);
		for (i = 0; i < t->base_count; i++) {
			t->base[i] = ov_new_conn(t);
			if (!t->base[i])
				WRITE_ONCE(t->error, 3);
		}
	} else {
		extra = ov_new_conn(t);
		if (!extra) {
			WRITE_ONCE(t->error, 4);
			return;
		}
		count = atomic_read(&t->dest->totalconns);
		flags = READ_ONCE(t->dest->flags);
		selected = ov_selected(t);
		t->persistent_error = count == 101 &&
				      !(flags & IP_VS_DEST_F_OVERLOAD) && selected;
		pr_info("OVTEST extra total=%d overload=%u selected=%u\n",
			count, !!(flags & IP_VS_DEST_F_OVERLOAD), selected);
		ov_put_conn(extra);
	}
}

static int ov_run(struct ov_test *t)
{
	struct task_struct *editor = NULL, *accountant = NULL;
	cpumask_var_t saved_mask;
	unsigned int n, i, spins, flags;
	unsigned int cpus[3], nr = 0, cpu;
	u64 start;
	int ret;

	if (!alloc_cpumask_var(&saved_mask, GFP_KERNEL))
		return -ENOMEM;
	cpumask_copy(saved_mask, current->cpus_ptr);
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		cpus[nr++] = cpu;
		if (nr == 3)
			break;
	}
	if (nr < 3) {
		ret = -ENODEV;
		goto out_mask;
	}
	ret = set_cpus_allowed_ptr(current, cpumask_of(cpus[0]));
	if (ret)
		goto out_mask;
	ret = ov_fixture(t);
	if (ret)
		goto out_fixture;
	editor = kthread_create(ov_editor, t, "ipvs-ov-edit");
	if (IS_ERR(editor)) {
		ret = PTR_ERR(editor);
		editor = NULL;
		goto out_fixture;
	}
	accountant = kthread_create(ov_accountant, t, "ipvs-ov-count");
	if (IS_ERR(accountant)) {
		ret = PTR_ERR(accountant);
		accountant = NULL;
		goto out_workers;
	}
	kthread_bind(editor, cpus[1]);
	kthread_bind(accountant, cpus[2]);
	wake_up_process(editor);
	wake_up_process(accountant);
	start = ktime_get_ns();
	pr_info("OVTEST start case=%s iterations=%u delay_max=%u cpus=%u,%u,%u\n",
		t->lower ? "unbind" : "bind", t->iterations, t->delay_max,
		cpus[0], cpus[1], cpus[2]);
	for (n = 1; n <= t->iterations; n++) {
		if (t->lower) {
			t->racing = ov_new_conn(t);
			if (!t->racing || ov_edit(t, 75, 50) || ov_edit(t, 100, 50)) {
				ret = -EINVAL;
				break;
			}
		} else if (ov_edit(t, 200, 0)) {
			ret = -EINVAL;
			break;
		}
		if (atomic_read(&t->dest->totalconns) != (t->lower ? 75 : 99) ||
		    !!(READ_ONCE(t->dest->flags) & IP_VS_DEST_F_OVERLOAD) != t->lower) {
			ret = -EUCLEAN;
			break;
		}
		smp_store_release(&t->epoch.value, n);
		spins = 0;
		while (smp_load_acquire(&t->edited.value) != n ||
		       smp_load_acquire(&t->accounted.value) != n) {
			cpu_relax();
			if (!(++spins & 4095))
				cond_resched();
		}
		t->completed++;
		if (t->error || atomic_read(&t->dest->totalconns) !=
		    (t->lower ? 74 : 100) || READ_ONCE(t->dest->u_threshold) != 100 ||
		    READ_ONCE(t->dest->l_threshold_val) != 75) {
			ret = -EUCLEAN;
			break;
		}
		flags = READ_ONCE(t->dest->flags);
		if (!!(flags & IP_VS_DEST_F_OVERLOAD) != !t->lower) {
			bool selected = ov_selected(t);

			t->misses++;
			if (selected == !t->lower)
				t->wrong_schedule++;
			if (t->misses <= 3)
				pr_info("OVTEST miss case=%s round=%u total=%d upper=%u lower=%u overload=%u selected=%u\n",
					t->lower ? "unbind" : "bind", n,
					atomic_read(&t->dest->totalconns),
					READ_ONCE(t->dest->u_threshold),
					READ_ONCE(t->dest->l_threshold_val),
					!!(flags & IP_VS_DEST_F_OVERLOAD), selected);
			if (t->misses == 1)
				ov_check_persistence(t);
		}
		ov_put_conn(t->racing);
		t->racing = NULL;
		if (t->error) {
			ret = -EUCLEAN;
			break;
		}
		cond_resched();
	}
	t->elapsed_ns = ktime_get_ns() - start;
out_workers:
	if (accountant)
		kthread_stop(accountant);
	if (editor)
		kthread_stop(editor);
out_fixture:
	ov_put_conn(t->racing);
	for (i = 0; i < t->base_count; i++)
		ov_put_conn(t->base[i]);
	if (t->svc) {
		mutex_lock(&t->ipvs->service_mutex);
		ip_vs_del_service(t->svc);
		mutex_unlock(&t->ipvs->service_mutex);
	}
	set_cpus_allowed_ptr(current, saved_mask);
out_mask:
	cpus_read_unlock();
	free_cpumask_var(saved_mask);
	return ret;
}

static ssize_t ov_write(struct file *file, const char __user *buf,
			size_t count, loff_t *pos)
{
	char input[80], which[16];
	struct ov_test *t;
	unsigned int iterations, delay;
	int ret;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (count >= sizeof(input) || copy_from_user(input, buf, count))
		return -EINVAL;
	input[count] = 0;
	if (sscanf(input, "%15s %u %u", which, &iterations, &delay) != 3 ||
	    (strcmp(which, "bind") && strcmp(which, "unbind")) ||
	    !iterations || iterations > 20000000 || delay > 4096)
		return -EINVAL;
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->lower = !strcmp(which, "unbind");
	t->iterations = iterations;
	t->delay_max = delay;
	mutex_lock(&ov_test_mutex);
	ret = ov_run(t);
	snprintf(ov_result, sizeof(ov_result),
		 "case=%s requested=%u completed=%u delay_max=%u misses=%u wrong_schedule=%u persistent_error=%u error=%u return=%d elapsed_ms=%llu\n",
		 which, iterations, t->completed, delay, t->misses,
		 t->wrong_schedule, t->persistent_error, t->error, ret,
		 div_u64(t->elapsed_ns, NSEC_PER_MSEC));
	pr_info("OVTEST result %s", ov_result);
	mutex_unlock(&ov_test_mutex);
	kfree(t);
	return ret ? ret : count;
}

static int ov_show(struct seq_file *seq, void *v)
{
	mutex_lock(&ov_test_mutex);
	seq_puts(seq, ov_result);
	mutex_unlock(&ov_test_mutex);
	return 0;
}

static int ov_open(struct inode *inode, struct file *file)
{
	return single_open(file, ov_show, NULL);
}

static const struct proc_ops ov_ops = {
	.proc_open = ov_open,
	.proc_read = seq_read,
	.proc_write = ov_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init ov_init(void)
{
	return proc_create("ipvs_overload_test", 0600, NULL, &ov_ops) ? 0 : -ENOMEM;
}
late_initcall(ov_init);
