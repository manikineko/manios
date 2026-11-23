/* TCP (RFC 9293), the client side: active open, in-order delivery,
 * retransmission with exponential back-off, flow control in both
 * directions and an orderly close. Enough to fetch from a web server.
 *
 * Design. Each connection is a vnode a program reads and writes like a
 * pipe end. Segments arrive on the network thread (tcp_input), which
 * only updates state and wakes waiters: it never transmits, because
 * sending may wait for ARP. Everything that goes out -- SYN, data, ACKs,
 * FIN, retransmissions -- is sent by one "tcp" kernel thread, which also
 * keeps the timers. Interrupts off guard the connection list and every
 * connection's state, as in udp.c.
 *
 * Simplifications, each safe for a client: segments arriving out of
 * order are dropped and re-acknowledged (the peer retransmits them); no
 * congestion window beyond the peer's window; no listening sockets; a
 * connection that has been closed by its owner is forgotten once both
 * FINs are acknowledged, or after TCP_ORPHAN_MS. */
#include "tcp.h"
#include <stdbool.h>
#include "cpu.h"
#include "heap.h"
#include "kerrno.h"
#include "kprintf.h"
#include "kstring.h"
#include "net.h"
#include "poll.h"
#include "sched.h"
#include "timer.h"
#include "vfs.h"

#define PROTO_TCP 6
#define TCP_HEADER 20
#define TCP_MSS (ETH_MTU - IP_HEADER - TCP_HEADER) /* 1460: what we accept */
#define TCP_MSS_DEFAULT 536                          /* without the option */
#define SEND_BUF (32 * 1024)
#define RECV_BUF (32 * 1024)
#define RTO_INITIAL_MS 1000
#define RTO_MAX_MS 16000
#define RETRIES_MAX 8
#define TIME_WAIT_MS 2000
#define TCP_ORPHAN_MS 30000
#define TICK_MS 20
#define EPHEMERAL_FIRST 49152

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum tcp_state {
	SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT,
	CLOSED,
};

struct tcp_conn {
	struct vnode vnode; /* first: the vnode is the connection */
	enum tcp_state state;
	uint32_t local_ip, remote_ip;
	uint16_t local_port, remote_port;
	int error;      /* the reason the connection failed, or 0 */
	bool released;  /* the owner closed it: an orphan the thread finishes */

	/* Sending. snd_una is the first unacknowledged sequence number;
	 * sbuf holds the data from there on (sbuf_len bytes, from
	 * sbuf_head), of which `sent` are in flight. */
	uint32_t iss, snd_una, snd_wnd;
	uint16_t mss;
	uint8_t sbuf[SEND_BUF];
	size_t sbuf_head, sbuf_len, sent;
	bool syn_sent, fin_queued, fin_sent, fin_acked;

	/* Receiving: rcv_nxt is the next sequence number expected; rbuf
	 * holds what the owner hasn't read yet. */
	uint32_t rcv_nxt;
	uint8_t rbuf[RECV_BUF];
	size_t rbuf_head, rbuf_len;
	bool peer_fin, ack_pending;
	uint32_t window_advertised;

	/* Timers, in milliseconds of uptime. */
	uint64_t rto_at;     /* retransmit (0: nothing outstanding) */
	uint32_t rto_ms;
	int retries;
	uint64_t done_at;    /* TIME_WAIT end, or orphan give-up */

	struct waitq changed; /* readable, writable, state: one queue */
	struct tcp_conn *next;
};

struct tcp_stats tcp_stats;

static struct tcp_conn *conns;
static uint16_t next_ephemeral = EPHEMERAL_FIRST;
static struct waitq work = WAITQ_INIT;
static uint32_t iss_counter;

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static void kick(void)
{
	waitq_wake_all(&work);
}

static void wake(struct tcp_conn *c)
{
	waitq_wake_all(&c->changed);
	poll_notify();
}

static void fail(struct tcp_conn *c, int err)
{
	if (!c->error) {
		c->error = err;
	}
	c->state = CLOSED;
	c->rto_at = 0;
	wake(c);
}

