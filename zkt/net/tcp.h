/* TCP (RFC 9293) connections, opened actively (tcp_connect) or taken
 * by a listener (tcp_listen, tcp_accept: 0.31). A connection is a stream vnode (VNODE_PIPE: no
 * seeking); its last release closes the connection gracefully. */
#ifndef ZKT_NET_TCP_H
#define ZKT_NET_TCP_H

#include <stddef.h>
#include <stdint.h>

struct vnode;

/* Opens a connection to dst:port, waiting up to timeout_ms for it to be
 * established. On success *out holds a vnode with one reference: reads
 * give the received bytes (0 at the peer's end of stream), writes queue
 * bytes to send. Errors: -ECONNREFUSED, -ETIMEDOUT, -EHOSTUNREACH,
 * -ENOMEM. */
int tcp_connect(uint32_t dst, uint16_t port, uint32_t timeout_ms, struct vnode **out);

/* Listens on `port` (on every address): *out is a vnode with one
 * reference, which polls readable while connections wait to be
 * accepted; releasing it stops listening. -EADDRINUSE if something
 * listens there already. */
int tcp_listen(uint16_t port, struct vnode **out);
/* Waits for a connection on listener `v` (-EINVAL if it isn't one):
 * *out is the connection, as tcp_connect gives it, and ip and port say
 * who is on the other end. */
int tcp_accept(struct vnode *v, struct vnode **out, uint32_t *ip, uint16_t *port);

/* Called by ip_input for protocol 6 (on the network thread). */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len);

/* Starts the TCP timer/transmit thread (net_init). */
void tcp_init(void);

struct tcp_stats {
	uint32_t segs_in, segs_out, retransmits, bad, no_conn, opened, reset;
};
extern struct tcp_stats tcp_stats;

#endif
