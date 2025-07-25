// SPDX-License-Identifier: BSD-2-Clause

/* This file various utility functions for unit testing; this file
 * is implemented entirely in C, and accesses Homa and kernel internals.
 */

#include "homa_impl.h"
#include "homa_grant.h"
#include "homa_pacer.h"
#include "homa_peer.h"
#include "homa_rpc.h"
#include "ccutils.h"
#define KSELFTEST_NOT_MAIN 1
#include "kselftest_harness.h"
#include "mock.h"
#include "utils.h"

/**
 * unit_client_rpc() - Create a homa_client_rpc and arrange for it to be
 * in a given state.
 * @hsk:           Socket that will receive the incoming RPC.
 * @state:         Desired state for the RPC.
 * @client_ip:     Client's IP address.
 * @server_ip:     Server's IP address.
 * @server_port:   Port number on the server.
 * @id:            Id for the RPC (0 means use default).
 * @req_length:    Amount of data in the request.
 * @resp_length:   Amount of data in the response.
 *
 * Return:         The properly initialized homa_client_rpc, or NULL if
 *                 there was an error. The RPC is not locked.
 */
struct homa_rpc *unit_client_rpc(struct homa_sock *hsk,
		enum unit_rpc_state state, struct in6_addr *client_ip,
		struct in6_addr *server_ip, int server_port, int id,
		int req_length, int resp_length)
{
	int saved_id = atomic64_read(&hsk->homa->next_outgoing_id);
	union sockaddr_in_union server_addr;
	int bytes_received, this_size;
	struct homa_rpc *crpc;

	server_addr.in6.sin6_family = AF_INET6;
	server_addr.in6.sin6_addr = *server_ip;
	server_addr.in6.sin6_port =  htons(server_port);
	if (id != 0)
		atomic64_set(&hsk->homa->next_outgoing_id, id);
	crpc = homa_rpc_alloc_client(hsk, &server_addr);
	if (IS_ERR(crpc))
		return NULL;
	if (homa_message_out_fill(crpc, unit_iov_iter(NULL, req_length), 0)) {
		homa_rpc_end(crpc);
		return NULL;
	}
	homa_rpc_unlock(crpc);
	if (id != 0)
		atomic64_set(&hsk->homa->next_outgoing_id, saved_id);
	EXPECT_EQ(RPC_OUTGOING, crpc->state);
	if (state == UNIT_OUTGOING)
		return crpc;
	crpc->msgout.next_xmit_offset = crpc->msgout.length;

	struct homa_data_hdr h;
	memset(&h, 0, sizeof(h));
	h.common = (struct homa_common_hdr){
		.sport = htons(server_port),
		.dport = htons(hsk->port),
		.type = DATA,
		.sender_id = cpu_to_be64(id ^ 1)
	};
	h.message_length = htonl(resp_length);
#ifndef __STRIP__ /* See strip.py */
	h.incoming = htonl(10000);
#endif /* See strip.py */

	this_size = (resp_length > UNIT_TEST_DATA_PER_PACKET)
			? UNIT_TEST_DATA_PER_PACKET : resp_length;
	homa_dispatch_pkts(mock_skb_alloc(server_ip, &h.common, this_size, 0));
	if (state == UNIT_RCVD_ONE_PKT)
		return crpc;
	for (bytes_received = UNIT_TEST_DATA_PER_PACKET;
			bytes_received < resp_length;
			bytes_received += UNIT_TEST_DATA_PER_PACKET) {
		this_size = resp_length - bytes_received;
		if (this_size >  UNIT_TEST_DATA_PER_PACKET)
			this_size = UNIT_TEST_DATA_PER_PACKET;
		h.seg.offset = htonl(bytes_received);
		homa_dispatch_pkts(mock_skb_alloc(server_ip, &h.common,
				this_size, 0));
	}
	if (state == UNIT_RCVD_MSG)
		return crpc;
	FAIL("%s received unexpected state %d", __func__, state);
	homa_rpc_end(crpc);
	return NULL;
}

/**
 * unit_get_in_addr - Parse a string into an IPv6 host addresss
 * @s:          IPV4 host specification such as 192.168.0.1, or IPv6
 *              spec that can be parsed by inet_pton.
 *
 * Return:      The in_addr (in network order) corresponding to @s. If
 *              s couldn't be parsed properly then this function aborts.
 *
 */