/* --- receiving segments (network thread) --- */

static struct tcp_conn *find(uint32_t remote, uint16_t rport, uint16_t lport)
{
	for (struct tcp_conn *c = conns; c; c = c->next) {
		if (c->local_port == lport && c->remote_port == rport && c->remote_ip == remote) {
			return c;
		}
	}
	return 0;
}

static uint16_t parse_mss(const uint8_t *opt, size_t len)
{
	for (size_t i = 0; i < len;) {
		if (opt[i] == 0) {
			break;
		}
		if (opt[i] == 1) {
			i++;
			continue;
		}
		if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) {
			break;
		}
		if (opt[i] == 2 && opt[i + 1] == 4) {
			return get_be16(opt + i + 2);
		}
		i += opt[i + 1];
	}
	return TCP_MSS_DEFAULT;
}

/* The bytes of our sequence space an ACK can cover: the SYN, the data
 * in flight, the FIN. Applies an acceptable ACK. */
static void apply_ack(struct tcp_conn *c, uint32_t ack)
{
	if (!seq_lt(c->snd_una, ack)) {
		return; /* old or duplicate */
	}
	uint32_t top = c->snd_una + (uint32_t)c->sent + (c->fin_sent ? 1 : 0);
	if (seq_lt(top, ack)) {
		return; /* acknowledges what we never sent */
	}
	uint32_t n = ack - c->snd_una;
	size_t data = n > c->sent ? c->sent : n;
	c->sbuf_head = (c->sbuf_head + data) % SEND_BUF;
	c->sbuf_len -= data;
	c->sent -= data;
	if (c->fin_sent && n > data) {
		c->fin_acked = true;
	}
	c->snd_una = ack;
	c->retries = 0;
	c->rto_ms = RTO_INITIAL_MS;
	c->rto_at = (c->sent || (c->fin_sent && !c->fin_acked)) ? timer_uptime_ms() + c->rto_ms : 0;
	wake(c);
}

