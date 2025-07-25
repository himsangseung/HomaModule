// SPDX-License-Identifier: BSD-2-Clause

/* This file provides simplified substitutes for many Linux variables and
 * functions in order to allow Homa unit tests to be run outside a Linux
 * kernel.
 */

#include "homa_impl.h"
#include "homa_pool.h"
#ifndef __STRIP__ /* See strip.py */
#include "homa_skb.h"
#endif /* See strip.py */
#include "ccutils.h"
#include "utils.h"

#include <linux/rhashtable.h>

/* It isn't safe to include some header files, such as stdlib, because
 * they conflict with kernel header files. The explicit declarations
 * below replace those header files.
 */

extern void       free(void *ptr);
extern void      *malloc(size_t size);
#ifdef memcpy
#undef memcpy
#endif
extern void      *memcpy(void *dest, const void *src, size_t n);

/* The variables below can be set to non-zero values by unit tests in order
 * to simulate error returns from various functions. If bit 0 is set to 1,
 * the next call to the function will fail; bit 1 corresponds to the next
 * call after that, and so on.
 */
int mock_alloc_page_errors;
int mock_alloc_skb_errors;
int mock_copy_data_errors;
int mock_copy_to_iter_errors;
int mock_copy_to_user_errors;
int mock_cpu_idle;
int mock_dst_check_errors;
int mock_import_ubuf_errors;
int mock_import_iovec_errors;
int mock_ip6_xmit_errors;
int mock_ip_queue_xmit_errors;
int mock_kmalloc_errors;
int mock_kthread_create_errors;
int mock_prepare_to_wait_errors;
int mock_register_protosw_errors;
int mock_register_sysctl_errors;
int mock_rht_init_errors;
int mock_rht_insert_errors;
int mock_route_errors;
int mock_spin_lock_held;
int mock_trylock_errors;
int mock_vmalloc_errors;
int mock_wait_intr_irq_errors;

/* The value that prepare_to_wait_event should return when
 * mock_prepare_to_wait_errors is nonzero.
 */
int mock_prepare_to_wait_status = -ERESTARTSYS;

/* The return value from calls to signal_pending(). */
int mock_signal_pending;

/* Used as current task during tests. Also returned by kthread_run. */
struct task_struct mock_task;

/* If a test sets this variable to nonzero, ip_queue_xmit will log
 * outgoing packets using the long format rather than short.
 */
int mock_xmit_log_verbose;

/* If a test sets this variable to nonzero, ip_queue_xmit will log
 * the contents of the homa_info from packets.
 */
int mock_xmit_log_homa_info;

/* If a test sets this variable to nonzero, calls to wake_up and
 * wake_up_all will be logged.
 */
int mock_log_wakeups;

/* If a test sets this variable to nonzero, call_rcu_sched will log
 * whenever it is invoked.
 */
int mock_log_rcu_sched;

/* A zero value means that copy_to_user will actually copy bytes to
 * the destination address; if nonzero, then 0 bits determine which
 * copies actually occur (bit 0 for the first copy, etc., just like
 * error masks).
 */
int mock_copy_to_user_dont_copy;

/* HOMA_BPAGE_SIZE will evaluate to this. */
int mock_bpage_size = 0x10000;

/* HOMA_BPAGE_SHIFT will evaluate to this. */
int mock_bpage_shift = 16;

/* Keeps track of all the spinlocks that have been locked but not unlocked.
 * Reset for each test.
 */
static struct unit_hash *spinlocks_held;

/* Keeps track of all the blocks of memory that have been allocated by
 * kmalloc but not yet freed by kfree. Reset for each test.
 */
static struct unit_hash *kmallocs_in_use;

/* Keeps track of all the results returned by proc_create that have not
 * yet been closed by calling proc_remove. Reset for each test.
 */
static struct unit_hash *proc_files_in_use;

/* Keeps track of all the results returned by alloc_pages that have
 * not yet been released by calling put_page. The value of each entry is
 * a (char *) giving the reference count for the page. Reset for each test.
 */
static struct unit_hash *pages_in_use;

/* Keeps track of all the results returned by ip_route_output_flow that
 * have not yet been freed. Reset for each test.
 */
static struct unit_hash *routes_in_use;

/* Keeps track of all sk_buffs that are alive in the current test.
 * Reset for each test.
 */
static struct unit_hash *skbs_in_use;

/* Keeps track of all the blocks of memory that have been allocated by
 * vmalloc but not yet freed by vfree. Reset for each test.
 */
static struct unit_hash *vmallocs_in_use;

/* The number of locks (other than spin locks) that have been acquired
 * but not yet released. Should be 0 at the end of each test.
 */
static int mock_active_locks;

/* Total number of successful spinlock acquisitions during current test. */
int mock_total_spin_locks;

/* The number of times rcu_read_lock has been called minus the number
 * of times rcu_read_unlock has been called.
 * Should be 0 at the end of each test.
 */
static int mock_active_rcu_locks;

/* Number of calls to sock_hold that haven't been matched with calls
 * to sock_put.
 */
int mock_sock_holds;

/* Number of calls to homa_rpc_hold that haven't been matched with calls
 * to homa_rpc_put.
 */
int mock_rpc_holds;

/* The number of times preempt_disable() has been invoked, minus the
 * number of times preempt_enable has been invoked.
 */
static int mock_preempt_disables;

/* Used as the return value for calls to homa_clock. */
u64 mock_clock;

/* Add this value to mock_clock every time homa_clock is invoked. */
u64 mock_clock_tick;

/* If values are present here, use them as the return values from
 * homa_clock, without considering mock_clock or mock_clock_tick.
 */
#define MAX_CLOCK_VALS 10
u64 mock_clock_vals[MAX_CLOCK_VALS];
int mock_next_clock_val = 0;
int mock_num_clock_vals = 0;

/* Used as the return value for tt_get_cycles. */
u64 mock_tt_cycles;

/* Indicates whether we should be simulation IPv6 or IPv4 in the
 * current test. Can be overridden by a test.
 */
bool mock_ipv6 = true;

/* The value to use for mock_ipv6 in each test unless overridden. */
bool mock_ipv6_default;

/* List of priorities for all outbound packets. */
char mock_xmit_prios[1000];
int mock_xmit_prios_offset;

/* Maximum packet size allowed by "network" (see homa_message_out_fill;
 * chosen so that data packets will have UNIT_TEST_DATA_PER_PACKET bytes
 * of payload. The variable can be modified if useful in some tests.
 * Set by mock_sock_init.
 */
int mock_mtu;

/* Used instead of MAX_SKB_FRAGS when running some unit tests. */
int mock_max_skb_frags = MAX_SKB_FRAGS;

/* Each bit gives the NUMA node (0 or 1) for a particular core.*/
int mock_numa_mask = 5;

/* Bits determine the result of successive calls to compound order, starting
 * at the lowest bit. 0 means return HOMA_SKB_PAGE_ORDER, 1 means return 0.
 */
int mock_compound_order_mask;

/* Bits specify the NUMA node number that will be returned by the next
 * calls to mock_page_to_nid, starting with the low-order bit.
 */
int mock_page_nid_mask;

/* Used to collect printk output. */
char mock_printk_output [5000];

/* Used as the return values from rhashtable_walk_next calls. */
void **mock_rht_walk_results;
int mock_rht_num_walk_results;

/* Used instead of HOMA_MIN_DEFAULT_PORT by homa_skb.c. */
__u16 mock_min_default_port = 0x8000;

/* Used as sk_socket for all sockets created by mock_sock_init. */
static struct socket mock_socket;

#define MOCK_MAX_NETS 10
static struct net mock_nets[MOCK_MAX_NETS];
static struct homa_net mock_hnets[MOCK_MAX_NETS];
static int mock_num_hnets;

/* Nonzero means don't generate a unit test failure when freeing peers
 * if the reference count isn't zero (log a message instead).
 */
int mock_peer_free_no_fail;

struct dst_ops mock_dst_ops = {
	.mtu = mock_get_mtu,
	.check = mock_dst_check};
