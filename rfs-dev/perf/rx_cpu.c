/* SPDX-License-Identifier: GPL-2.0 */
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct bpf_object *obj;
	struct bpf_link *link;
	unsigned long long *values;
	unsigned int key = 0;
	int nr, fd, i;

	if (argc != 3)
		return 1;
	obj = bpf_object__open_file(argv[1], NULL);
	if (libbpf_get_error(obj) || bpf_object__load(obj))
		return 1;
	link = bpf_program__attach(bpf_object__find_program_by_name(obj, "count_rx"));
	if (libbpf_get_error(link))
		return 1;
	nr = libbpf_num_possible_cpus();
	if (nr <= 0)
		return 1;
	values = calloc(nr, sizeof(*values));
	fd = bpf_object__find_map_fd_by_name(obj, "rx_counts");
	if (!values || fd < 0)
		return 1;
	fprintf(stderr, "RX tracer attached\n");
	sleep(atoi(argv[2]));
	if (bpf_map_lookup_elem(fd, &key, values))
		return 1;
	bpf_link__destroy(link);
	printf("[");
	for (i = 0; i < nr; i++)
		printf("%s%llu", i ? "," : "", values[i]);
	puts("]");
	bpf_object__close(obj);
	free(values);
	return 0;
}
