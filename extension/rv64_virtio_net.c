#include "rv64_virtio.h"
#include <postgres.h>
#include <utils/elog.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#ifdef HAVE_LIBSLIRP
#include <libslirp.h>

/* Standard virtio_net_hdr */
struct virtio_net_hdr {
	uint8_t flags;
	uint8_t gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
} __attribute__((packed));

static slirp_ssize_t slirp_send_packet(const void *buf, size_t len, void *opaque)
{
	struct virtio_mmio_dev *dev = (struct virtio_mmio_dev *)opaque;

	if (dev->rx_head - dev->rx_tail >= 256) 
		return len; /* Ring completely full, drop packet */

	int idx;
	
	if (len > 2048) len = 2048; /* truncate */

	idx = dev->rx_head % 256;
	dev->rx_ring_sizes[idx] = len;
	memcpy(dev->rx_ring_buffers[idx], buf, len);
	dev->rx_head++;
	return len;
}

static void slirp_guest_error(const char *msg, void *opaque)
{
	elog(WARNING, "slirp guest error: %s", msg);
}

static int64_t slirp_clock_get_ns(void *opaque)
{
#ifdef __APPLE__
	return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

static void *slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque) { return NULL; }
static void slirp_timer_free(void *timer, void *opaque) {}
static void slirp_timer_mod(void *timer, int64_t expire_time, void *opaque) {}
static void slirp_notify(void *opaque) {}

static void slirp_register_poll_fd(int fd, void *opaque) {}
static void slirp_unregister_poll_fd(int fd, void *opaque) {}

static struct SlirpCb slirp_callbacks = {
	.send_packet = slirp_send_packet,
	.guest_error = slirp_guest_error,
	.clock_get_ns = slirp_clock_get_ns,
	.timer_new = slirp_timer_new,
	.timer_free = slirp_timer_free,
	.timer_mod = slirp_timer_mod,
	.notify = slirp_notify,
	.register_poll_fd = slirp_register_poll_fd,
	.unregister_poll_fd = slirp_unregister_poll_fd,
};
#endif

void virtio_net_init(struct virtio_mmio_dev *dev, uint32_t irq, uint8_t *ram, uint64_t ram_size)
{
	memset(dev, 0, sizeof(*dev));
	dev->device_id = VIRTIO_DEV_NET;
	dev->irq = irq;
	dev->ram = ram;
	dev->ram_size = ram_size;
	
#ifdef HAVE_LIBSLIRP
	SlirpConfig cfg = {0};
	struct in_addr host_addr = { .s_addr = htonl(INADDR_LOOPBACK) };
	struct in_addr guest_addr = { .s_addr = htonl(0x0A00020F) }; /* 10.0.2.15 */
	
	cfg.version = 1;
	cfg.restricted = 0;
	cfg.in_enabled = true;
	cfg.vnetwork.s_addr = htonl(0x0a000200); /* 10.0.2.0 */
	cfg.vnetmask.s_addr = htonl(0xffffff00); /* 255.255.255.0 */
	cfg.vhost.s_addr = htonl(0x0a000202);    /* 10.0.2.2 */
	cfg.in6_enabled = false;
	
	dev->slirp = slirp_new(&cfg, &slirp_callbacks, dev); 
	/* Forward Host localhost:54320 to Guest 10.0.2.15:5432 */
	slirp_add_hostfwd(dev->slirp, 0, host_addr, 54320, guest_addr, 5432);
	elog(NOTICE, "Virtio-Net: Port 54320 securely forwarded to guest 5432");
#endif
}

void virtio_net_process_queue(struct virtio_mmio_dev *dev, uint32_t qid, uint8_t *ram, uint64_t ram_size)
{
	if (qid == 1) {
		/* Transmit (Guest -> Host) */
		struct virtio_queue *vq = &dev->queues[qid];
		uint16_t avail_idx = 0;
		guest_read(ram, ram_size, vq->avail_addr + offsetof(struct vring_avail, idx), &avail_idx, 2);
		
		uint16_t start_avail = vq->last_avail;
		while (vq->last_avail != avail_idx) {
			uint16_t ring_idx = vq->last_avail % vq->num;
			uint16_t desc_head = 0;
			guest_read(ram, ram_size, vq->avail_addr + offsetof(struct vring_avail, ring) + ring_idx * 2, &desc_head, 2);

			struct vring_desc desc;
			guest_read(ram, ram_size, vq->desc_addr + desc_head * sizeof(desc), &desc, sizeof(desc));
			
			uint8_t pkt[65536];
			uint32_t pkt_len = 0;
			uint16_t cur = desc_head;
			
			while (1) {
				guest_read(ram, ram_size, vq->desc_addr + cur * sizeof(desc), &desc, sizeof(desc));
				if (pkt_len + desc.len <= sizeof(pkt)) {
					guest_read(ram, ram_size, desc.addr, pkt + pkt_len, desc.len);
					pkt_len += desc.len;
				}
				if (!(desc.flags & VRING_DESC_F_NEXT)) break;
				cur = desc.next;
			}
			
			if (pkt_len > 10) {
#ifdef HAVE_LIBSLIRP
				if (dev->slirp) {
	elog(LOG, "slirp_input called with len=%d", pkt_len - 10);
	slirp_input(dev->slirp, pkt + 10, pkt_len - 10);
	elog(LOG, "slirp_input returned");
}
#endif
			}
			
			uint16_t used_idx = 0;
			guest_read(ram, ram_size, vq->used_addr + offsetof(struct vring_used, idx), &used_idx, 2);
			uint16_t used_ring_idx = used_idx % vq->num;
			struct vring_used_elem elem = { .id = desc_head, .len = 0 };
			guest_write(ram, ram_size, vq->used_addr + offsetof(struct vring_used, ring) + used_ring_idx * sizeof(elem), &elem, sizeof(elem));
			used_idx++;
			guest_write(ram, ram_size, vq->used_addr + offsetof(struct vring_used, idx), &used_idx, 2);
			
			vq->last_avail++;
		}
		
		if (vq->last_avail != start_avail)
			dev->int_status |= VIRTIO_INT_USED_RING;
	}
}

