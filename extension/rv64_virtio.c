/*
 * rv64_virtio.c — Virtio MMIO transport + virtio-blk device
 *
 * Implements the virtio 1.1 memory-mapped register interface and a
 * simple virtio-blk backend that reads/writes from an in-memory disk
 * image. The disk image is a flat byte array provided by the caller.
 */

#include "postgres.h"
#include "rv64_virtio.h"
#include "rv64_cpu.h"
#include <string.h>
#include <stdio.h>

/* ---- Init ---- */

void virtio_blk_init(struct virtio_mmio_dev *dev, uint32_t irq,
                     uint8_t *disk, uint64_t disk_size)
{
	memset(dev, 0, sizeof(*dev));
	dev->device_id = VIRTIO_DEV_BLK;
	dev->irq = irq;
	dev->disk_data = disk;
	dev->disk_size = disk_size;
	/* VERSION_1 required for modern virtio, BLK_SIZE advertises sector size */
	dev->dev_features = VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_BLK_SIZE;
}

/* ---- Guest memory helpers ---- */

/*
 * Read bytes from guest physical memory.
 * Returns 0 on success, -1 if out of bounds.
 */
int guest_read(uint8_t *ram, uint64_t ram_size,
               uint64_t addr, void *buf, size_t len)
{
	if (addr >= RAM_BASE && addr + len <= RAM_BASE + ram_size) {
		memcpy(buf, ram + (addr - RAM_BASE), len);
		return 0;
	}
	return -1;
}

int guest_write(uint8_t *ram, uint64_t ram_size,
                uint64_t addr, const void *buf, size_t len)
{
	if (addr >= RAM_BASE && addr + len <= RAM_BASE + ram_size) {
		memcpy(ram + (addr - RAM_BASE), buf, len);
		return 0;
	}
	return -1;
}

/* ---- Virtqueue processing ---- */

/*
 * Process all available descriptors in a virtio-blk request queue.
 * Each request is a chain of descriptors:
 *   [0] header (virtio_blk_req_hdr, device-readable)
 *   [1..n-1] data buffers
 *   [n] status byte (device-writable, 1 byte)
 */
