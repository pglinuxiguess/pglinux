#ifndef RV64_VIRTIO_H
#define RV64_VIRTIO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Virtio MMIO transport + virtio-blk device.
 *
 * Reference: Virtual I/O Device (VIRTIO) Version 1.1, Section 4.2 (MMIO)
 *            and Section 5.2 (Block Device).
 *
 * The virtio-mmio register interface is at a 4KB-aligned MMIO region.
 * The virtio-blk config space starts at offset 0x100 within that region.
 */

/* ---- Virtio MMIO register offsets (Section 4.2.2) ---- */
#define VIRTIO_MMIO_MAGIC          0x000  /* R:  0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION        0x004  /* R:  device version (2 = modern) */
#define VIRTIO_MMIO_DEVICE_ID      0x008  /* R:  device type ID */
#define VIRTIO_MMIO_VENDOR_ID      0x00C  /* R:  vendor ID */
#define VIRTIO_MMIO_DEV_FEATURES   0x010  /* R:  device feature bits (sel) */
#define VIRTIO_MMIO_DEV_FEATURES_SEL 0x014 /* W: which feature word */
#define VIRTIO_MMIO_DRV_FEATURES   0x020  /* W:  driver accepted features */
#define VIRTIO_MMIO_DRV_FEATURES_SEL 0x024 /* W: which feature word */
#define VIRTIO_MMIO_QUEUE_SEL      0x030  /* W:  select queue index */
#define VIRTIO_MMIO_QUEUE_NUM_MAX  0x034  /* R:  max queue size */
#define VIRTIO_MMIO_QUEUE_NUM      0x038  /* W:  current queue size */
#define VIRTIO_MMIO_QUEUE_READY    0x044  /* RW: queue ready */
#define VIRTIO_MMIO_QUEUE_NOTIFY   0x050  /* W:  notify queue (queue idx) */
#define VIRTIO_MMIO_INT_STATUS     0x060  /* R:  interrupt status */
#define VIRTIO_MMIO_INT_ACK        0x064  /* W:  acknowledge interrupt */
#define VIRTIO_MMIO_STATUS         0x070  /* RW: device status */
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080 /* W: descriptor table addr low */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084 /* W: descriptor table addr high */
#define VIRTIO_MMIO_QUEUE_DRV_LOW   0x090 /* W: available ring addr low */
#define VIRTIO_MMIO_QUEUE_DRV_HIGH  0x094 /* W: available ring addr high */
#define VIRTIO_MMIO_QUEUE_DEV_LOW   0x0A0 /* W: used ring addr low */
#define VIRTIO_MMIO_QUEUE_DEV_HIGH  0x0A4 /* W: used ring addr high */
#define VIRTIO_MMIO_CONFIG_GEN     0x0FC  /* R:  config generation counter */
#define VIRTIO_MMIO_CONFIG         0x100  /* R:  device-specific config */

/* Magic value */
#define VIRTIO_MMIO_MAGIC_VALUE    0x74726976

/* Device IDs */
#define VIRTIO_DEV_NET   1
#define VIRTIO_DEV_BLK   2

/* Device status bits */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* Feature bits (virtio-blk) */
#define VIRTIO_BLK_F_SIZE_MAX     (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX      (1ULL << 2)
#define VIRTIO_BLK_F_GEOMETRY     (1ULL << 4)
#define VIRTIO_BLK_F_RO           (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE     (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH        (1ULL << 9)
/* Common feature bits */
#define VIRTIO_F_VERSION_1        (1ULL << 32)

/* Interrupt status bits */
#define VIRTIO_INT_USED_RING      1
#define VIRTIO_INT_CONFIG_CHANGE  2

/* ---- Virtqueue structures (in guest memory) ---- */

/* Descriptor flags */
#define VRING_DESC_F_NEXT    1  /* descriptor has next field */
#define VRING_DESC_F_WRITE   2  /* device writes (vs reads) */

/* Descriptor entry (16 bytes) */
struct vring_desc {
	uint64_t addr;   /* guest-physical address */
	uint32_t len;    /* length in bytes */
	uint16_t flags;  /* VRING_DESC_F_* */
	uint16_t next;   /* next descriptor if NEXT flag set */
} __attribute__((packed));

/* Available ring header */
struct vring_avail {
	uint16_t flags;
	uint16_t idx;      /* next entry driver will write */
	uint16_t ring[];   /* descriptor chain heads */
} __attribute__((packed));