#ifdef HAVE_LIBSLIRP
#include <poll.h>

struct slirp_poll_state {
	struct pollfd pfds[256];
	int count;
};

static int slirp_add_poll_cb(slirp_os_socket fd, int events, void *opaque) {
	struct slirp_poll_state *ps = opaque;
	if (ps->count >= 256) return -1;
	
	ps->pfds[ps->count].fd = fd;
	ps->pfds[ps->count].events = 0;
	if (events & SLIRP_POLL_IN) ps->pfds[ps->count].events |= POLLIN;
	if (events & SLIRP_POLL_OUT) ps->pfds[ps->count].events |= POLLOUT;
	if (events & SLIRP_POLL_PRI) ps->pfds[ps->count].events |= POLLPRI;
	ps->pfds[ps->count].revents = 0;
	
	return ps->count++;
}

static int slirp_get_revents_cb(int idx, void *opaque) {
	struct slirp_poll_state *ps = opaque;
	if (idx < 0 || idx >= ps->count) return 0;
	
	int revents = 0;
	short pr = ps->pfds[idx].revents;
	if (pr & POLLIN) revents |= SLIRP_POLL_IN;
	if (pr & POLLOUT) revents |= SLIRP_POLL_OUT;
	if (pr & POLLPRI) revents |= SLIRP_POLL_PRI;
	if (pr & POLLERR) revents |= SLIRP_POLL_ERR;
	if (pr & POLLHUP) revents |= SLIRP_POLL_HUP;
	
	return revents;
}
#endif

void virtio_net_poll(struct virtio_mmio_dev *dev)
{
#ifdef HAVE_LIBSLIRP
	struct slirp_poll_state ps;
	uint32_t timeout;
	int ret;
	struct virtio_queue *vq;

	if (!dev->slirp) return;
	
	ps.count = 0;
	timeout = 0; // immediate return
	
	slirp_pollfds_fill_socket(dev->slirp, &timeout, slirp_add_poll_cb, &ps);
elog(LOG, "virtio_net_poll: ps.count=%d, timeout=%u", ps.count, timeout);
	
	ret = poll(ps.pfds, ps.count, 0);
elog(LOG, "virtio_net_poll: poll returned %d", ret);
	
	slirp_pollfds_poll(dev->slirp, (ret < 0), slirp_get_revents_cb, &ps);

	/* Drain RX ring into Guest */
	vq = &dev->queues[0]; /* RX queue */
	if (vq->ready && dev->rx_head != dev->rx_tail) {
		uint16_t avail_idx = 0;
		guest_read(dev->ram, dev->ram_size, vq->avail_addr + offsetof(struct vring_avail, idx), &avail_idx, 2);
		
		int pushed = 0;
		while (dev->rx_head != dev->rx_tail && vq->last_avail != avail_idx) {
			int r_idx;
			size_t curlen;
			uint8_t *buf;
			uint16_t ring_idx;
			uint16_t desc_head;
			struct vring_desc desc;
			uint64_t w_addr;
			uint32_t w_len;
			struct virtio_net_hdr hdr;
			
			r_idx = dev->rx_tail % 256;
			curlen = dev->rx_ring_sizes[r_idx];
			buf = dev->rx_ring_buffers[r_idx];
			
			ring_idx = vq->last_avail % vq->num;
			desc_head = 0;
			guest_read(dev->ram, dev->ram_size, vq->avail_addr + offsetof(struct vring_avail, ring) + ring_idx * 2, &desc_head, 2);
			
			guest_read(dev->ram, dev->ram_size, vq->desc_addr + desc_head * sizeof(desc), &desc, sizeof(desc));
			
			/* Write header and payload */
			w_addr = desc.addr;
			w_len = desc.len;
			memset(&hdr, 0, sizeof(hdr));
			
			if (w_len >= sizeof(hdr)) {
				uint32_t payload_space = w_len - sizeof(hdr);
				size_t to_copy = (curlen > payload_space) ? payload_space : curlen;
				uint16_t used_idx = 0;
				uint16_t used_ring_idx;
				struct vring_used_elem elem;
				guest_write(dev->ram, dev->ram_size, w_addr, &hdr, sizeof(hdr));
				guest_write(dev->ram, dev->ram_size, w_addr + sizeof(hdr), buf, to_copy);
				
				guest_read(dev->ram, dev->ram_size, vq->used_addr + offsetof(struct vring_used, idx), &used_idx, 2);
				
				used_ring_idx = used_idx % vq->num;
				elem.id = desc_head;
				elem.len = sizeof(hdr) + to_copy;
				guest_write(dev->ram, dev->ram_size, vq->used_addr + offsetof(struct vring_used, ring) + used_ring_idx * sizeof(elem), &elem, sizeof(elem));
				
				used_idx++;
				guest_write(dev->ram, dev->ram_size, vq->used_addr + offsetof(struct vring_used, idx), &used_idx, 2);
				
				vq->last_avail++;
				pushed = 1;
				dev->rx_tail++;
			} else {
				dev->rx_tail++; /* drop packet if descriptor too small */
			}
		}
		if (pushed)
			dev->int_status |= VIRTIO_INT_USED_RING;
	}
#endif
}