static void input(struct tcp_conn *c, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                  const uint8_t *opt, size_t optlen, const uint8_t *payload, size_t plen)
{
	if (c->state == CLOSED) {
		return;
	}
	if (c->state == SYN_SENT) {
		if ((flags & F_ACK) && ack != c->iss + 1) {
			return; /* not an answer to our SYN */
		}
		if (flags & F_RST) {
			if (flags & F_ACK) {
				fail(c, -ECONNREFUSED);
			}
			return;
		}
		if ((flags & (F_SYN | F_ACK)) != (F_SYN | F_ACK)) {
			return; /* simultaneous open: not for a client */
		}
		c->rcv_nxt = seq + 1;
		c->snd_una = ack;
		c->snd_wnd = window;
		c->mss = parse_mss(opt, optlen);
		if (c->mss > TCP_MSS) {
			c->mss = TCP_MSS;
		}
		if (c->mss < 64) {
			c->mss = 64;
		}
		c->state = ESTABLISHED;
		c->rto_at = 0;
		c->retries = 0;
		c->ack_pending = true;
		wake(c);
		kick();
		return;
	}

	/* Synchronized states. A segment is acceptable when it starts at or
	 * before rcv_nxt and something of it lies at or after (RFC 9293
	 * 3.10.7.4, simplified for in-order delivery). */
	if (flags & F_RST) {
		if (seq == c->rcv_nxt || (seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + RECV_BUF))) {
			tcp_stats.reset++;
			fail(c, -ECONNRESET);
		}
		return;
	}
	if (flags & F_SYN) {
		c->ack_pending = true; /* a retransmitted SYN-ACK: our ACK was lost */
		kick();
		return;
	}
	if (flags & F_ACK) {
		apply_ack(c, ack);
		c->snd_wnd = window;
		if (c->fin_acked) {
			if (c->state == FIN_WAIT_1) {
				c->state = FIN_WAIT_2;
			} else if (c->state == CLOSING) {
				c->state = TIME_WAIT;
				c->done_at = timer_uptime_ms() + TIME_WAIT_MS;
			} else if (c->state == LAST_ACK) {
				c->state = CLOSED;
			}
			wake(c);
		}
	}

	bool in_order = seq == c->rcv_nxt;
	if (plen) {
		if (in_order && !c->peer_fin) {
			size_t room = RECV_BUF - c->rbuf_len;
			size_t take = plen < room ? plen : room;
			for (size_t i = 0; i < take; i++) {
				c->rbuf[(c->rbuf_head + c->rbuf_len + i) % RECV_BUF] = payload[i];
			}
			c->rbuf_len += take;
			c->rcv_nxt += (uint32_t)take;
			in_order = take == plen;
			if (take) {
				wake(c);
			}
		}
		c->ack_pending = true;
	}
	if ((flags & F_FIN) && in_order && seq + (uint32_t)plen == c->rcv_nxt && !c->peer_fin) {
		c->rcv_nxt++;
		c->peer_fin = true;
		c->ack_pending = true;
		if (c->state == ESTABLISHED) {
			c->state = CLOSE_WAIT;
		} else if (c->state == FIN_WAIT_1) {
			c->state = c->fin_acked ? TIME_WAIT : CLOSING;
		} else if (c->state == FIN_WAIT_2) {
			c->state = TIME_WAIT;
		}
		if (c->state == TIME_WAIT) {
			c->done_at = timer_uptime_ms() + TIME_WAIT_MS;
		}
		wake(c);
	} else if (flags & F_FIN) {
		c->ack_pending = true;
	}
	if (c->ack_pending || c->sbuf_len > c->sent) {
		kick();
	}
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
	tcp_stats.segs_in++;
	if (len < TCP_HEADER) {
		tcp_stats.bad++;
		return;
	}
	size_t hlen = (size_t)(data[12] >> 4) * 4;
	if (hlen < TCP_HEADER || hlen > len) {
		tcp_stats.bad++;
		return;
	}
	uint8_t pseudo[12];
	put_be32(pseudo, src);
	put_be32(pseudo + 4, dst);
	pseudo[8] = 0;
	pseudo[9] = PROTO_TCP;
	put_be16(pseudo + 10, (uint16_t)len);
	if (inet_fold(inet_sum(inet_sum(0, pseudo, 12), data, len)) != 0) {
		tcp_stats.bad++;
		return;
	}
	uint32_t flags = cpu_irq_save();
	struct tcp_conn *c = find(src, get_be16(data), get_be16(data + 2));
	if (!c || c->local_ip != dst) {
		cpu_irq_restore(flags);
		tcp_stats.no_conn++;
		return; /* nothing listens; a client may stay silent */
	}
	input(c, get_be32(data + 4), get_be32(data + 8), data[13], get_be16(data + 14),
	      data + TCP_HEADER, hlen - TCP_HEADER, data + hlen, len - hlen);
	cpu_irq_restore(flags);
}

/* --- sending segments (the tcp thread) --- */

static uint32_t window_now(const struct tcp_conn *c)
{
	size_t free = RECV_BUF - c->rbuf_len;
	return free > 65535 ? 65535 : (uint32_t)free;
}

/* Builds and sends one segment. Called with interrupts on. */
static void emit(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq,
                 uint32_t ack, uint8_t flags, uint16_t window, const uint8_t *data, size_t len)
{
	size_t hlen = TCP_HEADER + ((flags & F_SYN) ? 4 : 0);
	uint8_t *seg = kmalloc(hlen + len);
	if (!seg) {
		return;
	}
	memset(seg, 0, hlen);
	put_be16(seg, sport);
	put_be16(seg + 2, dport);
	put_be32(seg + 4, seq);
	put_be32(seg + 8, (flags & F_ACK) ? ack : 0);
	seg[12] = (uint8_t)((hlen / 4) << 4);
	seg[13] = flags;
	put_be16(seg + 14, window);
	if (flags & F_SYN) {
		seg[20] = 2;
		seg[21] = 4;
		put_be16(seg + 22, TCP_MSS);
	}
	if (len) {
		memcpy(seg + hlen, data, len);
	}
	uint8_t pseudo[12];
	put_be32(pseudo, src);
	put_be32(pseudo + 4, dst);
	pseudo[8] = 0;
	pseudo[9] = PROTO_TCP;
	put_be16(pseudo + 10, (uint16_t)(hlen + len));
	put_be16(seg + 16, inet_fold(inet_sum(inet_sum(0, pseudo, 12), seg, hlen + len)));
	tcp_stats.segs_out++;
	ip_output(dst, PROTO_TCP, seg, hlen + len);
	kfree(seg);
}