static void virtio_blk_process_queue(struct virtio_mmio_dev *dev,
                                     struct virtio_queue *vq,
                                     uint8_t *ram, uint64_t ram_size)
{
	if (!vq->ready || !vq->desc_addr)
		return;

	/* Read the available ring index */
	uint16_t avail_idx;
	if (guest_read(ram, ram_size,
	               vq->avail_addr + offsetof(struct vring_avail, idx),
	               &avail_idx, 2) < 0)
		return;

	uint16_t start_avail = vq->last_avail;

	while (vq->last_avail != avail_idx) {
		/* Get the head descriptor index from available ring */
		uint16_t ring_idx = vq->last_avail % vq->num;
		uint16_t desc_head;
		if (guest_read(ram, ram_size,
		               vq->avail_addr +
		               offsetof(struct vring_avail, ring) +
		               ring_idx * 2,
		               &desc_head, 2) < 0)
			break;

		/* Walk the descriptor chain */
		struct virtio_blk_req_hdr hdr;
		uint8_t status = VIRTIO_BLK_S_OK;
		int phase = 0;  /* 0=header, 1=data, 2=status */
		uint16_t idx = desc_head;
		uint32_t total_bytes = 0;

		for (int i = 0; i < vq->num; i++) {  /* safety limit */
			struct vring_desc desc;
			if (guest_read(ram, ram_size,
			               vq->desc_addr + idx * sizeof(desc),
			               &desc, sizeof(desc)) < 0) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}

			if (phase == 0) {
				/* Header descriptor (device-readable) */
				if (desc.len < sizeof(hdr)) {
					status = VIRTIO_BLK_S_IOERR;
					break;
				}
				if (guest_read(ram, ram_size, desc.addr,
				               &hdr, sizeof(hdr)) < 0) {
					status = VIRTIO_BLK_S_IOERR;
					break;
				}
				phase = 1;
			} else if (!(desc.flags & VRING_DESC_F_NEXT) ||
			           (desc.flags & VRING_DESC_F_WRITE &&
			            desc.len == 1)) {
				/*
				 * Last descriptor in chain = status byte.
				 * Or: single-byte writable descriptor.
				 */
				phase = 2;
			} else {
				/* Data descriptor */
				uint64_t disk_offset = hdr.sector * 512;

				if (hdr.type == VIRTIO_BLK_T_IN) {
					/* Read from disk to guest */
					if (disk_offset + desc.len > dev->disk_size) {
						elog(LOG, "VIRTIO_BLK_T_IN IOERR: offset=%lu len=%u size=%lu", disk_offset, desc.len, dev->disk_size);
						status = VIRTIO_BLK_S_IOERR;
					} else {
						if (guest_write(ram, ram_size, desc.addr,
						                dev->disk_data + disk_offset,
						                desc.len) < 0) {
							elog(LOG, "VIRTIO_BLK_T_IN IOERR: guest_write failed addr=%lx len=%u", desc.addr, desc.len);
							status = VIRTIO_BLK_S_IOERR;
						} else {
							total_bytes += desc.len;
							dev->blk_reads++;
							dev->blk_bytes_read += desc.len;
						}
					}
					hdr.sector += desc.len / 512;
				} else if (hdr.type == VIRTIO_BLK_T_OUT) {
					/* Write from guest to disk */
					if (disk_offset + desc.len > dev->disk_size) {
						elog(LOG, "VIRTIO_BLK_T_OUT IOERR: offset=%lu len=%u size=%lu", disk_offset, desc.len, dev->disk_size);
						status = VIRTIO_BLK_S_IOERR;
					} else {
						if (guest_read(ram, ram_size, desc.addr,
					               dev->disk_data + disk_offset,
					               desc.len) < 0) {
							elog(LOG, "VIRTIO_BLK_T_OUT IOERR: guest_read failed addr=%lx len=%u", desc.addr, desc.len);
							status = VIRTIO_BLK_S_IOERR;
						} else {
						total_bytes += desc.len;
						dev->blk_writes++;
						dev->blk_bytes_written += desc.len;
						/* Notify dirty tracker */
						if (dev->mark_dirty)
							dev->mark_dirty(disk_offset, desc.len,
							                dev->mark_dirty_opaque);
					}
					}
					hdr.sector += desc.len / 512;
				} else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
					/* No-op for in-memory disk */
					status = VIRTIO_BLK_S_OK;
				} else {
					elog(LOG, "VIRTIO_BLK_T_UNSUPP: type=%u", hdr.type);
					status = VIRTIO_BLK_S_UNSUPP;
				}
			}

			if (!(desc.flags & VRING_DESC_F_NEXT))
				break;
			idx = desc.next;
		}

		/* Write status byte to the last descriptor */
		if (phase >= 1) {
			/* Find the last descriptor again to write status */
			idx = desc_head;
			struct vring_desc desc;
			for (int i = 0; i < vq->num; i++) {
				guest_read(ram, ram_size,
				           vq->desc_addr + idx * sizeof(desc),
				           &desc, sizeof(desc));
				if (!(desc.flags & VRING_DESC_F_NEXT))
					break;
				idx = desc.next;
			}
			/* Write status to the last descriptor's buffer */
			int gw_res = guest_write(ram, ram_size, desc.addr, &status, 1);
			if (gw_res < 0)
				elog(LOG, "VIRTIO_BLK IOERR: guest_write for status failed addr=%lx", desc.addr);
			
			elog(LOG, "VIRTIO_BLK completed req: type=%u sector=%lu bytes=%u status=%u desc_head=%u gw_res=%d",
				hdr.type, hdr.sector - (total_bytes/512), total_bytes, status, desc_head, gw_res);
		}

		/* Add to used ring */
		uint16_t used_idx = 0;
		guest_read(ram, ram_size,
		           vq->used_addr + offsetof(struct vring_used, idx),
		           &used_idx, 2);
		uint16_t used_ring_idx = used_idx % vq->num;
		struct vring_used_elem elem = {
			.id = desc_head,
			.len = total_bytes
		};
		guest_write(ram, ram_size,
		            vq->used_addr +
		            offsetof(struct vring_used, ring) +
		            used_ring_idx * sizeof(elem),
		            &elem, sizeof(elem));
		used_idx++;
		guest_write(ram, ram_size,
		            vq->used_addr + offsetof(struct vring_used, idx),
		            &used_idx, 2);

		vq->last_avail++;
	}

	/* Signal interrupt if we processed new descriptors */
	if (vq->last_avail != start_avail)
		dev->int_status |= VIRTIO_INT_USED_RING;
}

/* ---- MMIO register access ---- */

uint64_t virtio_mmio_load(struct virtio_mmio_dev *dev, uint64_t offset,
                          int size)
{
	/* Config space (offset >= 0x100) */
	if (offset >= VIRTIO_MMIO_CONFIG) {
		uint64_t cfg_off = offset - VIRTIO_MMIO_CONFIG;
		if (dev->device_id == VIRTIO_DEV_BLK) {
			struct virtio_blk_config cfg = {
				.capacity = dev->disk_size / 512,
			};
			/* Also report blk_size at offset 20 (4 bytes) */
			uint8_t cfg_buf[24];
			memset(cfg_buf, 0, sizeof(cfg_buf));
			memcpy(cfg_buf, &cfg, sizeof(cfg));
			/* blk_size at offset 20 = 512 */
			uint32_t blk_size = 512;
			memcpy(cfg_buf + 20, &blk_size, 4);

			if (cfg_off + size <= sizeof(cfg_buf)) {
				uint64_t val = 0;
				memcpy(&val, cfg_buf + cfg_off, size);
				return val;
			}
		}
		return 0;
	}

	switch (offset) {
	case VIRTIO_MMIO_MAGIC:
		return VIRTIO_MMIO_MAGIC_VALUE;
	case VIRTIO_MMIO_VERSION:
		return 2;  /* modern virtio-mmio */
	case VIRTIO_MMIO_DEVICE_ID:
		return dev->device_id;
	case VIRTIO_MMIO_VENDOR_ID:
		return 0x554D4551;  /* "QEMU" */
	case VIRTIO_MMIO_DEV_FEATURES:
		if (dev->dev_features_sel == 0)
			return (uint32_t)dev->dev_features;
		else if (dev->dev_features_sel == 1)
			return (uint32_t)(dev->dev_features >> 32);
		return 0;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		return VIRTIO_QUEUE_SIZE_MAX;
	case VIRTIO_MMIO_QUEUE_READY: {
		int qi = dev->queue_sel;
		if (qi < 4) return dev->queues[qi].ready;
		return 0;
	}
	case VIRTIO_MMIO_INT_STATUS:
		return dev->int_status;
	case VIRTIO_MMIO_STATUS:
		return dev->status;
	case VIRTIO_MMIO_CONFIG_GEN:
		return dev->config_gen;
	default:
		return 0;
	}
}

