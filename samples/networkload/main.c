/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bidirectional network check sample for the MANA driver.
 * The lwIP stack answers ICMP echo requests. This application answers TCP
 * probes on port 8080 with one line, then it closes the connection.
 * Set the static IPv4 configuration on the kernel command line:
 *   netdev.ip=<ip>/<maskbits>:<gateway>
 * See samples/networkload/README.md for the supported keys.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <uk/essentials.h>
#include <uk/mana.h>

#define CHECK_PORT 8080
#define CHECK_BACKLOG 8
static const char check_reply[] = "mana-ok\n";

int main(int argc __unused, char *argv[] __unused)
{
	struct sockaddr_in addr;
	const int reuse = 1;
	ssize_t written;
	int client;
	int err;
	int fd;

	printf("MANA netload %04x:%04x, Unikraft %s\n",
	       mana_build_info.pci_vendor_id, mana_build_info.pci_device_id,
	       mana_build_info.unikraft_revision);
	printf("netload: waiting for lwIP netif setup from netdev.ip=\n");

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("netload: socket failed: %d\n", errno);
		return 1;
	}
	err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (err != 0)
		printf("netload: SO_REUSEADDR failed: %d\n", errno);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(CHECK_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	err = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (err != 0) {
		printf("netload: bind on port %d failed: %d\n", CHECK_PORT,
		       errno);
		return 1;
	}
	err = listen(fd, CHECK_BACKLOG);
	if (err != 0) {
		printf("netload: listen on port %d failed: %d\n", CHECK_PORT,
		       errno);
		return 1;
	}
	printf("netload: TCP check server is ready on port %d\n", CHECK_PORT);

	for (;;) {
		client = accept(fd, NULL, NULL);
		if (client < 0) {
			printf("netload: accept failed: %d\n", errno);
			return 1;
		}
		written = write(client, check_reply, sizeof(check_reply) - 1);
		if (written != (ssize_t)(sizeof(check_reply) - 1))
			printf("netload: short reply: %d\n", (int)written);
		close(client);
		printf("netload: answered one TCP check on port %d\n",
		       CHECK_PORT);
	}
}