struct netdev_queue mock_net_queue = {.state = 0};
struct net_device mock_net_device = {
		.gso_max_segs = 1000,
		.gso_max_size = 0,
		._tx = &mock_net_queue,
		.nd_net = {.net = &mock_nets[0]}
	};
const struct net_offload *inet_offloads[MAX_INET_PROTOS];
const struct net_offload *inet6_offloads[MAX_INET_PROTOS];
struct net_offload tcp_offload;
struct net_offload tcp_v6_offload;

static struct hrtimer_clock_base clock_base;
struct task_struct *current_task = &mock_task;
unsigned long ex_handler_refcount;
struct net init_net;
unsigned long volatile jiffies = 1100;
unsigned int nr_cpu_ids = 8;
unsigned long page_offset_base;
unsigned long phys_base;
unsigned long vmemmap_base;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES];
#endif
int __preempt_count;
struct pcpu_hot pcpu_hot = {.cpu_number = 1, .current_task = &mock_task};
char sock_flow_table[RPS_SOCK_FLOW_TABLE_SIZE(1024)];
struct net_hotdata net_hotdata = {
	.rps_cpu_mask = 0x1f,
	.rps_sock_flow_table = (struct rps_sock_flow_table *) sock_flow_table
};
int debug_locks;
struct static_call_key __SCK__cond_resched;
struct static_call_key __SCK__might_resched;
struct static_call_key __SCK__preempt_schedule;
struct paravirt_patch_template pv_ops;
struct workqueue_struct *system_wq;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
struct lockdep_map rcu_lock_map;
#endif /* CONFIG_DEBUG_LOCK_ALLOC */

extern void add_wait_queue(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry)
{}

struct sk_buff *__alloc_skb(unsigned int size, gfp_t priority, int flags,
		int node)
{
	struct sk_buff *skb;
	int shinfo_size;

	if (mock_check_error(&mock_alloc_skb_errors))
		return NULL;
	skb = malloc(sizeof(struct sk_buff));
	if (skb == NULL)
		FAIL(" skb malloc failed in %s", __func__);
	memset(skb, 0, sizeof(*skb));
	if (!skbs_in_use)
		skbs_in_use = unit_hash_new();
	unit_hash_set(skbs_in_use, skb, "used");
	shinfo_size = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	skb->head = malloc(size + shinfo_size);
	memset(skb->head, 0, size + shinfo_size);
	if (skb->head == NULL)
		FAIL(" data malloc failed in %s", __func__);
	skb->data = skb->head;
	skb_reset_tail_pointer(skb);
	skb->end = skb->tail + size;
	skb->network_header = 0;
	skb->transport_header = 0;
	skb->data_len = 0;
	skb->len = 0;
	skb->users.refs.counter = 1;
	skb->_skb_refdst = 0;
	ip_hdr(skb)->saddr = 0;
	skb->truesize = SKB_TRUESIZE(size);
	skb->dev = &mock_net_device;
	return skb;
}

int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
			     int sync, void *key)
{
	return 0;
}

void BUG_func(void)
{}

void call_rcu(struct rcu_head *head, void free_func(struct rcu_head *head))
{
	unit_log_printf("; ", "call_rcu invoked");
}

bool cancel_work_sync(struct work_struct *work)
{
	return false;
}

void __check_object_size(const void *ptr, unsigned long n, bool to_user) {}

size_t _copy_from_iter(void *addr, size_t bytes, struct iov_iter *iter)
{
	size_t bytes_left = bytes;

	if (mock_check_error(&mock_copy_data_errors))
		return false;
	if (bytes > iter->count) {
		unit_log_printf("; ", "copy_from_iter needs %lu bytes, but iov_iter has only %lu", bytes,
				iter->count);
		return 0;
	}
	while (bytes_left > 0) {
		struct iovec *iov = (struct iovec *) iter_iov(iter);
		u64 int_base = (u64) iov->iov_base;
		size_t chunk_bytes = iov->iov_len;

		if (chunk_bytes > bytes_left)
			chunk_bytes = bytes_left;
		unit_log_printf("; ", "_copy_from_iter %lu bytes at %llu",
				chunk_bytes, int_base);
		bytes_left -= chunk_bytes;
		iter->count -= chunk_bytes;
		iov->iov_base = (void *) (int_base + chunk_bytes);
		iov->iov_len -= chunk_bytes;
		if (iov->iov_len == 0)
			iter->__iov++;
	}
	return bytes;
}

bool _copy_from_iter_full(void *addr, size_t bytes, struct iov_iter *i)
{
	if (mock_check_error(&mock_copy_data_errors))
		return false;
	unit_log_printf("; ", "_copy_from_iter_full copied %lu bytes", bytes);
	return true;
}

bool _copy_from_iter_full_nocache(void *addr, size_t bytes, struct iov_iter *i)
{
	if (mock_check_error(&mock_copy_data_errors))
		return false;
	unit_log_printf("; ", "_copy_from_iter_full_nocache copid %lu bytes",
			bytes);
	return true;
}

size_t _copy_to_iter(const void *addr, size_t bytes, struct iov_iter *i)
{
	if (mock_check_error(&mock_copy_to_iter_errors))
		return 0;
	unit_log_printf("; ", "_copy_to_iter: %.*s", (int) bytes,
			(char *) addr);
	return bytes;
}

unsigned long _copy_to_user(void __user *to, const void *from, unsigned long n)
{
	if (mock_check_error(&mock_copy_to_user_errors))
		return -1;
	if (!mock_check_error(&mock_copy_to_user_dont_copy))
		memcpy(to, from, n);
	unit_log_printf("; ", "_copy_to_user copied %lu bytes to %p", n, to);
	return 0;
}

unsigned long _copy_from_user(void *to, const void __user *from,
		unsigned long n)
{
	u64 int_from = (u64) from;

	if (mock_check_error(&mock_copy_data_errors))
		return 1;
	if (int_from > 200000)
		memcpy(to, from, n);
	unit_log_printf("; ", "_copy_from_user %lu bytes at %llu", n, int_from);
	return 0;
}

void __copy_overflow(int size, unsigned long count)
{
	abort();
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
int debug_lockdep_rcu_enabled(void)
{
	return 0;
}
#endif

int do_wait_intr_irq(wait_queue_head_t *, wait_queue_entry_t *)
{
	UNIT_HOOK("do_wait_intr_irq");
	if (mock_check_error(&mock_wait_intr_irq_errors))
		return -ERESTARTSYS;
	return 0;
}

void dst_release(struct dst_entry *dst)
{
	if (!dst)
		return;
	atomic_dec(&dst->__rcuref.refcnt);
	if (atomic_read(&dst->__rcuref.refcnt) > 0)
		return;
	if (!routes_in_use || unit_hash_get(routes_in_use, dst) == NULL) {
		FAIL(" %s on unknown route", __func__);
		return;
	}
	unit_hash_erase(routes_in_use, dst);
	free(dst);
}

void finish_wait(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry)
{}

void get_random_bytes(void *buf, size_t nbytes)
{
	memset(buf, 0, nbytes);
}

u32 get_random_u32(void)
{
	return 0;
}

int hrtimer_cancel(struct hrtimer *timer)
{
	return 0;
}

u64 hrtimer_forward(struct hrtimer *timer, ktime_t now,
		ktime_t interval)
{
	return 0;
}

ktime_t hrtimer_get_time(void)
{
	return 0;
}

void hrtimer_init(struct hrtimer *timer, clockid_t clock_id,
		  enum hrtimer_mode mode)
{
	timer->base = &clock_base;
	clock_base.get_time = &hrtimer_get_time;
}

void hrtimer_setup(struct hrtimer *timer,
		   enum hrtimer_restart (*function)(struct hrtimer *),
		   clockid_t clock_id, enum hrtimer_mode mode)
{
	timer->base = &clock_base;
	clock_base.get_time = &hrtimer_get_time;
	timer->function = function;
}

void hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
		u64 range_ns, const enum hrtimer_mode mode)
{}

void __icmp_send(struct sk_buff *skb, int type, int code, __be32 info,
		const struct ip_options *opt)
{
	unit_log_printf("; ", "icmp_send type %d, code %d", type, code);
}

