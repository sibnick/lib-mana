/* SPDX-License-Identifier: BSD-3-Clause */
/* Shared definitions for the MANA userspace VFIO runner. */

#ifndef MANA_VFIO_RUNNER_H
#define MANA_VFIO_RUNNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uk/mana/platform.h>

#define VFIO_BAR_COUNT 6U
#define VFIO_THP_SIZE 0x200000U

/* Gate exit codes. Usage errors exit with 2 and print no RUNNER_DONE line. */
#define VFIO_EXIT_OK 0
#define VFIO_EXIT_PROBE 1
#define VFIO_EXIT_GDMA 2
#define VFIO_EXIT_CONTEXT 3
#define VFIO_EXIT_PACKET 4

struct vfio_bar {
	uint64_t base;   /* current BAR address presented in config space */
	uint64_t size;   /* region size reported by VFIO */
	uint64_t offset; /* region offset inside the device fd */
	bool present;
};

enum vfio_irq_mode {
	VFIO_IRQ_MODE_NONE = 0,      /* no usable interrupt path yet */
	VFIO_IRQ_MODE_MSIX_EVENTFD,  /* kernel programs MSI-X, signals eventfd */
	VFIO_IRQ_MODE_POLL_FALLBACK  /* SET_IRQS failed; the driver polls */
};

struct vfio_ctx {
	int container_fd;
	int group_fd;
	int device_fd;
	int event_fd;                /* MSI-X eventfd, -1 when unused */
	enum vfio_irq_mode irq_mode;
	int irq_errno;               /* errno from the SET_IRQS probe */
	unsigned int msix_vectors;   /* table size from VFIO or the capability */
	uint16_t msix_cap;           /* MSI-X capability offset, 0 if none */
	uint16_t msix_ctrl_virt;     /* virtualized MSI-X Message Control */
	uint32_t msix_table;         /* capability dword: BIR plus offset */
	uint32_t msix_pba;           /* capability dword: BIR plus offset */
	struct vfio_bar bars[VFIO_BAR_COUNT];
	uint64_t config_offset;
	uint64_t config_size;
	void (*handler)(void *);     /* driver IRQ handler from irq_alloc */
	void *handler_arg;
	uint64_t irq_events;         /* deliveries passed to the handler */
	const char *stage;           /* current gate tag for messages */
	const char *wait_label;      /* what the in-flight call waits for */
	uint64_t deadline_ms;        /* absolute monotonic ms, 0 disables it */
	unsigned int timeout_ms;     /* per-call deadline width */
	uint64_t timeouts;           /* expired waits so far */
	unsigned int mmio_errors;    /* rejected or failed MMIO accesses */
	unsigned int unlocked;       /* DMA pages the kernel refused to mlock */
	uint64_t dma_allocs;         /* live DMA allocations */
	char last_op[24];            /* last failing platform operation */
	uint64_t last_off;           /* offset or size at the failure */
	int last_errno;              /* errno of that failure, 0 when none */
};

/* Platform and lifecycle, implemented in platform.c. */
const struct mana_platform *vfio_platform(struct vfio_ctx *ctx);
int vfio_open_device(struct vfio_ctx *ctx, const char *bdf);
void vfio_close_device(struct vfio_ctx *ctx);
void vfio_pump_irq(struct vfio_ctx *ctx);
void vfio_begin_wait(struct vfio_ctx *ctx, const char *what);
int vfio_irq_probe(struct vfio_ctx *ctx);

/* Config space and BAR access for the probe stage. */
int vfio_config_read(struct vfio_ctx *ctx, uint32_t off, unsigned int width,
		     uint32_t *value);
uint32_t vfio_bar_read32(struct vfio_ctx *ctx, unsigned int bar, uint64_t off);

/* DMA path checks that need no device. Returns a process exit code. */
int vfio_selftest(void);

#endif
