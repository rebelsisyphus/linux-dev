/*
 * qdisc_uaf_repro_loader.c
 *
 * Userspace C wrapper that loads the deterministic kernel-module reproducer
 * for the qdisc_pkt_len_segs_init use-after-free / out-of-bounds read.
 *
 * Usage: qdisc_uaf_repro_loader [simulate_fix]
 *   simulate_fix=0  -- trigger the bug (default)
 *   simulate_fix=1  -- verify the safety path
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __NR_finit_module
#define __NR_finit_module 313
#endif

int main(int argc, char **argv)
{
	const char *module_path = "/mnt/shared/qdisc_uaf_repro_mod.ko";
	const char *params = "simulate_fix=0";

	if (argc > 1) {
		if (strcmp(argv[1], "1") == 0 ||
		    strcmp(argv[1], "fix") == 0 ||
		    strcmp(argv[1], "fixed") == 0)
			params = "simulate_fix=1";
	}

	if (getuid() != 0) {
		fprintf(stderr, "Must run as root\n");
		return 1;
	}

	int fd = open(module_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(module_path);
		return 1;
	}

	printf("Loading %s with params '%s'...\n", module_path, params);
	fflush(stdout);

	long ret = syscall(__NR_finit_module, fd, params, 0);
	if (ret < 0) {
		perror("finit_module");
		close(fd);
		return 1;
	}

	close(fd);
	printf("Module loaded. If simulate_fix=0 the kernel should have oopsed.\n");
	return 0;
}