void icmp6_send(struct sk_buff *skb, u8 type, u8 code, u32 info,
		const struct in6_addr *force_saddr,
		const struct inet6_skb_parm *parm)
{
	unit_log_printf("; ", "icmp6_send type %d, code %d", type, code);
}

int idle_cpu(int cpu)
{
	return mock_check_error(&mock_cpu_idle);
}

ssize_t import_iovec(int type, const struct iovec __user *uvector,
		unsigned int nr_segs, unsigned int fast_segs,
		struct iovec **iov, struct iov_iter *iter)
{
	ssize_t size;
	unsigned int i;

	*iov = kmalloc(nr_segs*sizeof(struct iovec), GFP_KERNEL);
	if (mock_check_error(&mock_import_iovec_errors))
		return -EINVAL;
	size = 0;
	for (i = 0; i < nr_segs; i++) {
		size += uvector[i].iov_len;
		(*iov)[i] = uvector[i];
	}
	iov_iter_init(iter, type, *iov, nr_segs, size);
	return size;
}

int import_ubuf(int rw, void __user *buf, size_t len, struct iov_iter *i)
{
	if (mock_check_error(&mock_import_ubuf_errors))
		return -EACCES;
	iov_iter_ubuf(i, rw,  buf, len);
	return 0;
}

int inet6_add_offload(const struct net_offload *prot, unsigned char protocol)
{
	return 0;
}

int inet6_add_protocol(const struct inet6_protocol *prot, unsigned char num)
{
	return 0;
}

int inet6_del_offload(const struct net_offload *prot, unsigned char protocol)
{
	return 0;
}

int inet6_del_protocol(const struct inet6_protocol *prot, unsigned char num)
{
	return 0;
}

int inet6_getname(struct socket *sock, struct sockaddr *uaddr, int peer)
{
	return 0;
}

int inet6_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	return 0;
}

int inet6_register_protosw(struct inet_protosw *p)
{
	if (mock_check_error(&mock_register_protosw_errors))
		return -EINVAL;
	return 0;
}

int inet6_release(struct socket *sock)
{
	return 0;
}

void inet6_unregister_protosw(struct inet_protosw *p) {}

int inet_add_offload(const struct net_offload *prot, unsigned char protocol)
{
	return 0;
}

int inet_add_protocol(const struct net_protocol *prot, unsigned char num)
{
	return 0;
}

int inet_del_offload(const struct net_offload *prot, unsigned char protocol)
{
	return 0;
}

int inet_del_protocol(const struct net_protocol *prot, unsigned char num)
{
	return 0;
}

int inet_dgram_connect(struct socket *sock, struct sockaddr *uaddr,
		       int addr_len, int flags)
{
	return 0;
}

int inet_getname(struct socket *sock, struct sockaddr *uaddr, int peer)
{
	return 0;
}

int inet_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	return 0;
}

int inet_recvmsg(struct socket *sock, struct msghdr *msg, size_t size,
		int flags)
{
	return 0;
}

void inet_register_protosw(struct inet_protosw *p)
{}

int inet_release(struct socket *sock)
{
	return 0;
}

int inet_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
	return 0;
}

void inet_unregister_protosw(struct inet_protosw *p)
{}

void __init_swait_queue_head(struct swait_queue_head *q, const char *name,
		struct lock_class_key *key)
{}

void init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{}

void __init_waitqueue_head(struct wait_queue_head *wq_head, const char *name,
			   struct lock_class_key *)
{}

void iov_iter_init(struct iov_iter *i, unsigned int direction,
			const struct iovec *iov, unsigned long nr_segs,
			size_t count)
{
	direction &= READ | WRITE;
	i->iter_type = ITER_IOVEC | direction;
	i->__iov = iov;
	i->nr_segs = nr_segs;
	i->iov_offset = 0;
	i->count = count;
}

void iov_iter_revert(struct iov_iter *i, size_t bytes)
{
	unit_log_printf("; ", "iov_iter_revert %lu", bytes);
}

int ip6_datagram_connect(struct sock *sk, struct sockaddr *addr, int addr_len)
{
	return 0;
}

struct dst_entry *ip6_dst_lookup_flow(struct net *net, const struct sock *sk,
		struct flowi6 *fl6, const struct in6_addr *final_dst)
{
	struct rtable *route;

	if (mock_check_error(&mock_route_errors))
		return ERR_PTR(-EHOSTUNREACH);
	route = malloc(sizeof(struct rtable));
	if (!route) {
		FAIL(" malloc failed");
		return ERR_PTR(-ENOMEM);
	}
	atomic_set(&route->dst.__rcuref.refcnt, 1);
	route->dst.ops = &mock_dst_ops;
	route->dst.dev = &mock_net_device;
	route->dst.obsolete = 0;
	if (!routes_in_use)
		routes_in_use = unit_hash_new();
	unit_hash_set(routes_in_use, route, "used");
	return &route->dst;
}

unsigned int ip6_mtu(const struct dst_entry *dst)
{
	return mock_mtu;
}

int ip6_xmit(const struct sock *sk, struct sk_buff *skb, struct flowi6 *fl6,
	     u32 mark, struct ipv6_txoptions *opt, int tclass, u32 priority)
{
	char buffer[200];
	const char *prefix = " ";

	if (mock_check_error(&mock_ip6_xmit_errors)) {
		kfree_skb(skb);
		return -ENETDOWN;
	}
	if (mock_xmit_prios_offset == 0)
		prefix = "";
	mock_xmit_prios_offset += snprintf(
			mock_xmit_prios + mock_xmit_prios_offset,
			sizeof(mock_xmit_prios) - mock_xmit_prios_offset,
			"%s%d", prefix, tclass >> 4);
	if (mock_xmit_log_verbose)
		homa_print_packet(skb, buffer, sizeof(buffer));
	else
		homa_print_packet_short(skb, buffer, sizeof(buffer));
	unit_log_printf("; ", "xmit %s", buffer);
	if (mock_xmit_log_homa_info) {
		struct homa_skb_info *homa_info;

		homa_info = homa_get_skb_info(skb);
		unit_log_printf("; ", "homa_info: wire_bytes %d, data_bytes %d, seg_length %d, offset %d",
				homa_info->wire_bytes, homa_info->data_bytes,
				homa_info->seg_length, homa_info->offset);
	}
	kfree_skb(skb);
	return 0;
}

int ip_queue_xmit(struct sock *sk, struct sk_buff *skb, struct flowi *fl)
{
	const char *prefix = " ";
	char buffer[200];

	if (mock_check_error(&mock_ip_queue_xmit_errors)) {
		/* Latest data (as of 1/2019) suggests that ip_queue_xmit
		 * frees packets after errors.
		 */
		kfree_skb(skb);
		return -ENETDOWN;
	}
	if (mock_xmit_prios_offset == 0)
		prefix = "";
	mock_xmit_prios_offset += snprintf(
			mock_xmit_prios + mock_xmit_prios_offset,
			sizeof(mock_xmit_prios) - mock_xmit_prios_offset,
			"%s%d", prefix, ((struct inet_sock *) sk)->tos>>5);
	if (mock_xmit_log_verbose)
		homa_print_packet(skb, buffer, sizeof(buffer));
	else
		homa_print_packet_short(skb, buffer, sizeof(buffer));
	unit_log_printf("; ", "xmit %s", buffer);
	if (mock_xmit_log_homa_info) {
		struct homa_skb_info *homa_info;

		homa_info = homa_get_skb_info(skb);
		unit_log_printf("; ", "homa_info: wire_bytes %d, data_bytes %d",
				homa_info->wire_bytes, homa_info->data_bytes);
	}
	kfree_skb(skb);
	return 0;
}

unsigned int ipv4_mtu(const struct dst_entry *dst)
{
	return mock_mtu;
}

struct rtable *ip_route_output_flow(struct net *net, struct flowi4 *flp4,
		const struct sock *sk)
{
	struct rtable *route;

	if (mock_check_error(&mock_route_errors))
		return ERR_PTR(-EHOSTUNREACH);
	route = malloc(sizeof(struct rtable));
	if (!route) {
		FAIL(" malloc failed");
		return ERR_PTR(-ENOMEM);
	}
	atomic_set(&route->dst.__rcuref.refcnt, 1);
	route->dst.ops = &mock_dst_ops;
	route->dst.dev = &mock_net_device;
	route->dst.obsolete = 0;
	if (!routes_in_use)
		routes_in_use = unit_hash_new();
	unit_hash_set(routes_in_use, route, "used");
	return route;
}