/* Used ring element */
struct vring_used_elem {
	uint32_t id;       /* descriptor chain head */
	uint32_t len;      /* bytes written by device */
} __attribute__((packed));

/* Used ring header */
struct vring_used {
	uint16_t flags;
	uint16_t idx;      /* next entry device will write */
	struct vring_used_elem ring[];
} __attribute__((packed));

/* ---- Virtio-blk request header ---- */
#define VIRTIO_BLK_T_IN   0   /* read */
#define VIRTIO_BLK_T_OUT  1   /* write */
#define VIRTIO_BLK_T_FLUSH 4  /* flush */

#define VIRTIO_BLK_S_OK       0
#define VIRTIO_BLK_S_IOERR    1
#define VIRTIO_BLK_S_UNSUPP   2

struct virtio_blk_req_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
} __attribute__((packed));

/* ---- Virtio-blk config space (at MMIO offset 0x100) ---- */
struct virtio_blk_config {
	uint64_t capacity;    /* disk size in 512-byte sectors */
} __attribute__((packed));

/* ---- Per-queue state ---- */
#define VIRTIO_QUEUE_SIZE_MAX  256

struct virtio_queue {
	uint32_t num;         /* current queue size (set by driver) */
	uint32_t ready;       /* 1 = queue is ready */
	uint64_t desc_addr;   /* guest-physical: descriptor table */
	uint64_t avail_addr;  /* guest-physical: available ring */
	uint64_t used_addr;   /* guest-physical: used ring */
	uint16_t last_avail;  /* last index we consumed from avail ring */
};

/* ---- Virtio MMIO device state ---- */
struct virtio_mmio_dev {
	/* Device identity */
	uint32_t device_id;      /* VIRTIO_DEV_BLK, VIRTIO_DEV_NET, etc */
	uint32_t irq;            /* PLIC IRQ number for this device */

	/* MMIO registers */
	uint32_t status;         /* device status */
	uint32_t int_status;     /* pending interrupt bits */
	uint32_t dev_features_sel;  /* which 32-bit word of features */
	uint32_t drv_features_sel;
	uint64_t dev_features;   /* features we offer */
	uint64_t drv_features;   /* features driver accepted */
	uint32_t queue_sel;      /* selected queue index */
	uint32_t config_gen;     /* config generation counter */

	/* Queues (virtio-blk uses 1 queue, virtio-net uses 2+) */
	struct virtio_queue queues[4];

	/* Block device state */
	uint8_t *disk_data;      /* pointer to disk image in memory */
	uint64_t disk_size;      /* disk size in bytes */

	/* Net specific state */
	void *slirp;
	uint8_t *ram;
	uint64_t ram_size;
	
	/* RX Packet Ring */
	uint8_t rx_ring_buffers[256][2048];
	size_t rx_ring_sizes[256];
	int rx_head;
	int rx_tail;

	/*
	 * Optional dirty-block callback: called on every write with the
	 * starting sector and byte length. rv64_pgext hooks this to set
	 * bits in vm_disk_dirty for write-back persistence.
	 */
	void (*mark_dirty)(uint64_t byte_offset, uint64_t length, void *opaque);
	void *mark_dirty_opaque;

	/* I/O profiling counters */
	uint64_t blk_reads;
	uint64_t blk_writes;
	uint64_t blk_bytes_read;
	uint64_t blk_bytes_written;
};

/* ---- API ---- */

/* Initialize a virtio-blk device */
void virtio_blk_init(struct virtio_mmio_dev *dev, uint32_t irq,
                     uint8_t *disk, uint64_t disk_size);

/* Initialize a virtio-net device */
void virtio_net_init(struct virtio_mmio_dev *dev, uint32_t irq, uint8_t *ram, uint64_t ram_size);
void virtio_net_process_queue(struct virtio_mmio_dev *dev, uint32_t qid, uint8_t *ram, uint64_t ram_size);
void virtio_net_poll(struct virtio_mmio_dev *dev);

/* Guest RAM access helpers */
int guest_read(uint8_t *ram, uint64_t ram_size, uint64_t addr, void *buf, size_t len);
int guest_write(uint8_t *ram, uint64_t ram_size, uint64_t addr, const void *buf, size_t len);

/* MMIO register read/write */
uint64_t virtio_mmio_load(struct virtio_mmio_dev *dev, uint64_t offset,
                          int size);
void virtio_mmio_store(struct virtio_mmio_dev *dev, uint64_t offset,
                       uint64_t val, int size, uint8_t *ram,
                       uint64_t ram_size);

#endif /* RV64_VIRTIO_H */