/* One pass over a connection: whatever it needs to send now. Returns
 * with interrupts as they were (off on entry). */
static void service(struct tcp_conn *c, uint64_t now)
{
	static uint8_t chunk[TCP_MSS];
	if (c->state == CLOSED) {
		return;
	}
	/* Timers. */
	if (c->rto_at && now >= c->rto_at) {
		if (++c->retries > RETRIES_MAX) {
			fail(c, -ETIMEDOUT);
			return;
		}
		tcp_stats.retransmits++;
		c->rto_ms = c->rto_ms * 2 > RTO_MAX_MS ? RTO_MAX_MS : c->rto_ms * 2;
		c->rto_at = 0;
		if (c->state == SYN_SENT) {
			c->syn_sent = false;
		} else {
			c->sent = 0; /* go back: resend from snd_una */
			if (!c->fin_acked) {
				c->fin_sent = false;
			}
			if (c->snd_wnd == 0 && c->sbuf_len) {
				c->snd_wnd = 1; /* a window probe */
			}
		}
	}
	uint32_t src = c->local_ip, dst = c->remote_ip;
	uint16_t sport = c->local_port, dport = c->remote_port;

	if (c->state == SYN_SENT) {
		if (!c->syn_sent) {
			c->syn_sent = true;
			c->rto_at = now + c->rto_ms;
			uint32_t iss = c->iss;
			cpu_enable_interrupts();
			emit(src, dst, sport, dport, iss, 0, F_SYN, (uint16_t)RECV_BUF, 0, 0);
			(void)cpu_irq_save();
		}
		return;
	}

	/* Data, as far as the peer's window and our MSS allow. */
	bool can_send = c->state == ESTABLISHED || c->state == CLOSE_WAIT;
	while (can_send && c->sent < c->sbuf_len && c->sent < c->snd_wnd) {
		size_t n = c->sbuf_len - c->sent;
		if (n > c->snd_wnd - c->sent) {
			n = c->snd_wnd - c->sent;
		}
		if (n > c->mss) {
			n = c->mss;
		}
		for (size_t i = 0; i < n; i++) {
			chunk[i] = c->sbuf[(c->sbuf_head + c->sent + i) % SEND_BUF];
		}
		uint32_t seq = c->snd_una + (uint32_t)c->sent;
		c->sent += n;
		if (!c->rto_at) {
			c->rto_at = now + c->rto_ms;
		}
		c->ack_pending = false;
		uint32_t ack = c->rcv_nxt;
		uint16_t win = (uint16_t)window_now(c);
		c->window_advertised = win;
		cpu_enable_interrupts();
		emit(src, dst, sport, dport, seq, ack, F_ACK | F_PSH, win, chunk, n);
		(void)cpu_irq_save();
		if (c->state == CLOSED) {
			return;
		}
	}
	/* Our FIN, once everything before it has gone out. */
	if (c->fin_queued && !c->fin_sent && c->sent == c->sbuf_len
	    && (c->state == ESTABLISHED || c->state == CLOSE_WAIT || c->state == FIN_WAIT_1
	        || c->state == CLOSING || c->state == LAST_ACK)) {
		if (c->state == ESTABLISHED) {
			c->state = FIN_WAIT_1;
		} else if (c->state == CLOSE_WAIT) {
			c->state = LAST_ACK;
		}
		c->fin_sent = true;
		if (!c->rto_at) {
			c->rto_at = now + c->rto_ms;
		}
		c->ack_pending = false;
		uint32_t seq = c->snd_una + (uint32_t)c->sbuf_len, ack = c->rcv_nxt;
		uint16_t win = (uint16_t)window_now(c);
		cpu_enable_interrupts();
		emit(src, dst, sport, dport, seq, ack, F_FIN | F_ACK, win, 0, 0);
		(void)cpu_irq_save();
		return;
	}
	/* A bare ACK: something arrived, or our window opened again. */
	uint32_t win = window_now(c);
	if (c->ack_pending || (c->window_advertised < c->mss && win >= RECV_BUF / 2)) {
		c->ack_pending = false;
		c->window_advertised = win;
		uint32_t seq = c->snd_una + (uint32_t)c->sent + (c->fin_sent ? 1 : 0);
		uint32_t ack = c->rcv_nxt;
		cpu_enable_interrupts();
		emit(src, dst, sport, dport, seq, ack, F_ACK, (uint16_t)win, 0, 0);
		(void)cpu_irq_save();
	}
}