int ip4_datagram_connect(struct sock *sk, struct sockaddr *uaddr,
		int addr_len)
{
	return 0;
}

void device_set_wakeup_capable(struct device *dev, bool capable)
{}

void device_wakeup_disable(struct device *dev)
{}

int device_wakeup_enable(struct device *dev)
{
	return 0;
}

int filp_close(struct file *, fl_owner_t id)
{
	return 0;
}

struct file *filp_open(const char *, int, umode_t)
{
	return NULL;
}

void __fortify_panic(const u8 reason, const size_t avail, const size_t size)
{
	FAIL(" __fortify_panic invoked");

	/* API prohibits return. */
	while (1) ;
}

ssize_t kernel_read(struct file *file, void *buf, size_t count, loff_t *pos)
{
	return 0;
}

ssize_t kernel_write(struct file *file, const void *buf, size_t count,
		loff_t *pos)
{
	return 0;
}

void kfree(const void *block)
{
	if (block == NULL)
		return;
	UNIT_HOOK("kfree");
	if (!kmallocs_in_use || unit_hash_get(kmallocs_in_use, block) == NULL) {
		FAIL(" %s on unknown block %p", __func__, block);
		return;
	}
	unit_hash_erase(kmallocs_in_use, block);
	free((void *) block);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
void kfree_skb_reason(struct sk_buff *skb, enum skb_drop_reason reason)
#else
void __kfree_skb(struct sk_buff *skb)
#endif
{
	int i;
	struct skb_shared_info *shinfo = skb_shinfo(skb);

	skb->users.refs.counter--;
	if (skb->users.refs.counter > 0)
		return;
	skb_dst_drop(skb);
	if (!skbs_in_use || unit_hash_get(skbs_in_use, skb) == NULL) {
		FAIL(" kfree_skb on unknown sk_buff");
		return;
	}
	unit_hash_erase(skbs_in_use, skb);
	while (shinfo->frag_list) {
		struct sk_buff *next = shinfo->frag_list->next;

		kfree_skb(shinfo->frag_list);
		shinfo->frag_list = next;
	}
	for (i = 0; i < shinfo->nr_frags; i++)
		put_page(skb_frag_page(&shinfo->frags[i]));
	free(skb->head);
	free(skb);
}

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	return mock_kmalloc(size, gfpflags);
}

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
void __might_sleep(const char *file, int line)
{
	UNIT_HOOK("might_sleep");
}
#endif

void *mock_kmalloc(size_t size, gfp_t flags)
{
	void *block;

	UNIT_HOOK("kmalloc");
	if (mock_check_error(&mock_kmalloc_errors))
		return NULL;
	if (unit_hash_size(spinlocks_held)  > 0 &&
	    (flags & ~__GFP_ZERO) != GFP_ATOMIC)
		FAIL(" Incorrect flags 0x%x passed to mock_kmalloc; expected GFP_ATOMIC (0x%x)",
		     flags, GFP_ATOMIC);
	block = malloc(size);
	if (!block) {
		FAIL(" malloc failed");
		return NULL;
	}
	if (flags & __GFP_ZERO)
		memset(block, 0, size);
	if (!kmallocs_in_use)
		kmallocs_in_use = unit_hash_new();
	unit_hash_set(kmallocs_in_use, block, "used");
	return block;
}

void *__kmalloc_noprof(size_t size, gfp_t flags)
{
	return mock_kmalloc(size, flags);
}

void kvfree(const void *addr)
{
	kfree(addr);
}

void *__kvmalloc_node_noprof(DECL_BUCKET_PARAMS(size, b), gfp_t flags, int node)
{
	return mock_kmalloc(size, flags);
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	if (mock_check_error(&mock_kthread_create_errors))
		return ERR_PTR(-EACCES);
	return &mock_task;
}

int kthread_stop(struct task_struct *k)
{
	unit_log_printf("; ", "kthread_stop");
	return 0;
}

#ifdef CONFIG_DEBUG_LIST
bool __list_add_valid(struct list_head *new, struct list_head *prev,
		      struct list_head *next)
{
	return true;
}
#endif

bool __list_add_valid_or_report(struct list_head *new, struct list_head *prev,
				struct list_head *next)
{
	return true;
}

#ifdef CONFIG_DEBUG_LIST
bool __list_del_entry_valid(struct list_head *entry)
{
	return true;
}
#endif

bool __list_del_entry_valid_or_report(struct list_head *entry)
{
	return true;
}

void __local_bh_enable_ip(unsigned long ip, unsigned int cnt) {}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void lock_acquire(struct lockdep_map *lock, unsigned int subclass,
		  int trylock, int read, int check,
		  struct lockdep_map *nest_lock, unsigned long ip)
{}

void lockdep_rcu_suspicious(const char *file, const int line, const char *s)
{}
#endif

int lock_is_held_type(const struct lockdep_map *lock, int read)
{
	return 0;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void lock_release(struct lockdep_map *lock, unsigned long ip)
{}
#endif

void lock_sock_nested(struct sock *sk, int subclass)
{
	mock_active_locks++;
	sk->sk_lock.owned = 1;
}

ssize_t __modver_version_show(struct module_attribute *a,
		struct module_kobject *b, char *c)
{
	return 0;
}

void __mutex_init(struct mutex *lock, const char *name,
			 struct lock_class_key *key)
{

}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void mutex_lock_nested(struct mutex *lock, unsigned int subclass)
#else
void mutex_lock(struct mutex *lock)
#endif
{
	mock_active_locks++;
}

void mutex_unlock(struct mutex *lock)
{
	UNIT_HOOK("unlock");
	mock_active_locks--;
}

int netif_receive_skb(struct sk_buff *skb)
{
	struct homa_data_hdr *h = (struct homa_data_hdr *)
			skb_transport_header(skb);
	unit_log_printf("; ", "netif_receive_skb, id %llu, offset %d",
			be64_to_cpu(h->common.sender_id), ntohl(h->seg.offset));
	return 0;
}

void preempt_count_add(int val)
{
	int i;

	for (i = 0; i < val; i++)
		preempt_disable();
}

void preempt_count_sub(int val)
{
	int i;

	for (i = 0; i < val; i++)
		preempt_enable();
}

long prepare_to_wait_event(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry, int state)
{
	UNIT_HOOK("prepare_to_wait");
	if (mock_check_error(&mock_prepare_to_wait_errors))
		return mock_prepare_to_wait_status;
	return 0;
}

int _printk(const char *format, ...)
{
	int len = strlen(mock_printk_output);
	int available;
	va_list ap;

	available = sizeof(mock_printk_output) - len;
	if (available >= 10) {
		if (len != 0) {
			strcpy(mock_printk_output + len, "; ");
			len += 2;
			available -= 2;
		}
		va_start(ap, format);

		/* Skip initial characters of format that are used to
		 * indicate priority.
		 */
		if (format[0] == 1)
			format += 2;
		vsnprintf(mock_printk_output + len, available, format, ap);
		va_end(ap);

		/* Remove trailing newline. */
		len += strlen(mock_printk_output + len);
		if (mock_printk_output[len-1]  == '\n')
			mock_printk_output[len-1] = 0;
	}
	return 0;
}

struct proc_dir_entry *proc_create(const char *name, umode_t mode,
				   struct proc_dir_entry *parent,
				   const struct proc_ops *proc_ops)
{
	struct proc_dir_entry *entry = malloc(40);

	if (!entry) {
		FAIL(" malloc failed");
		return ERR_PTR(-ENOMEM);
	}
	if (!proc_files_in_use)
		proc_files_in_use = unit_hash_new();
	unit_hash_set(proc_files_in_use, entry, "used");
	return entry;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
int proc_dointvec(struct ctl_table *table, int write,
		     void __user *buffer, size_t *lenp, loff_t *ppos)
#else
int proc_dointvec(const struct ctl_table *table, int write,
		     void __user *buffer, size_t *lenp, loff_t *ppos)
#endif
{
	return 0;
}

void proc_remove(struct proc_dir_entry *de)
{
	if (!de)
		return;
	if (!proc_files_in_use
			|| unit_hash_get(proc_files_in_use, de) == NULL) {
		FAIL(" %s on unknown dir_entry", __func__);
		return;
	}
	unit_hash_erase(proc_files_in_use, de);
	free(de);

}

int proto_register(struct proto *prot, int alloc_slab)
{
	return 0;
}

void proto_unregister(struct proto *prot) {}

void *__pskb_pull_tail(struct sk_buff *skb, int delta)
{
	return NULL;
}

bool queue_work_on(int cpu, struct workqueue_struct *wq,
		   struct work_struct *work)
{
	return true;
}

void _raw_spin_lock(raw_spinlock_t *lock)
{
	mock_record_locked(lock);
	mock_total_spin_locks++;
}

void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)
{
	UNIT_HOOK("spin_lock");
	mock_record_locked(lock);
	mock_total_spin_locks++;
}

void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
{
	UNIT_HOOK("spin_lock");
	mock_record_locked(lock);
	mock_total_spin_locks++;
}

void __raw_spin_lock_init(raw_spinlock_t *lock, const char *name,
			  struct lock_class_key *key, short inner)
{}

int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	UNIT_HOOK("spin_lock");
	if (mock_check_error(&mock_trylock_errors))
		return 0;
	mock_record_locked(lock);
	mock_total_spin_locks++;
	return 1;
}