void virtio_mmio_store(struct virtio_mmio_dev *dev, uint64_t offset,
                       uint64_t val, int size, uint8_t *ram,
                       uint64_t ram_size)
{
	int qi = dev->queue_sel;

	switch (offset) {
	case VIRTIO_MMIO_DEV_FEATURES_SEL:
		dev->dev_features_sel = (uint32_t)val;
		break;
	case VIRTIO_MMIO_DRV_FEATURES:
		if (dev->drv_features_sel == 0)
			dev->drv_features = (dev->drv_features & 0xFFFFFFFF00000000ULL) | (val & 0xFFFFFFFF);
		else if (dev->drv_features_sel == 1)
			dev->drv_features = (dev->drv_features & 0xFFFFFFFF) | ((val & 0xFFFFFFFF) << 32);
		break;
	case VIRTIO_MMIO_DRV_FEATURES_SEL:
		dev->drv_features_sel = (uint32_t)val;
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		dev->queue_sel = (uint32_t)val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		if (qi < 4)
			dev->queues[qi].num = (uint32_t)val;
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		if (qi < 4)
			dev->queues[qi].ready = (uint32_t)val;
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		/* Driver is notifying us: process the specified queue */
		if (val < 4 && dev->queues[val].ready) {
			if (dev->device_id == VIRTIO_DEV_BLK) {
				virtio_blk_process_queue(dev, &dev->queues[val],
				                         ram, ram_size);
			} else if (dev->device_id == VIRTIO_DEV_NET) {
				virtio_net_process_queue(dev, val, ram, ram_size);
			}
		}
		break;
	case VIRTIO_MMIO_INT_ACK:
		dev->int_status &= ~(uint32_t)val;
		break;
	case VIRTIO_MMIO_STATUS:
		dev->status = (uint32_t)val;
		if (val == 0) {
			/* Driver reset — clear all state except identity */
			uint32_t id = dev->device_id;
			uint32_t irq = dev->irq;
			uint8_t *disk = dev->disk_data;
			uint64_t dsz = dev->disk_size;
			uint64_t feat = dev->dev_features;
			memset(dev, 0, sizeof(*dev));
			dev->device_id = id;
			dev->irq = irq;
			dev->disk_data = disk;
			dev->disk_size = dsz;
			dev->dev_features = feat;
		}
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		if (qi < 4) {
			dev->queues[qi].desc_addr &= 0xFFFFFFFF00000000ULL;
			dev->queues[qi].desc_addr |= (val & 0xFFFFFFFF);
		}
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		if (qi < 4) {
			dev->queues[qi].desc_addr &= 0xFFFFFFFF;
			dev->queues[qi].desc_addr |= ((val & 0xFFFFFFFF) << 32);
		}
		break;
	case VIRTIO_MMIO_QUEUE_DRV_LOW:
		if (qi < 4) {
			dev->queues[qi].avail_addr &= 0xFFFFFFFF00000000ULL;
			dev->queues[qi].avail_addr |= (val & 0xFFFFFFFF);
		}
		break;
	case VIRTIO_MMIO_QUEUE_DRV_HIGH:
		if (qi < 4) {
			dev->queues[qi].avail_addr &= 0xFFFFFFFF;
			dev->queues[qi].avail_addr |= ((val & 0xFFFFFFFF) << 32);
		}
		break;
	case VIRTIO_MMIO_QUEUE_DEV_LOW:
		if (qi < 4) {
			dev->queues[qi].used_addr &= 0xFFFFFFFF00000000ULL;
			dev->queues[qi].used_addr |= (val & 0xFFFFFFFF);
		}
		break;
	case VIRTIO_MMIO_QUEUE_DEV_HIGH:
		if (qi < 4) {
			dev->queues[qi].used_addr &= 0xFFFFFFFF;
			dev->queues[qi].used_addr |= ((val & 0xFFFFFFFF) << 32);
		}
		break;
	default:
		break;
	}
}