static void unlink_conn(struct tcp_conn *c)
{
	struct tcp_conn **link = &conns;
	while (*link && *link != c) {
		link = &(*link)->next;
	}
	if (*link) {
		*link = c->next;
	}
}

static void tcp_thread(void *unused)
{
	(void)unused;
	for (;;) {
		uint64_t now = timer_uptime_ms();
		uint32_t flags = cpu_irq_save();
		/* service() turns interrupts on while it transmits. Only this
		 * thread removes connections, and new ones join at the head, so
		 * the walk stays valid. */
		for (struct tcp_conn *c = conns; c; c = c->next) {
			service(c, now);
		}
		/* Finished orphans and expired TIME_WAITs. */
		struct tcp_conn *c = conns, *dead = 0;
		while (c) {
			struct tcp_conn *next = c->next;
			bool done = c->released
			            && (c->state == CLOSED || (c->done_at && now >= c->done_at));
			if (!c->released && c->state == TIME_WAIT && now >= c->done_at) {
				c->state = CLOSED;
				wake(c);
			}
			if (done) {
				unlink_conn(c);
				c->next = dead;
				dead = c;
			}
			c = next;
		}
		waitq_sleep_until(&work, timer_ticks() + (TICK_MS * TIMER_HZ + 999) / 1000);
		cpu_irq_restore(flags);
		while (dead) {
			struct tcp_conn *next = dead->next;
			kfree(dead);
			dead = next;
		}
	}
}

void tcp_init(void)
{
	if (!thread_create("tcp", tcp_thread, 0)) {
		kprintf("tcp: cannot start the tcp thread\n");
	}
}

/* --- the vnode a program holds --- */

static long conn_read(struct vnode *v, uint32_t offset, void *buf, size_t len)
{
	(void)offset;
	struct tcp_conn *c = (struct tcp_conn *)v;
	uint8_t *out = buf;
	uint32_t flags = cpu_irq_save();
	while (!c->rbuf_len && !c->peer_fin && !c->error && c->state != CLOSED) {
		waitq_sleep(&c->changed);
	}
	if (!c->rbuf_len && c->error && !c->peer_fin) {
		int err = c->error;
		cpu_irq_restore(flags);
		return err;
	}
	size_t n = len < c->rbuf_len ? len : c->rbuf_len;
	for (size_t i = 0; i < n; i++) {
		out[i] = c->rbuf[(c->rbuf_head + i) % RECV_BUF];
	}
	c->rbuf_head = (c->rbuf_head + n) % RECV_BUF;
	c->rbuf_len -= n;
	if (n && c->window_advertised < c->mss && window_now(c) >= RECV_BUF / 2) {
		kick(); /* tell the peer the window is open again */
	}
	cpu_irq_restore(flags);
	return (long)n;
}