struct in6_addr unit_get_in_addr(char *s)
{
	struct in6_addr ret = {};
	unsigned int a, b, c, d;

	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
		ret.s6_addr32[3] = htonl((a<<24) + (b<<16) + (c<<8) + d);
		ret.s6_addr32[2] = htonl(0x0000ffff);
	} else {
		int res = inet_pton(AF_INET6, s, &ret);

		if (res <= 0)
			abort();
	}
	return ret;
}

/**
 * unit_list_length() - Return the number of entries in a list (not including
 * the list header.
 * @head:   Header for the list (or any entry in the list, for that matter).
 */
int unit_list_length(struct list_head *head)
{
	struct list_head *pos;
	int count = 0;

	list_for_each(pos, head) {
		count++;
	}
	return count;
}

/**
 * unit_log_active_ids() - Append to the test log a list of the active
 * RPC ids for a given socket, in order.
 * @hsk:   Socket whose active RPC ids should be logged.
 */
void unit_log_active_ids(struct homa_sock *hsk)
{
	struct homa_rpc *rpc;

	list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links)
		unit_log_printf(" ", "%llu", rpc->id);
}

/**
 * unit_log_hashed_rpcs() - And to the test log a list of the RPC ids
 * all RPCs present in the hash table for a socket.
 * @hsk:    Socket whose hashed RPCs should be logged.
 */
void unit_log_hashed_rpcs(struct homa_sock *hsk)
{
	struct homa_rpc *rpc;
	int i;

	for (i = 0; i < HOMA_CLIENT_RPC_BUCKETS; i++) {
		hlist_for_each_entry_rcu(rpc, &hsk->client_rpc_buckets[i].rpcs,
				hash_links) {
			unit_log_printf(" ", "%llu", rpc->id);
		}
	}
	for (i = 0; i < HOMA_SERVER_RPC_BUCKETS; i++) {
		hlist_for_each_entry_rcu(rpc, &hsk->server_rpc_buckets[i].rpcs,
				hash_links) {
			unit_log_printf(" ", "%llu", rpc->id);
		}
	}
}

/**
 * unit_log_frag_list() - Append to the test log a human-readable description
 * of all of the packets on a given skb's frag_list.
 * @skb:         Packet whose frag_list is of interest.
 * @verbose:     If non-zero, use homa_print_packet for each packet;
 *               otherwise use homa_print_packet_short.
 */
void unit_log_frag_list(struct sk_buff *skb, int verbose)
{
	struct sk_buff *frag;
	char buffer[200];

	for (frag = skb_shinfo(skb)->frag_list; frag != NULL;
			frag = frag->next) {
		if (verbose)
			homa_print_packet(frag, buffer, sizeof(buffer));
		else
			homa_print_packet_short(frag, buffer, sizeof(buffer));
		unit_log_printf("; ", "%s", buffer);
	}
}

#ifndef __STRIP__ /* See strip.py */
/**
 * unit_log_grantables() - Append to the test log information about all of
 * the messages under grant->grantable_peers.
 * @homa:     Homa's overall state.
 */
void unit_log_grantables(struct homa *homa)
{
	struct homa_peer *peer;
	struct homa_rpc *rpc;
	int i;

	for (i = 0; i < homa->grant->num_active_rpcs; i++) {
		rpc = homa->grant->active_rpcs[i];
		unit_log_printf("; ", "active[%d]: id %llu ungranted %d",
				i, rpc->id,
				rpc->msgin.length - rpc->msgin.granted);
		if (rpc->msgin.rank != i) {
			unit_log_printf(" ", "bad rank %d", rpc->msgin.rank);
		}
	}
	list_for_each_entry(peer, &homa->grant->grantable_peers,
			    grantable_links) {
		unit_log_printf("; ", "peer %s:",
				homa_print_ipv6_addr(&peer->addr));
		list_for_each_entry(rpc, &peer->grantable_rpcs,
				grantable_links) {
			unit_log_printf(" ", "id %llu ungranted %d",
					rpc->id,
					rpc->msgin.length - rpc->msgin.granted);
		}
	}
}
#endif /* See strip.py */

/**
 * unit_log_message_out_packets() - Append to the test log a human-readable
 * description of the packets associated with a homa_message_out.
 * @message:     Message containing the packets.
 * @verbose:     If non-zero, use homa_print_packet for each packet;
 *               otherwise use homa_print_packet_short.
 *
 * This function also checks to be sure that homa->num_grantable matches
 * the actual number of entries in the list, and generates additional
 * log output if it doesn't.
 */