void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
{
	UNIT_HOOK("unlock");
	mock_record_unlocked(lock);
}

void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	UNIT_HOOK("unlock");
	mock_record_unlocked(lock);
}

void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	mock_record_unlocked(lock);
}

int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock)
{
	UNIT_HOOK("spin_lock");
	if (mock_check_error(&mock_spin_lock_held))
		return 0;
	mock_record_locked(lock);
	return 1;
}

bool rcu_is_watching(void)
{
	return true;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
int rcu_read_lock_any_held(void)
{
	return 1;
}

int rcu_read_lock_held(void)
{
	return 0;
}

int rcu_read_lock_bh_held(void)
{
	return 0;
}
#endif

void __rcu_read_lock(void)
{}

void __rcu_read_unlock(void)
{}

bool rcuref_get_slowpath(rcuref_t *ref)
{
	return true;
}

void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t) {}

int register_pernet_subsys(struct pernet_operations *)
{
	return 0;
}

void release_sock(struct sock *sk)
{
	mock_active_locks--;
	sk->sk_lock.owned = 0;
}

void remove_wait_queue(struct wait_queue_head *wq_head,
		struct wait_queue_entry *wq_entry)
{}

void schedule(void)
{
	UNIT_HOOK("schedule");
}

signed long schedule_timeout(signed long timeout)
{
	UNIT_HOOK("schedule_timeout");

	/* Result is time remaining in timeout. */
	return timeout - 1;
}

int __SCT__cond_resched(void)
{
	return 0;
}

int __SCT__might_resched(void)
{
	return 0;
}

void __SCT__preempt_schedule(void)
{}

void security_sk_classify_flow(const struct sock *sk,
		struct flowi_common *flic)
{}

void __show_free_areas(unsigned int filter, nodemask_t *nodemask,
		int max_zone_idx)
{}

void sk_common_release(struct sock *sk)
{}

int sk_set_peek_off(struct sock *sk, int val)
{
	return 0;
}

void sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb,
		enum skb_drop_reason reason)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	kfree_skb(skb);
#else
	__kfree_skb(skb);
#endif
}

int skb_copy_datagram_iter(const struct sk_buff *from, int offset,
		struct iov_iter *iter, int size)
{
	size_t bytes_left = size;

	if (mock_check_error(&mock_copy_data_errors))
		return -EFAULT;
	if (bytes_left > iter->count) {
		unit_log_printf("; ", "%s needs %lu bytes, but iov_iter has only %lu",
				__func__, bytes_left, iter->count);
		return 0;
	}
	while (bytes_left > 0) {
		struct iovec *iov = (struct iovec *) iter_iov(iter);
		u64 int_base = (u64) iov->iov_base;
		size_t chunk_bytes = iov->iov_len;

		if (chunk_bytes > bytes_left)
			chunk_bytes = bytes_left;
		unit_log_printf("; ",
				"%s: %lu bytes to 0x%llx: ", __func__,
				chunk_bytes, int_base);
		unit_log_data(NULL, from->data + offset + size - bytes_left,
				chunk_bytes);
		bytes_left -= chunk_bytes;
		iter->count -= chunk_bytes;
		iov->iov_base = (void *) (int_base + chunk_bytes);
		iov->iov_len -= chunk_bytes;
		if (iov->iov_len == 0)
			iter->__iov++;
	}
	return 0;
}

struct sk_buff *skb_dequeue(struct sk_buff_head *list)
{
	return __skb_dequeue(list);
}

void skb_dump(const char *level, const struct sk_buff *skb, bool full_pkt)
{}

void *skb_pull(struct sk_buff *skb, unsigned int len)
{
	if ((skb_tail_pointer(skb) - skb->data) < len)
		FAIL(" sk_buff underflow during %s", __func__);
	skb->len -= len;
	return skb->data += len;
}

void *skb_push(struct sk_buff *skb, unsigned int len)
{
	skb->data -= len;
	skb->len += len;
	if (unlikely(skb->data < skb->head))
		FAIL(" sk_buff underflow during %s", __func__);
	return skb->data;
}

void *skb_put(struct sk_buff *skb, unsigned int len)
{
	unsigned char *result = skb_tail_pointer(skb);

	skb->tail += len;
	skb->len += len;
	return result;
}

void skb_queue_purge_reason(struct sk_buff_head *list,
			    enum skb_drop_reason reason)
{
	while (skb_queue_len(list) > 0)
		kfree_skb(__skb_dequeue(list));
}

struct sk_buff *skb_segment(struct sk_buff *head_skb,
		netdev_features_t features)
{
	struct sk_buff *skb1, *skb2;
	struct homa_data_hdr h;
	int offset, length;

	/* Split the existing packet into two packets. */
	memcpy(&h, skb_transport_header(head_skb), sizeof(h));
	offset = ntohl(h.seg.offset);
	length = homa_data_len(head_skb);
	skb1 = mock_skb_alloc(&ipv6_hdr(head_skb)->saddr, &h.common, length/2,
			offset);
	offset += length/2;
	h.seg.offset = htonl(offset);
	skb2 = mock_skb_alloc(&ipv6_hdr(head_skb)->saddr, &h.common, length/2,
			offset);
	skb2->next = NULL;
	skb1->next = skb2;
	return skb1;
}

int sock_common_getsockopt(struct socket *sock, int level, int optname,
		char __user *optval, int __user *optlen)
{
	return 0;
}

int sock_common_setsockopt(struct socket *sock, int level, int optname,
		sockptr_t optval, unsigned int optlen)
{
	return 0;
}

int sock_no_accept(struct socket *sock, struct socket *newsock,
		struct proto_accept_arg *arg)
{
	return 0;
}

int sock_no_listen(struct socket *sock, int backlog)
{
	return 0;
}

int sock_no_mmap(struct file *file, struct socket *sock,
		struct vm_area_struct *vma)
{
	return 0;
}

int sock_no_shutdown(struct socket *sock, int how)
{
	return 0;
}

ssize_t sock_no_sendpage(struct socket *sock, struct page *page, int offset,
		size_t size, int flags)
{
	return 0;
}

int sock_no_socketpair(struct socket *sock1, struct socket *sock2)
{
	return 0;
}

void __tasklet_hi_schedule(struct tasklet_struct *t)
{}

void tasklet_init(struct tasklet_struct *t,
		void (*func)(unsigned long), unsigned long data)
{}

void tasklet_kill(struct tasklet_struct *t)
{}

void unregister_net_sysctl_table(struct ctl_table_header *header)
{
	UNIT_LOG("; ", "unregister_net_sysctl_table");
}