static long conn_write(struct vnode *v, uint32_t offset, const void *buf, size_t len)
{
	(void)offset;
	struct tcp_conn *c = (struct tcp_conn *)v;
	const uint8_t *in = buf;
	size_t done = 0;
	uint32_t flags = cpu_irq_save();
	while (done < len) {
		while (c->sbuf_len == SEND_BUF && !c->error && c->state != CLOSED) {
			waitq_sleep(&c->changed);
		}
		if (c->error || c->fin_queued
		    || !(c->state == ESTABLISHED || c->state == CLOSE_WAIT)) {
			int err = c->error ? c->error : -EPIPE;
			cpu_irq_restore(flags);
			return done ? (long)done : err;
		}
		size_t n = SEND_BUF - c->sbuf_len;
		if (n > len - done) {
			n = len - done;
		}
		for (size_t i = 0; i < n; i++) {
			c->sbuf[(c->sbuf_head + c->sbuf_len + i) % SEND_BUF] = in[done + i];
		}
		c->sbuf_len += n;
		done += n;
		kick();
	}
	cpu_irq_restore(flags);
	return (long)done;
}

static int conn_poll(struct vnode *v)
{
	struct tcp_conn *c = (struct tcp_conn *)v;
	return c->rbuf_len || c->peer_fin || c->error || c->state == CLOSED;
}

static void conn_release(struct vnode *v)
{
	struct tcp_conn *c = (struct tcp_conn *)v;
	uint32_t flags = cpu_irq_save();
	c->released = true;
	if (c->state == SYN_SENT || c->state == CLOSED) {
		c->state = CLOSED;
	} else {
		c->fin_queued = true;
		c->done_at = timer_uptime_ms() + TCP_ORPHAN_MS;
	}
	kick();
	cpu_irq_restore(flags);
}

static const struct vnode_ops conn_ops = {
	.read = conn_read,
	.write = conn_write,
	.release = conn_release,
	.poll = conn_poll,
};

int tcp_connect(uint32_t dst, uint16_t port, uint32_t timeout_ms, struct vnode **out)
{
	uint32_t src = ip_source_for(dst);
	if (!src) {
		return -ENETUNREACH;
	}
	struct tcp_conn *c = kmalloc(sizeof(*c));
	if (!c) {
		return -ENOMEM;
	}
	memset(c, 0, sizeof(*c));
	c->vnode.ops = &conn_ops;
	c->vnode.type = VNODE_PIPE;
	c->vnode.refs = 1;
	c->changed = (struct waitq)WAITQ_INIT;
	c->state = SYN_SENT;
	c->local_ip = src;
	c->remote_ip = dst;
	c->remote_port = port;
	c->mss = TCP_MSS_DEFAULT;
	c->rto_ms = RTO_INITIAL_MS;

	uint32_t flags = cpu_irq_save();
	/* An initial sequence number that differs per connection and per
	 * boot (RFC 9293 3.4.1 asks for a clock-driven one). */
	c->iss = (uint32_t)timer_uptime_ms() * 250u + (iss_counter += 64000u) + dst * 2654435761u;
	c->snd_una = c->iss;
	uint16_t lport = 0;
	for (unsigned tries = 0; tries < 65536 - EPHEMERAL_FIRST && !lport; tries++) {
		uint16_t candidate = next_ephemeral;
		next_ephemeral = next_ephemeral == 65535 ? EPHEMERAL_FIRST : next_ephemeral + 1;
		if (!find(dst, port, candidate)) {
			lport = candidate;
		}
	}
	if (!lport) {
		cpu_irq_restore(flags);
		kfree(c);
		return -EADDRINUSE;
	}
	c->local_port = lport;
	c->next = conns;
	conns = c;
	tcp_stats.opened++;
	kick();
	uint64_t deadline = timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000;
	while (c->state == SYN_SENT && waitq_sleep_until(&c->changed, deadline)) {
	}
	int rc = c->state == SYN_SENT ? -ETIMEDOUT : c->error;
	if (rc) {
		/* The thread frees it: closed and released. */
		c->error = c->error ? c->error : rc;
		c->state = CLOSED;
		c->released = true;
		kick();
		cpu_irq_restore(flags);
		return rc;
	}
	cpu_irq_restore(flags);
	*out = &c->vnode;
	return 0;
}