void unit_log_message_out_packets(struct homa_message_out *message, int verbose)
{
	struct sk_buff *skb;
	char buffer[200];

	for (skb = message->packets; skb != NULL;
			skb = homa_get_skb_info(skb)->next_skb) {
		if (verbose)
			homa_print_packet(skb, buffer, sizeof(buffer));
		else
			homa_print_packet_short(skb, buffer, sizeof(buffer));
		unit_log_printf("; ", "%s", buffer);
	}
}


/**
 * unit_log_filled_skbs() - Append to the test log a human-readable description
 * of a list of packet buffers created by homa_fill_packets.
 * @skb:         First in list of sk_buffs to print; the list is linked
 *               using homa_next_skb.
 * @verbose:     If non-zero, use homa_print_packet for each packet;
 *               otherwise use homa_print_packet_short.
 */
void unit_log_filled_skbs(struct sk_buff *skb, int verbose)
{
	char buffer[400];

	while (skb != NULL) {
		if (verbose)
			homa_print_packet(skb, buffer, sizeof(buffer));
		else
			homa_print_packet_short(skb, buffer, sizeof(buffer));
		unit_log_printf("; ", "%s", buffer);
		skb = homa_get_skb_info(skb)->next_skb;
	}
}

/**
 * unit_log_skb_list() - Append to the test log a human-readable description
 * of a list of packet buffers.
 * @packets:     Header for list of sk_buffs to print.
 * @verbose:     If non-zero, use homa_print_packet for each packet;
 *               otherwise use homa_print_packet_short.
 */
void unit_log_skb_list(struct sk_buff_head *packets, int verbose)
{
	struct sk_buff *skb;
	char buffer[200];

	skb_queue_walk(packets, skb) {
		if (verbose)
			homa_print_packet(skb, buffer, sizeof(buffer));
		else
			homa_print_packet_short(skb, buffer, sizeof(buffer));
		unit_log_printf("; ", "%s", buffer);
	}
}

/**
 * unit_log_throttled() - Append to the test log information about all of
 * the messages in homa->throttle_rpcs.
 * @homa:     Homa's overall state.
 */
void unit_log_throttled(struct homa *homa)
{
	struct homa_rpc *rpc;

	list_for_each_entry_rcu(rpc, &homa->pacer->throttled_rpcs, throttled_links) {
		unit_log_printf("; ", "%s id %llu, next_offset %d",
				homa_is_client(rpc->id) ? "request"
				: "response", rpc->id,
				rpc->msgout.next_xmit_offset);
	}
}

/**
 * unit_log_dead_peers() - Append to the test log the addresses of all
 * peers in peertab->dead_peers for @homa.
 * @homa:     Homa's overall state.
 */
void unit_log_dead_peers(struct homa *homa)
{
	struct homa_peer *peer;

	list_for_each_entry(peer, &homa->peertab->dead_peers, dead_links) {
		unit_log_printf("; ", "%s",
				homa_print_ipv6_addr(&peer->ht_key.addr));
	}
}

/**
 * unit_print_gaps() - Returns a static string describing the gaps in an RPC.
 * @rpc:     Log the gaps in this RPC.
 */
const char *unit_print_gaps(struct homa_rpc *rpc)
{
	struct homa_gap *gap;
	static char buffer[1000];
	int used = 0;

	buffer[0] = 0;
	list_for_each_entry(gap, &rpc->msgin.gaps, links) {
		if (used != 0)
			used += snprintf(buffer + used, sizeof(buffer) - used,
					"; ");
		used += snprintf(buffer + used, sizeof(buffer) - used,
				"start %d, end %d", gap->start, gap->end);
		if (gap->time != 0)
			used += snprintf(buffer + used, sizeof(buffer) - used,
					 ", time %llu", gap->time);
	}
	return buffer;
}

/**
 * unit_server_rpc() - Create a homa_server_rpc and arrange for it to be
 * in a given state.
 * @hsk:           Socket that will receive the incoming RPC.
 * @state:         Desired state for the RPC.
 * @client_ip:     Client's IP address.
 * @server_ip:     Server's IP address.
 * @client_port:   Port number that the client used.
 * @id:            Id for the RPC.
 * @req_length:    Amount of data in the request.
 * @resp_length:   Amount of data in the response.
 *
 * Return:         The properly initialized homa_server_rpc, or NULL if
 *                 there was an error. The RPC is not locked.
 */