void unregister_pernet_subsys(struct pernet_operations *)
{}

void vfree(const void *block)
{
	if (!vmallocs_in_use || unit_hash_get(vmallocs_in_use, block) == NULL) {
		FAIL(" %s on unknown block", __func__);
		return;
	}
	unit_hash_erase(vmallocs_in_use, block);
	free((void *) block);
}

int vfs_fsync(struct file *file, int datasync)
{
	return 0;
}

void wait_for_completion(struct completion *x) {}

long wait_woken(struct wait_queue_entry *wq_entry, unsigned int mode,
		long timeout)
{
	return 0;
}

int __wake_up(struct wait_queue_head *wq_head, unsigned int mode,
		int nr_exclusive, void *key)
{
	if (!mock_log_wakeups)
		return 0;
	if (nr_exclusive == 1)
		unit_log_printf("; ", "wake_up");
	else
		unit_log_printf("; ", "wake_up_all");
	return 0;
}

void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode,
		      int nr)
{
	if (!mock_log_wakeups)
		return;
	unit_log_printf("; ", "wake_up_locked");
}

int wake_up_process(struct task_struct *tsk)
{
	unit_log_printf("; ", "wake_up_process pid %d", tsk ? tsk->pid : -1);
	return 0;
}

void __warn_printk(const char *s, ...) {}

int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned int mode,
		int sync, void *key)
{
	return 0;
}

/**
 * mock_alloc_hnet: Allocate a new struct homa_net.
 * @homa:   struct homa that the homa_net will be associated with.
 * Return:  The new homa_net.
 */
struct homa_net *mock_alloc_hnet(struct homa *homa)
{
	struct homa_net *hnet;

	if (mock_num_hnets >= MOCK_MAX_NETS) {
		FAIL("Max number of network namespaces (%d) exceeded",
		     MOCK_MAX_NETS);
		return &mock_hnets[0];
	}
	hnet = &mock_hnets[mock_num_hnets];
	homa_net_init(hnet, &mock_nets[mock_num_hnets], homa);
	mock_num_hnets++;
	return hnet;
}

/**
 * mock_alloc_pages() - Called instead of alloc_pages when Homa is compiled
 * for unit testing.
 */
struct page *mock_alloc_pages(gfp_t gfp, unsigned int order)
{
	struct page *page;

	if (mock_check_error(&mock_alloc_page_errors))
		return NULL;
	page = (struct page *)malloc(PAGE_SIZE << order);
	if (!pages_in_use)
		pages_in_use = unit_hash_new();
	unit_hash_set(pages_in_use, page, (char *)1);
	return page;
}

/**
 * mock_check_error() - Determines whether a method should simulate an error
 * return.
 * @errorMask:  Address of a variable containing a bit mask, indicating
 *              which of the next calls should result in errors.
 *
 * Return:      zero means the function should behave normally; 1 means return
 *              an error
 */
int mock_check_error(int *errorMask)
{
	int result = *errorMask & 1;
	*errorMask = *errorMask >> 1;
	return result;
}

/**
 * mock_clear_xmit_prios() - Remove all information from the list of
 * transmit priorities.
 */
void mock_clear_xmit_prios(void)
{
	mock_xmit_prios_offset = 0;
	mock_xmit_prios[0] = 0;
}

#ifndef __STRIP__ /* See strip.py */
/**
 * mock_compound_order() - Replacement for compound_order function.
 */
unsigned int mock_compound_order(struct page *page)
{
	unsigned int result;

	if (mock_compound_order_mask & 1)
		result = 0;
	else
		result = HOMA_SKB_PAGE_ORDER;
	mock_compound_order_mask >>= 1;
	return result;
}
#endif /* See strip.py */

/**
 * mock_cpu_to_node() - Replaces cpu_to_node to determine NUMA node for
 * a CPU.
 */
int mock_cpu_to_node(int core)
{
	if (mock_numa_mask & (1<<core))
		return 1;
	return 0;
}

/**
 * mock_data_ready() - Invoked through sk->sk_data_ready; logs a message
 * to indicate that it was invoked.
 * @sk:    Associated socket; not used here.
 */
void mock_data_ready(struct sock *sk)
{
	unit_log_printf("; ", "sk->sk_data_ready invoked");
}

struct dst_entry *mock_dst_check(struct dst_entry *dst, __u32 cookie)
{
	if (mock_check_error(&mock_dst_check_errors))
		return NULL;
	return dst;
}

/**
 * mock_get_clock() - Replacement for homa_clock; allows time to be
 * controlled by unit tests.
 */
u64 mock_get_clock(void)
{
	if (mock_next_clock_val < mock_num_clock_vals) {
		mock_next_clock_val++;
		return mock_clock_vals[mock_next_clock_val - 1];
	}
	mock_clock += mock_clock_tick;
	return mock_clock;
}

/**
 * This function is invoked through dst->dst_ops.mtu. It returns the
 * maximum size of packets that the network can transmit.
 * @dst_entry:   The route whose MTU is desired.
 */
unsigned int mock_get_mtu(const struct dst_entry *dst)
{
	return mock_mtu;
}

void mock_get_page(struct page *page)
{
	int64_t ref_count = (int64_t) unit_hash_get(pages_in_use, page);

	if (ref_count == 0)
		FAIL(" unallocated page passed to %s", __func__);
	else
		unit_hash_set(pages_in_use, page, (void *) (ref_count+1));
}

void *mock_net_generic(const struct net *net, unsigned int id)
{
	struct homa_net *hnet;
	int i;

	if (id != homa_net_id)
		return NULL;
	for (i = 0; i < MOCK_MAX_NETS; i++) {
		hnet = &mock_hnets[i];
		if (hnet->net == net)
			return hnet;
	}
	return NULL;
}

/**
 * mock_page_refs() - Returns current reference count for page (0 if no
 * such page exists).
 */
int mock_page_refs(struct page *page)
{
	return (int64_t) unit_hash_get(pages_in_use, page);
}

/**
 * mock_page_to_nid() - Replacement for page_to_nid function.
 */
int mock_page_to_nid(struct page *page)
{
	int result;

	if (mock_page_nid_mask & 1)
		result = 1;
	else
		result = 0;
	mock_page_nid_mask >>= 1;
	return result;
}

void mock_preempt_disable()
{
	mock_preempt_disables++;
}

void mock_preempt_enable()
{
	if (mock_preempt_disables == 0)
		FAIL(" preempt_enable invoked without preempt_disable");
	mock_preempt_disables--;
}

int mock_processor_id()
{
	return pcpu_hot.cpu_number;
}

void mock_put_page(struct page *page)
{
	int64_t ref_count = (int64_t) unit_hash_get(pages_in_use, page);

	if (ref_count == 0)
		FAIL(" unallocated page passed to %s", __func__);
	else {
		ref_count--;
		if (ref_count == 0) {
			unit_hash_erase(pages_in_use, page);
			free(page);
		} else {
			unit_hash_set(pages_in_use, page, (void *) ref_count);
		}
	}
}

/**
 * mock_rcu_read_lock() - Called instead of rcu_read_lock when Homa is compiled
 * for unit testing.
 */
void mock_rcu_read_lock(void)
{
	mock_active_rcu_locks++;
}

/**
 * mock_rcu_read_unlock() - Called instead of rcu_read_unlock when Homa is
 * compiled for unit testing.
 */
void mock_rcu_read_unlock(void)
{
	if (mock_active_rcu_locks == 0)
		FAIL(" rcu_read_unlock called without rcu_read_lock");
	mock_active_rcu_locks--;
}

void mock_record_locked(void *lock)
{
	if (!spinlocks_held)
		spinlocks_held = unit_hash_new();
	if (unit_hash_get(spinlocks_held, lock) != NULL)
		FAIL(" locking lock 0x%p when already locked", lock);
	else
		unit_hash_set(spinlocks_held, lock, "locked");
}

void mock_record_unlocked(void *lock)
{
	if (!spinlocks_held || unit_hash_get(spinlocks_held, lock) == NULL) {
		FAIL(" unlocking lock 0x%p that isn't locked", lock);
		return;
	}
	unit_hash_erase(spinlocks_held, lock);
}

