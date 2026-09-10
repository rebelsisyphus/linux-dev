/* Review-only probes; not part of the patch series. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "rfs_policy.h"
#include "rfs_policy.skel.h"

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL %s: errno=%d\n", what, errno);
		exit(1);
	}
	printf("PASS %s\n", what);
}

static int denied(int map, int link)
{
	struct __user_cap_header_struct hdr = {
		.version = _LINUX_CAPABILITY_VERSION_3,
	};
	struct __user_cap_data_struct caps[2] = {};
	pid_t pid;
	int status;

	fflush(NULL);
	pid = fork();
	if (pid < 0)
		return 0;
	if (!pid) {
		int ret;

		if (syscall(SYS_capget, &hdr, caps))
			_exit(2);
		caps[CAP_NET_ADMIN / 32].effective &= ~(1U << (CAP_NET_ADMIN % 32));
		if (syscall(SYS_capset, &hdr, caps))
			_exit(3);
		ret = link < 0 ? bpf_link_create(map, 0, BPF_STRUCT_OPS, NULL) :
			bpf_link_update(link, map, NULL);
		_exit(ret < 0 && errno == EPERM ? 0 : 4);
	}
	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0;
}

int main(void)
{
	struct rfs_topology topo = {};
	struct rfs_policy *a, *b, *other;
	unsigned int dev = if_nametoindex("review0");
	unsigned int dev2 = if_nametoindex("review1");
	int afd, bfd, otherfd, link, ret;

	require(dev && dev2, "devices found");
	require(!rfs_topology_load(&topo, "/sys/devices/system/cpu", "1,5", 0),
		"topology load");
	a = rfs_policy_open(&topo, dev, RFS_NUMA);
	b = rfs_policy_open(&topo, dev, RFS_PROCESS);
	other = rfs_policy_open(&topo, dev2, RFS_NUMA);
	require(a && b && other, "three policy objects opened");
	require(!rfs_policy_load(a, &topo) && !rfs_policy_prepare(a), "prepare a");
	require(!rfs_policy_load(b, &topo) && !rfs_policy_prepare(b), "prepare b");
	require(!rfs_policy_load(other, &topo) && !rfs_policy_prepare(other),
		"prepare different device");
	afd = bpf_map__fd(a->maps.rfs_ops);
	bfd = bpf_map__fd(b->maps.rfs_ops);
	otherfd = bpf_map__fd(other->maps.rfs_ops);
	require(denied(afd, -1), "attach without effective CAP_NET_ADMIN: EPERM");
	link = bpf_link_create(afd, 0, BPF_STRUCT_OPS, NULL);
	require(link >= 0, "authorized attach");
	ret = bpf_link_create(bfd, 0, BPF_STRUCT_OPS, NULL);
	require(ret < 0 && errno == EBUSY, "duplicate device: EBUSY");
	require(denied(bfd, link), "update without effective CAP_NET_ADMIN: EPERM");
	ret = bpf_link_update(link, otherfd, NULL);
	require(ret < 0 && errno == EINVAL, "update to different device: EINVAL");
	LIBBPF_OPTS(bpf_link_update_opts, opts,
		.flags = BPF_F_REPLACE,
		.old_map_fd = bfd,
	);
	ret = bpf_link_update(link, bfd, &opts);
	require(ret < 0 && errno == EPERM, "wrong expected-old map: EPERM");
	opts.old_map_fd = afd;
	require(!bpf_link_update(link, bfd, &opts), "valid expected-old update");
	require(!bpf_link_detach(link), "detach after update");
	close(link);
	link = bpf_link_create(afd, 0, BPF_STRUCT_OPS, NULL);
	require(link >= 0, "old map reusable after detach");
	require(!bpf_link_detach(link), "detach reused map");
	close(link);
	rfs_policy__destroy(a);
	rfs_policy__destroy(b);
	rfs_policy__destroy(other);
	rfs_topology_free(&topo);
	return 0;
}