struct homa_rpc *unit_server_rpc(struct homa_sock *hsk,
		enum unit_rpc_state state, struct in6_addr *client_ip,
		struct in6_addr *server_ip, int client_port, int id,
		int req_length, int resp_length)
{
	int bytes_received, created;
	struct homa_data_hdr h;
	int status;

	memset(&h, 0, sizeof(h));
	h.common = (struct homa_common_hdr){
		.sport = htons(client_port),
		.dport = htons(hsk->port),
		.type = DATA,
		.sender_id = cpu_to_be64(id ^ 1)
	};
	h.message_length = htonl(req_length);
#ifndef __STRIP__ /* See strip.py */
	h.incoming = htonl(10000);
#endif /* See strip.py */
	struct homa_rpc *srpc = homa_rpc_alloc_server(hsk, client_ip, &h,
			&created);

	if (IS_ERR(srpc))
		return NULL;
	EXPECT_EQ(srpc->completion_cookie, 0);
	homa_rpc_unlock(srpc);
	homa_dispatch_pkts(mock_skb_alloc(client_ip, &h.common,
			(req_length > UNIT_TEST_DATA_PER_PACKET)
			? UNIT_TEST_DATA_PER_PACKET : req_length, 0));
	if (state == UNIT_RCVD_ONE_PKT)
		return srpc;
	for (bytes_received = UNIT_TEST_DATA_PER_PACKET;
			bytes_received < req_length;
			bytes_received += UNIT_TEST_DATA_PER_PACKET) {
		int this_size = req_length - bytes_received;

		if (this_size >  UNIT_TEST_DATA_PER_PACKET)
			this_size = UNIT_TEST_DATA_PER_PACKET;
		h.seg.offset = htonl(bytes_received);
		homa_dispatch_pkts(mock_skb_alloc(client_ip, &h.common,
				this_size, 0));
	}
	if (state == UNIT_RCVD_MSG)
		return srpc;
	srpc->state = RPC_IN_SERVICE;
	if (state == UNIT_IN_SERVICE)
		return srpc;
	homa_rpc_lock(srpc);
	status = homa_message_out_fill(srpc, unit_iov_iter((void *) 2000,
				       resp_length), 0);
	homa_rpc_unlock(srpc);
	if (status != 0)
		goto error;
	srpc->state = RPC_OUTGOING;
	if (state == UNIT_OUTGOING)
		return srpc;
	FAIL("%s received unexpected state %d", __func__, state);

error:
	homa_rpc_end(srpc);
	return NULL;
}

/**
 * unit_teardown() - This function should be invoked at the end of every test.
 * It performs various cleanup operations, and it also performs a set of
 * consistency checks, such as checking for memory leaks or lost sk_buffs.
 */
void unit_teardown(void)
{
	mock_teardown();
	unit_log_clear();
}

/**
 * unit_iov_iter() - Return an iov_iter corresponding to the arguments.
 * @buffer:     First byte of data.
 * @length:     Number of bytes of data.
 */
struct iov_iter *unit_iov_iter(void *buffer, size_t length)
{
	static struct iovec iovec;
	static struct iov_iter iter;

	iovec.iov_base = buffer;
	iovec.iov_len = length;
	iov_iter_init(&iter, WRITE, &iovec, 1, length);
	return &iter;
}

/**
 * unit_ack_string() - Returns a human-readable description of the fields
 * in an ack.
 * @ack:  The ack to stringify.
 */
char *unit_ack_string(struct homa_ack *ack)
{
	static char buffer[1000];

	snprintf(buffer, sizeof(buffer),
			"server_port %d, client_id %llu",
			ntohs(ack->server_port), be64_to_cpu(ack->client_id));
	return buffer;
}

/**
 * unit_homa_destroy() - When unit tests are run, this function is invoked
 * by homa_destroy. It checks for various errors and reports them.
 * @homa:       Homa shared data that is about to be deleted.
 */
void unit_homa_destroy(struct homa *homa)
{
	/* Currently nothing to check. */
}

/**
 * unit_sock_destroy() - Invoked by unit tests to cleanup and destroy
 * a socket.
 * @hsk:    Socket to destroy.
 */
void unit_sock_destroy(struct homa_sock *hsk)
{
	homa_sock_shutdown(hsk);
	homa_sock_destroy(&hsk->sock);
}

/**
 * unit_log_peers() - Return a count of the number of peers in the
 * homa_peertab for @homa (could also include peers from other homas).
 * @homa:       Used to locate homa_peertab to count.
 */
int unit_count_peers(struct homa *homa)
{
	struct rhashtable_iter iter;
	struct homa_peer *peer;
	int count = 0;

	rhashtable_walk_enter(&homa->peertab->ht, &iter);
	rhashtable_walk_start(&iter);
	while (1) {
		peer = rhashtable_walk_next(&iter);
		if (!peer)
			break;
		if (IS_ERR(peer))
			continue;
		count++;
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	return count;
}