/**
 * mock_register_net_sysctl() - Called instead of register_net_sysctl
 * when Homa is compiled for unit testing.
 */
struct ctl_table_header *mock_register_net_sysctl(struct net *net,
		const char *path, struct ctl_table *table)
{
	if (mock_check_error(&mock_register_sysctl_errors))
		return NULL;
	return (struct ctl_table_header *)11111;
}

int mock_rht_init(struct rhashtable *ht,
		    const struct rhashtable_params *params)
{
	if (mock_check_error(&mock_rht_init_errors))
		return -EINVAL;
	return rhashtable_init(ht, params);
}

void *mock_rht_lookup_get_insert_fast(struct rhashtable *ht,
				      struct rhash_head *obj,
				      const struct rhashtable_params params)
{
	if (mock_check_error(&mock_rht_insert_errors))
		return ERR_PTR(-EINVAL);
	return rhashtable_lookup_get_insert_fast(ht, obj, params);
}

void *mock_rht_walk_next(struct rhashtable_iter *iter)
{
	void *result;

	if (!mock_rht_walk_results)
		return rhashtable_walk_next(iter);
	if (mock_rht_num_walk_results == 0)
		return NULL;
	result = *mock_rht_walk_results;
	mock_rht_walk_results++;
	mock_rht_num_walk_results--;
	return result;
}

void mock_rpc_hold(struct homa_rpc *rpc)
{
	mock_rpc_holds++;
	atomic_inc(&rpc->refs);
}

void mock_rpc_put(struct homa_rpc *rpc)
{
	if (atomic_read(&rpc->refs) == 0)
		FAIL("homa_rpc_put invoked when RPC has no active holds");
	mock_rpc_holds--;
	atomic_dec(&rpc->refs);
}

/**
 * mock_set_clock_vals() - Specify one or more clock values to be returned
 * by the next calls to homa_clock(). The list of arguments must be
 * terminated by a zero value (which will not be used as a clock value).
 * @t:    The first clock reading to return.
 */
void mock_set_clock_vals(u64 t, ...)
{
	va_list args;

	mock_clock_vals[0] = t;
	mock_num_clock_vals = 1;
	va_start(args, t);
	while (mock_num_clock_vals < MAX_CLOCK_VALS) {
		u64 time = va_arg(args, u64);

		if (time == 0)
			break;
		mock_clock_vals[mock_num_clock_vals] = time;
		mock_num_clock_vals++;
	}
	va_end(args);
	mock_next_clock_val = 0;
}

/**
 * mock_set_core() - Set internal state that indicates the "current core".
 * @num:     Integer identifier for a core.
 */
void mock_set_core(int num)
{
	pcpu_hot.cpu_number = num;
}

/**
 * mock_set_ipv6() - Invoked by some tests to make them work when tests
 * are run with --ipv4. Changes the socket to an IPv6 socket and sets
 * mock_mtu and mock_ipv6.
 * @hsk:     Socket to reset for IPv6, if it's currently set for IPv4.
 */
void mock_set_ipv6(struct homa_sock *hsk)
{
	mock_ipv6 = true;
	mock_mtu -= hsk->ip_header_length - sizeof(struct ipv6hdr);
	hsk->ip_header_length = sizeof(struct ipv6hdr);
	hsk->sock.sk_family = AF_INET6;
}

/**
 * mock_skb_alloc() - Allocate and return a packet buffer. The buffer is
 * initialized as if it just arrived from the network.
 * @saddr:        IPv6 address to use as the sender of the packet, in
 *                network byte order.
 * @h:            Header for the buffer; actual length and contents depend
 *                on the type. If NULL then no Homa header is added;
 *                extra_bytes of total space will be allocated for the
 *                skb, initialized to zero.
 * @extra_bytes:  How much additional data to add to the buffer after
 *                the header.
 * @first_value:  Determines the data contents: the first u32 will have
 *                this value, and each successive u32 will increment by 4.
 *
 * Return:        A packet buffer containing the information described above.
 *                The caller owns this buffer and is responsible for freeing it.
 */
struct sk_buff *mock_skb_alloc(struct in6_addr *saddr, struct homa_common_hdr *h,
		int extra_bytes, int first_value)
{
	int header_size, ip_size, data_size, shinfo_size;
	struct sk_buff *skb;
	unsigned char *p;

	if (h) {
		switch (h->type) {
		case DATA:
			header_size = sizeof(struct homa_data_hdr);
			break;
#ifndef __STRIP__ /* See strip.py */
		case GRANT:
			header_size = sizeof(struct homa_grant_hdr);
			break;
#endif /* See strip.py */
		case RESEND:
			header_size = sizeof(struct homa_resend_hdr);
			break;
		case RPC_UNKNOWN:
			header_size = sizeof(struct homa_rpc_unknown_hdr);
			break;
		case BUSY:
			header_size = sizeof(struct homa_busy_hdr);
			break;
#ifndef __STRIP__ /* See strip.py */
		case CUTOFFS:
			header_size = sizeof(struct homa_cutoffs_hdr);
			break;
		case FREEZE:
			header_size = sizeof(struct homa_freeze_hdr);
			break;
#endif /* See strip.py */
		case NEED_ACK:
			header_size = sizeof(struct homa_need_ack_hdr);
			break;
		case ACK:
			header_size = sizeof(struct homa_ack_hdr);
			break;
		default:
			header_size = sizeof(struct homa_common_hdr);
			break;
		}
	} else {
		header_size = 0;
	}
	skb = malloc(sizeof(struct sk_buff));
	memset(skb, 0, sizeof(*skb));
	if (!skbs_in_use)
		skbs_in_use = unit_hash_new();
	unit_hash_set(skbs_in_use, skb, "used");

	ip_size = mock_ipv6 ? sizeof(struct ipv6hdr) : sizeof(struct iphdr);
	data_size = SKB_DATA_ALIGN(ip_size + header_size + extra_bytes);
	shinfo_size = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	if (h) {
		skb->head = malloc(data_size + shinfo_size);
		memset(skb->head, 0, data_size + shinfo_size);
	} else {
		skb->head = malloc(extra_bytes);
		memset(skb->head, 0, extra_bytes);

	}
	skb->data = skb->head;
	skb_reset_tail_pointer(skb);
	skb->end = skb->tail + data_size;
	skb_reserve(skb, ip_size);
	skb_reset_transport_header(skb);
	if (header_size != 0) {
		p = skb_put(skb, header_size);
		memcpy(skb->data, h, header_size);
	}
	if (h && extra_bytes != 0) {
		p = skb_put(skb, extra_bytes);
		unit_fill_data(p, extra_bytes, first_value);
	}
	skb->users.refs.counter = 1;
	if (mock_ipv6) {
		ipv6_hdr(skb)->version = 6;
		ipv6_hdr(skb)->saddr = *saddr;
		ipv6_hdr(skb)->nexthdr = IPPROTO_HOMA;
	} else {
		ip_hdr(skb)->version = 4;
		ip_hdr(skb)->saddr = saddr->in6_u.u6_addr32[3];
		ip_hdr(skb)->protocol = IPPROTO_HOMA;
		ip_hdr(skb)->check = 0;
	}
	skb->_skb_refdst = 0;
	skb->hash = 3;
	skb->next = NULL;
	skb->dev = &mock_net_device;
	return skb;
}

/**
 * Returns the number of sk_buffs currently in use.
 */
int mock_skb_count(void)
{
	return unit_hash_size(skbs_in_use);
}

void mock_sock_hold(struct sock *sk)
{
	mock_sock_holds++;
}

void mock_sock_put(struct sock *sk)
{
	if (mock_sock_holds == 0)
		FAIL("sock_put invoked when there were no active sock_holds");
	mock_sock_holds--;
}

/**
 * mock_sock_init() - Constructor for sockets; initializes the Homa-specific
 * part, and mocks out the non-Homa-specific parts.
 * @hsk:          Storage area to be initialized.
 * @hnet:         Network namesspace for the socket.
 * @port:         Port number to use for the socket, or 0 to
 *                use default.
 * Return: 0 for success, otherwise a negative errno.
 */
int mock_sock_init(struct homa_sock *hsk, struct homa_net *hnet, int port)
{
	static struct ipv6_pinfo hsk_pinfo;
	struct sock *sk = &hsk->sock;
	int saved_port;
	int err = 0;

	saved_port = hnet->prev_default_port;
	memset(hsk, 0, sizeof(*hsk));
	sk->sk_data_ready = mock_data_ready;
	sk->sk_family = mock_ipv6 ? AF_INET6 : AF_INET;
	sk->sk_socket = &mock_socket;
	sk->sk_net.net = hnet->net;
	memset(&mock_socket, 0, sizeof(mock_socket));
	refcount_set(&sk->sk_wmem_alloc, 1);
	init_waitqueue_head(&mock_socket.wq.wait);
	rcu_assign_pointer(sk->sk_wq, &mock_socket.wq);
	sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;
	if (port != 0 && port >= mock_min_default_port)
		hnet->prev_default_port = port - 1;
	err = homa_sock_init(hsk);
	hsk->is_server = true;
	if (port != 0)
		hnet->prev_default_port = saved_port;
	if (err != 0)
		return err;
	if (port != 0 && port < mock_min_default_port)
		homa_sock_bind(hnet, hsk, port);
	hsk->inet.pinet6 = &hsk_pinfo;
	mock_mtu = UNIT_TEST_DATA_PER_PACKET + hsk->ip_header_length
		+ sizeof(struct homa_data_hdr);
	mock_net_device.gso_max_size = mock_mtu;
	err = homa_pool_set_region(hsk, (void *) 0x1000000,
				   100*HOMA_BPAGE_SIZE);
	return err;
}

/**
 * mock_spin_unlock() - Called instead of spin_unlock when Homa is compiled
 * for unit testing.
 * @lock:   Lock to be released (ignored).
 */
void mock_spin_unlock(spinlock_t *lock)
{
	UNIT_HOOK("unlock");
	mock_record_unlocked(lock);
}

/**
 * mock_teardown() - Invoked at the end of each unit test to check for
 * consistency issues with all of the information managed by this file.
 * This function also cleans up the mocking information, so it is ready
 * for the next unit test.
 */
void mock_teardown(void)
{
	int count;

	pcpu_hot.cpu_number = 1;
	pcpu_hot.current_task = &mock_task;
	mock_alloc_page_errors = 0;
	mock_alloc_skb_errors = 0;
	mock_copy_data_errors = 0;
	mock_copy_to_iter_errors = 0;
	mock_copy_to_user_errors = 0;
	mock_cpu_idle = 0;
	mock_clock = 0;
	mock_clock = 0;
	mock_clock_tick = 0;
	mock_next_clock_val = 0;
	mock_num_clock_vals = 0;
	mock_tt_cycles = 0;
	mock_ipv6 = mock_ipv6_default;
	mock_dst_check_errors = 0;
	mock_import_ubuf_errors = 0;
	mock_import_iovec_errors = 0;
	mock_ip6_xmit_errors = 0;
	mock_ip_queue_xmit_errors = 0;
	mock_kmalloc_errors = 0;
	mock_kthread_create_errors = 0;
	mock_prepare_to_wait_errors = 0;
	mock_register_protosw_errors = 0;
	mock_register_sysctl_errors = 0;
	mock_rht_init_errors = 0;
	mock_rht_insert_errors = 0;
	mock_wait_intr_irq_errors = 0;
	mock_copy_to_user_dont_copy = 0;
	mock_bpage_size = 0x10000;
	mock_bpage_shift = 16;
	mock_xmit_prios_offset = 0;
	mock_xmit_prios[0] = 0;
	mock_log_rcu_sched = 0;
	mock_route_errors = 0;
	mock_trylock_errors = 0;
	mock_vmalloc_errors = 0;
	memset(&mock_task, 0, sizeof(mock_task));
	mock_prepare_to_wait_status = -ERESTARTSYS;
	mock_signal_pending = 0;
	mock_xmit_log_verbose = 0;
	mock_xmit_log_homa_info = 0;
	mock_log_wakeups = 0;
	mock_mtu = 0;
	mock_max_skb_frags = MAX_SKB_FRAGS;
	mock_numa_mask = 5;
	mock_compound_order_mask = 0;
	mock_page_nid_mask = 0;
	mock_printk_output[0] = 0;
	mock_rht_walk_results = NULL;
	mock_rht_num_walk_results = 0;
	mock_min_default_port = 0x8000;
	homa_net_id = 0;
	mock_num_hnets = 0;
	mock_peer_free_no_fail = 0;
	mock_net_device.gso_max_size = 0;
	mock_net_device.gso_max_segs = 1000;
	memset(inet_offloads, 0, sizeof(inet_offloads));
	inet_offloads[IPPROTO_TCP] = (struct net_offload __rcu *) &tcp_offload;
	memset(inet6_offloads, 0, sizeof(inet6_offloads));
	inet6_offloads[IPPROTO_TCP] = (struct net_offload __rcu *)
			&tcp_v6_offload;
	jiffies = 1100;

	count = unit_hash_size(skbs_in_use);
	if (count > 0)
		FAIL(" %u sk_buff(s) still in use after test", count);
	unit_hash_free(skbs_in_use);
	skbs_in_use = NULL;

	count = unit_hash_size(spinlocks_held);
	if (count > 0)
		FAIL(" %u spinlocks still held after test", count);
	unit_hash_free(spinlocks_held);
	spinlocks_held = NULL;

	count = unit_hash_size(kmallocs_in_use);
	if (count > 0)
		FAIL(" %u kmalloced block(s) still allocated after test", count);
	unit_hash_free(kmallocs_in_use);
	kmallocs_in_use = NULL;

	count = unit_hash_size(pages_in_use);
	if (count > 0)
		FAIL(" %u pages still allocated after test", count);
	unit_hash_free(pages_in_use);
	pages_in_use = NULL;

	count = unit_hash_size(proc_files_in_use);
	if (count > 0)
		FAIL(" %u proc file(s) still allocated after test", count);
	unit_hash_free(proc_files_in_use);
	proc_files_in_use = NULL;

	count = unit_hash_size(routes_in_use);
	if (count > 0)
		FAIL(" %u route(s) still allocated after test", count);
	unit_hash_free(routes_in_use);
	routes_in_use = NULL;

	count = unit_hash_size(vmallocs_in_use);
	if (count > 0)
		FAIL(" %u vmalloced block(s) still allocated after test", count);
	unit_hash_free(vmallocs_in_use);
	vmallocs_in_use = NULL;

	if (mock_active_locks != 0)
		FAIL(" %d (non-spin) locks still locked after test",
		     mock_active_locks);
	mock_active_locks = 0;
	mock_total_spin_locks = 0;

	if (mock_active_rcu_locks != 0)
		FAIL(" %d rcu_read_locks still active after test",
				mock_active_rcu_locks);
	mock_active_rcu_locks = 0;

	if (mock_sock_holds != 0)
		FAIL(" %d sock_holds still active after test",
				mock_sock_holds);
	mock_sock_holds = 0;

	if (mock_rpc_holds != 0)
		FAIL(" %d homa_rpc_holds still active after test",
				mock_rpc_holds);
	mock_rpc_holds = 0;

	if (mock_preempt_disables != 0)
		FAIL(" %d preempt_disables still active after test",
				mock_preempt_disables);
	mock_preempt_disables = 0;

#ifndef __STRIP__ /* See strip.py */
	memset(homa_metrics, 0, sizeof(homa_metrics));
#endif /* See strip.py */

	unit_hook_clear();
}

/**
 * mock_vmalloc() - Called instead of vmalloc when Homa is compiled
 * for unit testing.
 * @size:   Number of bytes to allocate.
 */
void *mock_vmalloc(size_t size)
{
	void *block;

	UNIT_HOOK("kmalloc");
	if (mock_check_error(&mock_vmalloc_errors))
		return NULL;
	block = malloc(size);
	if (!block) {
		FAIL(" malloc failed");
		return NULL;
	}
	if (!vmallocs_in_use)
		vmallocs_in_use = unit_hash_new();
	unit_hash_set(vmallocs_in_use, block, "used");
	return block;
}
