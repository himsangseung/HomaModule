/* Copyright (c) 2019-2023 Homa Developers
 * SPDX-License-Identifier: BSD-1-Clause
 */

/* This file contains a program that runs on one node, as part of
 * the cluster_perf test.
 */

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "dist.h"
extern "C" {
#include "homa.h"
}
#include "homa_receiver.h"
#include "test_utils.h"
#include "time_trace.h"

using std::string;

/* Command-line parameter values (note: changes to default values must
 * also be reflected in client and server constructors): */
uint32_t client_max = 1;
uint32_t client_port_max = 1;
int client_ports = 0;
int first_port = -1;
bool is_server = false;
int node_id = -1;
double net_gbps = 0.0;
bool tcp_trunc = true;
bool one_way = false;
int port_receivers = 1;
int port_threads = 1;
std::string protocol_string;
const char *protocol = "homa";
int server_ports = 1;
bool verbose = false;
std::string workload_string;
const char *workload = "100";
int unloaded = 0;
bool client_iovec = false;
bool server_iovec = false;
int inet_family = AF_INET;
int server_core = -1;
int buf_bpages = 1000;

/* Node ids for client to send requests to. */
std::vector<int> server_ids;

/** @rand_gen: random number generator. */
static std::mt19937 rand_gen(
		std::chrono::system_clock::now().time_since_epoch().count());

/**
 * struct conn_id - A 32-bit value that encodes a unique connection
 * between a TCP client and server.
 */
struct conn_id {
	/**
	 * @client_port: the index (starting at 0) of the port within
	 * the client (corresponds to a particular sending thread).
	 * This will be the low byte returned by int().
	 */
	uint8_t client_port;

	/** @client: the node index for the client (starts from zero). */
	uint8_t client;

	/**
	 * @server_port: the index (starting at 0) of a particular port
	 * within the server.
	 */
	uint8_t server_port;

	/** @server: the node index for the server (starts from 0). */
	uint8_t server;

	conn_id(uint8_t server, uint8_t server_port, uint8_t client,
			uint8_t client_port)
		: client_port(client_port), client(client),
		server_port(server_port), server(server)
	{}

	conn_id()
		: client_port(0), client(0), server_port(0), server(0)
	{}

	inline operator int()
	{
		return *(reinterpret_cast<int *>(this));
	}
};

/** @message_id: used to generate unique identifiers for outgoing messages.*/
std::atomic<uint32_t> message_id;

/**
 * @experiments: names of all known experiments (may include some that are
 * no longer in use)
 */
std::vector<std::string> experiments;

/**
 * @last_stats_time: time (in rdtsc cycles) when we last printed
 * statistics. Zero means that none of the statistics below are valid.
 */
uint64_t last_stats_time = 0;

/**
 * @last_client_rpcs: entries correspond to @experiments; total number of
 * client RPCs completed by that experiment as of the last time we printed
 * statistics.
 */
std::vector<uint64_t> last_client_rpcs;

/**
 * @last_client_bytes_out: entries correspond to @experiments; total amount
 * of data sent in request messages by client RPCs in that experiment as
 * of the last time we printed statistics.
 */
std::vector<uint64_t> last_client_bytes_out;

/**
 * @last_client_bytes_in: entries correspond to @experiments; total
 * amount of data received in response messages for client RPCs in that
 * experiment as of the last time we printed statistics.
 */
std::vector<uint64_t> last_client_bytes_in;

/**
 * @last_total_rtt: entries correspond to @experiments; total amount of
 * elapsed time for all client RPCs in that experiment (units of rdtsc cycles)
 * as of the last time we printed statistics.
 */
std::vector<uint64_t> last_total_rtt;

/**
 * @last_lag: entries correspond to @experiments; total lag (measured in rdtsc
 * cycles) for all clients in that experiment, as of the last time we printed
 * statistics.
 */
std::vector<uint64_t> last_lag;

/**
 * @last_backups: entries correspond to @experiments; total # of backed-up
 * sends for client RPCs issued by that experiment as of the last time we
 * printed statistics.
 */
std::vector<uint64_t> last_backups;

/**
 * @last_server_rpcs: entries correspond to @experiments; total # of server
 * RPCs handled by that experiment as of the last time we printed statistics.
 */
std::vector<uint64_t> last_server_rpcs;

/**
 * @last_server_bytes_in: entries correspond to @experiments; total amount
 * of data in incoming requests handled by that experiment as of the last
 * time we printed statistics.
 */
std::vector<uint64_t> last_server_bytes_in;

/**
 * @last_server_bytes_out: entries correspond to @experiments; total amount
 * of data in responses sent by that experiment as of the last time we printed
 * statistics.
 */
std::vector<uint64_t> last_server_bytes_out;

/**
 * @last_per_server_rpcs: server->requests for each individual server,
 * as of the last time we printed statistics.
 */
std::vector<uint64_t> last_per_server_rpcs;

/** @log_file: where log messages get printed. */
FILE* log_file = stdout;

enum Msg_Type {NORMAL, VERBOSE};

/** @log_level: only print log messages if they have a level <= this value. */
Msg_Type log_level = NORMAL;

extern void log(Msg_Type type, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * @cmd_lock: held whenever a command is executing.  Used to ensure that
 * operations such as statistics printing don't run when commands such
 * as "stop" are changing the client or server structure.
 */
std::mutex cmd_lock;

/**
 * @fd_locks: used to synchronize concurrent accesses to the same fd
 * (indexed by fd).
 */
#define MAX_FDS 10000
std::atomic_bool fd_locks[MAX_FDS];

/**
 * @kfreeze_count: number of times that kfreeze has been evoked since
 * the last time a client was created; used to eliminate redundant
 * freezes that waste time.
 */
int kfreeze_count = 0;

/**
 * @debug: values set with the "debug" command; typically used to
 * trigger various debugging behaviors.
 */
int64_t debug[5];

/**
 * fatal() - Invoked when fatal errors occur: exits the application.
 */
void fatal()
{
	fflush(stdout);
	fflush(stderr);
	_exit(1);
}

/**
 * print_help() - Print out usage information for this program.
 * @name:   Name of the program (argv[0])
 */
void print_help(const char *name)
{
	printf("Usage: cp_node [command]\n\n"
		"If there are command-line options, they constitute a single command\n"
		"to execute, after which cp_node will print statistics every second.\n\n"
		"If there are no command-line options, then cp_node enters a loop reading\n"
		"lines from standard input and executing them as commands. The following\n"
		"commands are supported, each followed by a list of options supported\n"
		"by that command:\n\n"
	        "client [options]      Start one or more client threads\n");
	printf("    --buf-bpages      Number of bpages to allocate in the buffer poool for\n"
		"                      incoming messages (default: %d)\n",
			buf_bpages);
	printf("    --client-max      Maximum number of outstanding requests from a single\n"
		"                      client machine (divided equally among client ports)\n"
		"                      (default: %d)\n", client_max);
	printf("    --exp             Name of the experiment in which these client threads\n");
	printf("                      will be participating; used to label measurement data\n");
	printf("                      (defaults to <protocol>_<workload>)\n");
	printf("    --first-port      Lowest port number to use for each server (default: \n");
	printf("                      4000 for Homa, 5000 for TCP)\n");
	printf("    --first-server    Id of first server node (default: 1, meaning node1)\n");
	printf("    --gbps            Target network utilization, including only message data,\n"
		"                      Gbps; 0 means send continuously (default: %.1f)\n",
			net_gbps);
	printf("    --id              Id of this node; a value of I >= 0 means requests will\n"
		"                      not be sent to nodeI (default: -1)\n");
        printf("    --ipv6            Use IPv6 instead of IPv4\n");
	printf("    --no-trunc        For TCP, allow messages longer than Homa's limit\n");
	printf("    --one-way         Make all response messages 100 B, instead of the same\n"\
		"                      size as request messages\n");
	printf("    --ports           Number of ports on which to send requests (one\n"
		"                      sending thread per port (default: %d)\n",
		client_ports);
	printf("    --port-receivers  Number of threads to listen for responses on each\n"
		"                      port (default: %d). Zero means senders wait for their\n"
		"                      own requests synchronously\n",
			port_receivers);
	printf("    --protocol        Transport protocol to use: homa or tcp (default: %s)\n",
			protocol);
	printf("    --server-nodes    Number of nodes running server threads (default: 1)\n");
	printf("    --server-ports    Number of server ports on each server node\n"
		"                      (default: %d)\n",
			server_ports);
	printf("    --servers         Comma-separated list of integer ids to use as server\n");
	printf("                      nodes; if specified, overrides --first-server and\n"
		"                      --server-nodes\n");
	printf("    --unloaded        Nonzero means run test in special mode for collecting\n"
		"                      baseline data, with the given number of measurements\n"
		"                      per length in the distribution (Homa only, default: 0)\n");
	printf("    --workload        Name of distribution for request lengths (e.g., 'w1')\n"
		"                      or integer for fixed length (default: %s)\n\n",
			workload);
	printf("debug value value ... Set one or more int64_t values that may be used for\n"
		"                      various debugging purposes\n\n");
	printf("dump_times file [exp] Log RTT times (and lengths) for clients running\n");
	printf("                      experiment exp to file; if exp is omitted, dump\n");
	printf("                      all RTTs\n\n");
	printf("exit                  Exit the application\n\n");
	printf("log [options] [msg]   Configure logging as determined by the options. If\n"
		"                      there is an \"option\" that doesn't start with \"--\",\n"
		"                      then it and all of the remaining words are printed to\n"
		"                      the log as a message.\n");
	printf("    --file            Name of log file to use for future messages (\"-\"\n"
		"                      means use standard output)\n");
	printf("    --level           Log level: either normal or verbose\n\n");
	printf("server [options]      Start serving requests on one or more ports\n");
	printf("    --buf-bpages      Number of bpages to allocate in the buffer poool for\n"
		"                      incoming messages (default: %d)\n",
			buf_bpages);
	printf("    --exp             Name of the experiment in which these server ports\n");
	printf("                      will be participating; used to label measurement data\n");
	printf("                      (defaults to <protocol>_<workload>)\n");
	printf("    --first-port      Lowest port number to use (default: 4000 for Homa,\n");
	printf("                      5000 for TCP)\n");
	printf("    --iovec           Use homa_replyv instead of homa_reply\n");
	printf("    --ipv6            Use IPv6 instead of IPv4\n");
	printf("    --pin             All server threads will be restricted to run only\n"
	        "                      on the givevn core\n");
	printf("    --protocol        Transport protocol to use: homa or tcp (default: %s)\n",
			protocol);
	printf("    --port-threads    Number of server threads to service each port\n"
		"                      (Homa only, default: %d)\n",
			port_threads);
	printf("    --ports           Number of ports to listen on (default: %d)\n\n",
			server_ports);
	printf("stop [options]        Stop existing client and/or server threads; each\n"
		"                      option must be either 'clients' or 'servers'\n\n");
	printf(" tt [options]         Manage time tracing:\n");
	printf("     freeze           Stop recording time trace information until\n"
		"                      print has been invoked\n");
	printf("     kfreeze          Freeze the kernel's internal timetrace\n");
	printf("     print file       Dump timetrace information to file\n");
}

/**
 * log() - Print a message to the current log file
 * @type:   Kind of message (NORMAL or VERBOSE); used to control degree of
 *          log verbosity
 * @format: printf-style format string, followed by printf-style arguments.
 */
void log(Msg_Type type, const char *format, ...)
{
	char buffer[1000];
	struct timespec now;
	va_list args;

	if (type > log_level)
		return;
	va_start(args, format);
	clock_gettime(CLOCK_REALTIME, &now);

	vsnprintf(buffer, sizeof(buffer), format, args);
	fprintf(log_file, "%010lu.%09lu %s", now.tv_sec, now.tv_nsec, buffer);
}

inline void parse_type(const char *s, char **end, int *value)
{
	*value = strtol(s, end, 0);
}

inline void parse_type(const char *s, char **end, int64_t *value)
{
	*value = strtoll(s, end, 0);
}

inline void parse_type(const char *s, char **end, double *value)
{
	*value = strtod(s, end);
}

/**
 * parse() - Parse a value of a particular type from an argument word.
 * @words:     Words of a command being parsed.
 * @i:         Index within words of a word expected to contain an integer
 *             value (may be outside the range of words, in which case an
 *             error message is printed).
 * @value:     The parsed value corresponding to @words[i] is stored here,
 *             if the function completes successfully.
 * @format:    Name of option being parsed (for use in error messages).
 * @type_name: Human-readable name for ValueType (for use in error messages).
 * Return:     Nonzero means success, zero means an error occurred (and a
 *             message was printed).
 */
template<typename ValueType>
int parse(std::vector<string> &words, unsigned i, ValueType *value,
		const char *option, const char *type_name)
{
	ValueType num;
	char *end;

	if (i >= words.size()) {
		printf("No value provided for %s\n", option);
		return 0;
	}
	parse_type(words[i].c_str(), &end, &num);
	if (*end != 0) {
		printf("Bad value '%s' for %s; must be %s\n",
				words[i].c_str(), option, type_name);
		return 0;
	}
	*value = num;
	return 1;
}

/**
 * log_affinity() - Log a message listing the core affinity for the
 * current thread.
 */
void log_affinity()
{
	cpu_set_t cores;
	if (sched_getaffinity(0, sizeof(cores), &cores) != 0) {
		log(NORMAL, "ERROR: couldn't read core affinities: %s",
				strerror(errno));
		return;
	}
	int total = CPU_COUNT(&cores);
	std::string list = "";
	for (int i = 0; total > 0; i++) {
		if (!CPU_ISSET(i, &cores))
			continue;
		total--;
		if (!list.empty())
			list.append(" ");
		list.append(std::to_string(i));
	}
	log(NORMAL, "Core affinities: %s\n", list.c_str());
}

/**
 * kfreeze() - Freeze the kernel-level timetrace.
 */
void kfreeze()
{
#ifndef __STRIP__ /* See strip.py */
	kfreeze_count++;
	if (kfreeze_count > 1)
		return;
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		log(NORMAL, "ERROR: kfreeze couldn't open Homa socket: %s\n",
				strerror(errno));
		return;
	}
	if (ioctl(fd, HOMAIOCFREEZE, NULL) != 0)
		log(NORMAL, "ERROR: HOMAIOCFREEZE ioctl failed: %s\n",
				strerror(errno));
	close(fd);
#endif /* See strip.py */
}

/**
 * struct message_header - The first few bytes of each message (request or
 * response) have the structure defined here. The client initially specifies
 * this information in the request, and the server returns the information
 * in the response.
 */
struct message_header {
	/**
	 * @length: total number of bytes in the message, including this
	 * header.
	 */
	int length:30;

	/** @freeze: true means the recipient should freeze its time trace. */
	unsigned int freeze:1;

	/**
	 * @short_response: true means responses should only be 100 bytes,
	 * regardless of the request length.
	 */
	unsigned int short_response:1;

	/**
	 * @cid: uniquely identifies the connection between a client
	 * and a server.
	 */
	conn_id cid;

	/**
	 * @msg_id: unique identifier for this message among all those
	 * from a given client machine.
	 */
	uint32_t msg_id;
};

/**
 * class spin_lock - Implements simple spin lock guards: lock is acquired by
 * constructor, released by destructor.
 */
class spin_lock {
public:
	spin_lock(std::atomic_bool *mutex)
		: mutex(mutex)
	{
		do {
		    /* mutex.exchange() always invalidates the cache line
		     * mutex resides in, regardless of whether it succeeded
		     * in updating the value. To reduce cache invalidation
		     * traffic, wait until we observe the lock to be free.
		     */
		    while (mutex->load(std::memory_order_relaxed)) {
			/* Do nothing */
		    }
		} while (mutex->exchange(1, std::memory_order_acquire));
	}

	~spin_lock()
	{
		mutex->store(0, std::memory_order_release);
	}

    protected:
	std::atomic_bool *mutex;
};

/**
 * class tcp_connection - Handles the reading and writing of TCP messages
 * from/to a given peer. Incoming messages may arrive in several chunks
 * spaced out in time, and outgoing messages may have to be sent in
 * multiple chunks because the stream backed up. This class keeps track
 * of the state of partial messages.
 */
class tcp_connection {
public:
	tcp_connection(int fd, uint32_t epoll_id, int port,
			sockaddr_in_union peer);
	size_t pending();
	int read(bool loop, std::function<void (message_header *header)> func);
	bool send_message(message_header *header);
	void set_epoll_events(int epoll_fd, uint32_t events);
	bool xmit();

	/** @fd: File descriptor to use for reading and writing data. */
	int fd;

	/**
	 * @epoll_id: identifier for this connection, which will be stored
	 * in the u32 field of the data for epoll events for this
	 * connection. */
	uint32_t epoll_id;

	/**
	 * @port: Port number associated with this connection (listen port
	 * for servers, outgoing port for clients). Used for error messages.
	 */
	int port;

	/**
	 * @peer: Address of the machine on the other end of this connection.
	 */
	sockaddr_in_union peer;

	/**
	 * @bytes_received: nonzero means we have read part of an incoming
	 * request; the value indicates how many bytes have been received
	 * so far.
	 */
	int bytes_received;

	/**
	 * @header: will eventually hold the first bytes of an incoming
	 * message. If @bytes_received is less than the size of this value,
	 * then it has not yet been fully read.
	 */
	message_header header;

	/**
	 * @outgoing: queue of headers for messages waiting to be
	 * transmitted. The first entry may have been partially transmitted.
	 */
	std::deque<message_header> outgoing;

	/*
	 * @bytes_sent: Nonzero means we have sent part of the first message
	 * in outgoing; the value indicates how many bytes have been
	 * successfully transmitted.
	 */
	int bytes_sent;

	/**
	 * @epoll_events: OR-ed combination of epoll events such as EPOLLIN
	 * currently enabled for this connection.
	 */
	uint32_t epoll_events;

	/**
	 * @error_message: holds human-readable error information after
	 * an error.
	 */
	char error_message[200];
};

/**
 * tcp_connection:: tcp_connection() - Constructor for tcp_connection objects.
 * @fd:        File descriptor from which to read data.
 * @epoll_id:  Identifier to store in the u32 data field of epoll events
 *             for this connection.
 * @port:      Port number associated with this connection; used for messages.
 * @peer:      Address of the machine we're reading from; used for messages.
 */
tcp_connection::tcp_connection(int fd, uint32_t epoll_id, int port,
		sockaddr_in_union peer)
	: fd(fd)
	, epoll_id(epoll_id)
        , port(port)
	, peer(peer)
	, bytes_received(0)
        , header()
        , outgoing()
        , bytes_sent(0)
        , epoll_events(0)
{
}

/**
 * pending() - Return a count of the number of messages currently
 * waiting to be transmitted (nonzero means the connection is backed up).
 */
inline size_t tcp_connection::pending()
{
	return outgoing.size();
}

/**
 * tcp_connection::read() - Reads more data from a TCP connection and calls
 * a function to handle complete messages, if any.
 * @loop:      If true, this function will read repeatedly from the
 *             socket, stopping only when there is no more data to read
 *             (the socket must be in nonblocking mode). If false, only
 *             one read call will be issued.
 * @func:      Function to call when there is a complete message; the argument
 *             to the function is a pointer to the standard header from the
 *             message. Func may be called multiple times in a single
 *             invocation of this method.
 * Return:     Zero means success; nonzero means the socket was closed
 *             by the peer, or there was an error; a human-readable message
 *	       will be left in @error_message.
 */
int tcp_connection::read(bool loop,
		std::function<void (message_header *header)> func)
{
	char buffer[100000];
	char *next;

	while (1) {
		int count = ::read(fd, buffer, sizeof(buffer));
		if (count <= 0) {
			if ((count < 0) && ((errno == EAGAIN)
					|| (errno == EWOULDBLOCK)))
				return 0;
			if ((count == 0) || ((count < 0)
					&& (errno == ECONNRESET))) {
				/* Connection was closed by the client. */
				snprintf(error_message, sizeof(error_message),
					 "TCP connection on port %d (fd %d) closed by peer %s",
					port, fd, print_address(&peer));
				return 1;
			}

			/* At this point count < 0. */
			if (errno == EFAULT) {
				/* As of 6/2020, the system call above
				 * sometimes returns EFAULT for no apparent
				 * reason (particularly under high load).
				 * Retrying seems to work...
				 */
				log(NORMAL, "WARNING: tcp_connect::read "
						"retrying after EFAULT\n");
				continue;
			}
			log(NORMAL, "ERROR: read failed for TCP connection on "
					"port %d (fd %d) to %s: %s (%d)\n",
					port, fd, print_address(&peer),
					strerror(errno), errno);
			snprintf(error_message, sizeof(error_message),
					"Error reading from TCP connection on "
					"port %d (fd %d) to %s: %s", port, fd,
					print_address(&peer), strerror(errno));
			return 1;

		}

		if ((count >= 4) && (strncmp(buffer, "GET ", 4) == 0)) {
			/* It looks like someone is trying to make an HTTP
			 * connection to us; that's bogus.
			 */
			log(NORMAL, "ERROR: unexpected data received from "
					"%s: %.*s\n", print_address(&peer),
					count, buffer);
			snprintf(error_message, sizeof(error_message),
					"Unexpected data received from %s",
					print_address(&peer));
			return 1;
		}

		/*
		 * Process incoming bytes (could contains parts of multiple
		 * requests). The first 4 bytes of each request give its
		 * length.
		 */
		next = buffer;
		while (count > 0) {
			/* First, fill in the message header with incoming data
			 * (there's no guarantee that a single read will return
			 * all of the bytes needed for these).
			 */
			int header_bytes = sizeof32(message_header)
				- bytes_received;
			if (header_bytes > 0) {
				if (count < header_bytes)
					header_bytes = count;
				char *dst = reinterpret_cast<char *>(&header);
				memcpy(dst + bytes_received, next, header_bytes);
				bytes_received += header_bytes;
				next += header_bytes;
				count -= header_bytes;
				if (bytes_received < sizeof32(message_header)) {
					tt("Received %d header bytes; need %d "
							"more for complete "
							"header", count,
							sizeof32(message_header)
							- bytes_received);
					break;
				}
			}

			if ((header.length > HOMA_MAX_MESSAGE_LENGTH)
					|| (header.length < 0)) {
				log(NORMAL, "ERROR: invalid message length %d "
						"from %s, closing connection\n",
						header.length,
						print_address(&peer));
				snprintf(error_message, sizeof(error_message),
						"Invalid message length %d "
						"from %s",
						header.length,
						print_address(&peer));
				return 1;
			}

			/* At this point we know the request length, so read
			 * until we've got a full request.
			 */
			int needed = header.length - bytes_received;
			if (count < needed) {
				tt("Received %d bytes for cid 0x%08x, id %d; "
						"need %d more for complete "
						"message", count, header.cid,
						header.msg_id, needed-count);
				bytes_received += count;
				break;
			}

			/* We now have a full message. */
			count -= needed;
			next += needed;
			func(&header);
			bytes_received = 0;
		}
		if (!loop)
			return 0;
	}
}

/**
 * tcp_connection::set_epoll_events() - Convenience method to set events
 * for epolling on this connection.
 * @epoll_fd:  File descriptor on which epoll events are collected and
 *             waited for.
 * @event:     OR-ed combination of EPOLLIN, EPOLLOUT, etc.
 */
void tcp_connection::set_epoll_events(int epoll_fd, uint32_t events)
{
	struct epoll_event ev;

	if (events == epoll_events)
		return;
	ev.events = events;
	ev.data.u32 = epoll_id;
	if (epoll_ctl(epoll_fd, (epoll_events == 0) ? EPOLL_CTL_ADD
			: EPOLL_CTL_MOD, fd, &ev) < 0) {
		log(NORMAL, "FATAL: couldn't add/modify epoll event: %s\n",
				strerror(errno));
		fatal();
	}
	epoll_events = events;
}

/**
 * tcp_connection::send_message() - Begin the process of sending a message
 * to a peer; the message may not be completely transmitted at the time this
 * method returns.
 * @header:     Transmitted as the first bytes of the message.
 *              If the size isn't at least as large as the header,
 *              we'll round it up.
 * Return:  true means the message was completely transmitted; false means
 * it has not been fully transmitted (xmit will need to be called later to
 * finish the job).
 */
bool tcp_connection::send_message(message_header *header)
{
	if (header->length < sizeof32(*header))
		header->length = sizeof32(*header);
	outgoing.emplace_back(*header);
	if (outgoing.size() > 1)
		return false;
	return xmit();
}

/**
 * tcp_connection::xmit() - Transmit as much data as possible on this
 * connection.
 * Return:  true means all available data has been sent; false means
 *          there is data that couldn't be sent because the stream
 *          backed up.
 */
bool tcp_connection::xmit()
{
	char buffer[100000];
	struct message_header *header;
	int start;
	int send_length;
	ssize_t result;

	while (true) {
		if (outgoing.size() == 0)
			return true;
		header = &outgoing[0];
		if (bytes_sent < sizeof32(*header)) {
			*(reinterpret_cast<message_header *>(buffer))
					= *header;
			start = bytes_sent;
		} else
			start = 0;
		send_length = header->length - bytes_sent;
		if (send_length > (sizeof32(buffer) - start))
			send_length = sizeof32(buffer) - start;
		result = send(fd, buffer + start, send_length,
				MSG_NOSIGNAL|MSG_DONTWAIT);
		if (result >= 0)
			bytes_sent += result;
		else {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				return false;
			if ((errno == EPIPE) || (errno == ECONNRESET))
				bytes_sent = header->length;
			else {
				log(NORMAL, "FATAL: error sending TCP message "
						"to %s: %s (port %d)\n",
						print_address(&peer),
						strerror(errno), port);
				fatal();
			}
		}
		if (bytes_sent < header->length) {
			tt("Sent %d bytes (out of %d) on cid 0x%08x",
					result, header->length, header->cid);
			continue;
		}
		bytes_sent = 0;
		tt("Finished sending TCP message, cid 0x%08x, id %d, length %d, "
				"%u messages queued", header->cid,
				header->msg_id, header->length,
				outgoing.size() - 1);
		outgoing.pop_front();
	}
}

/**
 * class server_metrics - Keeps statistics for a single server thread
 * (i.e. all the requests arriving via one Homa port or one TCP listen
 * socket).
 */
class server_metrics {
public:
	/** @experiment: Name of experiment for this server thread */
	std::string experiment;

	/** @requests: Total number of requests handled so far. */
	uint64_t requests;

	/**
	 * @bytes_in: Total number of bytes of message data received by this
	 * server in requests.
	 */
	uint64_t bytes_in;

	/**
	 * @bytes_out: Total number of bytes of message data sent by this
	 * server in responses.
	 */
	uint64_t bytes_out;

	server_metrics(std::string& experiment) : experiment(experiment),
			requests(0), bytes_in(0), bytes_out(0) {}
};

/**
 * @metrics: keeps track of metrics for all servers (whether Homa or TCP).
 * These are malloc-ed and must be freed eventually. This is a pointer so
 * that it doesn't get destructed
 */
std::vector<server_metrics *> metrics;

/**
 * class homa_server - Holds information about a single port used
 * to receive incoming requests, including one or more threads that
 * handle requests arriving via the port.
 */
class homa_server {
public:
	homa_server(int port, int id, int inet_family, int num_threads,
			std::string& experiment);
	~homa_server();
	void server(int thread_id, server_metrics *metrics);

	/** @id: Unique identifier for this server among all Homa servers. */
	int id;

	/** @fd: File descriptor for a Homa socket. */
	int fd;

	/** @port: Homa port number managed by this object. */
	int port;

	/**  @experiment: name of the experiment this server is running. */
	string experiment;

	/**
	 * @buf_region: mmapped region of memory in which receive buffers
	 * are alloocated.
	 */
	char *buf_region;

	/** @buf_size: number of bytes available at @buf_region. */
	size_t buf_size;

	/** @threads: One or more threads that service incoming requests*/
	std::vector<std::thread> threads;
};

/** @homa_servers: keeps track of all existing Homa servers. */
std::vector<homa_server *> homa_servers;

/**
 * homa_server::homa_server() - Constructor for homa_servers. Sets up the
 * Homa socket and starts up the threads to service the port.
 * @port:         Homa port number for this port.
 * @id:           Unique identifier for this port; used in thread identifiers
 *                for time traces.
 * @inet_family:  AF_INET or AF_INET6: determines whether we use IPv4 or IPv6.
 * @num_threads:  How many threads should collctively service requests on
 *                @port.
 * @experiment:   Name of the experiment in which this server is participating.
 */
homa_server::homa_server(int port, int id, int inet_family, int num_threads,
		std::string& experiment)
	: id(id)
        , fd(-1)
        , port(port)
	, experiment(experiment)
        , buf_region(NULL)
        , buf_size(0)
        , threads()
{
	sockaddr_in_union addr;
	struct homa_rcvbuf_args arg;

	if (std::find(experiments.begin(), experiments.end(), experiment)
			== experiments.end())
		experiments.emplace_back(experiment);

	fd = socket(inet_family, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		log(NORMAL, "FATAL: homa_server couldn't open Homa "
				"socket: %s\n",
				strerror(errno));
		fatal();
	}

	memset(&addr, 0, sizeof(addr));
	addr.in4.sin_family = inet_family;
	if (inet_family == AF_INET)
		addr.in4.sin_port = htons(port);
	else {
		addr.in6.sin6_family = AF_INET6;
		addr.in6.sin6_port = htons(port);
	}
	if (bind(fd, &addr.sa, sizeof(addr)) != 0) {
		log(NORMAL, "FATAL: homa_server couldn't bind socket "
				"to Homa port %d: %s\n", port,
				strerror(errno));
		fatal();
	}
	log(NORMAL, "Successfully bound to Homa port %d\n", port);

	buf_size = buf_bpages*HOMA_BPAGE_SIZE;
	buf_region = (char *) mmap(NULL, buf_size, PROT_READ|PROT_WRITE,
			MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
	if (buf_region == MAP_FAILED) {
		printf("Couldn't mmap buffer region for server on port %d: %s\n",
				port, strerror(errno));
		fatal();
	}
	arg.start = (uintptr_t)buf_region;
	arg.length = buf_size;
	int status = setsockopt(fd, IPPROTO_HOMA, SO_HOMA_RCVBUF, &arg,
			sizeof(arg));
	if (status < 0) {
		printf("FATAL: error in setsockopt(SO_HOMA_RCVBUF): %s\n",
				strerror(errno));
		fatal();
	}

	for (int i = 0; i < num_threads; i++) {
		server_metrics *thread_metrics = new server_metrics(experiment);
		metrics.push_back(thread_metrics);
		threads.emplace_back([this, i, thread_metrics] () {
			server(i, thread_metrics);
		});
	}
}

/**
 * homa_server::~homa_server() - Destructor for homa_servers.
 */
homa_server::~homa_server()
{
	log(NORMAL, "Homa server on port %d shutting down\n", port);
	shutdown(fd, SHUT_RDWR);
	for (std::thread &thread: threads)
		thread.join();
	close(fd);
	munmap(buf_region, buf_size);
}

/**
 * homa_server::server() - Handles incoming requests arriving on a Homa
 * socket. Normally invoked as top-level method in a thread.
 * @thread_id:   Unique identifier for this thread among all those for the port.
 * @metrics:     Used to record statistics for this thread.
 */
void homa_server::server(int thread_id, server_metrics *metrics)
{
	homa::receiver receiver(fd, buf_region);
	struct iovec vecs[HOMA_MAX_BPAGES];
	struct homa_sendmsg_args homa_args;
	int length, num_vecs, result;
	message_header *header;
	struct msghdr msghdr;
	char thread_name[50];
	int offset;

	snprintf(thread_name, sizeof(thread_name), "S%d.%d", id, thread_id);
	time_trace::thread_buffer thread_buffer(thread_name);
	if (server_core >= 0) {
		printf("Pinning thread %s to core %d\n", thread_name,
				server_core);
		pin_thread(server_core);
	}

	while (1) {
		while (1) {
			length = receiver.receive(0, 0);
			if (length >= 0)
				break;
			if ((errno == EBADF) || (errno == ESHUTDOWN)) {
				log(NORMAL, "Homa server thread %s exiting "
						"(socket closed)\n",
						thread_name);
				return;
			}
			else if ((errno != EINTR) && (errno != EAGAIN))
				log(NORMAL, "recvmsg failed: %s\n",
						strerror(errno));
		}
		header = receiver.get<message_header>(0);
		tt("Received Homa request, cid 0x%08x, id %u, length %d",
				header->cid, header->msg_id, header->length);
		if ((header->freeze) && !time_trace::frozen) {
			tt("Freezing timetrace because of request on "
					"cid 0x%08x", header->cid);
			log(NORMAL, "Freezing timetrace because of request on "
					"cid 0x%08x", int(header->cid));
			time_trace::freeze();
			kfreeze();
		}
		if ((header->short_response) && (header->length > 100)) {
			header->length = 100;
		}

		num_vecs = 0;
		offset = 0;
		while (offset < header->length) {
			size_t chunk_size = header->length - offset;
			if (chunk_size > HOMA_BPAGE_SIZE)
				chunk_size = HOMA_BPAGE_SIZE;
			vecs[num_vecs].iov_len = chunk_size;
			vecs[num_vecs].iov_base = receiver.get<char>(offset);
			offset += chunk_size;
			num_vecs++;
		}
		init_sendmsg_hdrs(&msghdr, &homa_args, vecs, num_vecs,
				  receiver.src_addr(),
				  sockaddr_size(receiver.src_addr()));
		homa_args.id = receiver.id();
		result = sendmsg(fd, &msghdr, 0);
		if (result < 0) {
			log(NORMAL, "FATAL: homa_reply failed for server "
					"port %d: %s\n",
					port, strerror(errno));
			fatal();
		}
		metrics->requests++;
		metrics->bytes_in += length;
		metrics->bytes_out += header->length;
	}
}

/**
 * class tcp_server - Holds information about a single TCP server,
 * which consists of a thread that handles requests on a given port.
 */
class tcp_server {
public:
	tcp_server(int port, int id, int num_threads, std::string& experiment);
	~tcp_server();
	void accept(int epoll_fd);
	void read(int fd, int pid);
	void server(int thread_id);

	/**
	 * @mutex: For synchronizing access to server-wide state, such
	 * as listen_fd.
	 */
	std::atomic_bool mutex;

	/** @port: Port on which we listen for connections. */
	int port;

	/** @id: Unique identifier for this server. */
	int id;

	/**  @experiment: name of the experiment this server is running. */
	string experiment;

	/** @listen_fd: File descriptor for the listen socket. */
	int listen_fd;

	/** @epoll_fd: File descriptor used for epolling. */
	int epoll_fd;

	/**
	 * @epollet: EPOLLET if this flag should be used, or 0 otherwise.
	 * We only use edge triggering if there are multiple receiving
	 * threads (it's unneeded if there's only a single thread, and
	 * it's faster not to use it).
	 */
	int epollet;

	/**
	 * @connections: Entry i contains information for a client
	 * connection on fd i, or NULL if no such connection.
	 */
	tcp_connection *connections[MAX_FDS];

	/** @metrics: Performance statistics. Not owned by this class. */
	server_metrics *metrics;

	/**
	 * @thread: Background threads that both accept connections and
	 * service requests on them.
	 */
	std::vector<std::thread> threads;

	/** @stop: True means that background threads should exit. */
	bool stop;
};

/** @tcp_servers: keeps track of all existing Homa clients. */
std::vector<tcp_server *> tcp_servers;

/**
 * tcp_server::tcp_server() - Constructor for tcp_server objects.
 * @port:         Port number on which this server should listen for incoming
 *                requests.
 * @id:           Unique identifier for this server.
 * @num_threads:  Number of threads to service this listening socket and
 *                all of the other sockets excepted from it.
 * @experiment:   Name of the experiment in which this server is participating.
 */
tcp_server::tcp_server(int port, int id, int num_threads,
		std::string& experiment)
	: mutex(0)
	, port(port)
        , id(id)
	, listen_fd(-1)
	, epoll_fd(-1)
        , epollet((num_threads > 0) ? EPOLLET : 0)
        , connections()
        , metrics()
        , threads()
        , stop(false)
{
	if (std::find(experiments.begin(), experiments.end(), experiment)
			== experiments.end())
		experiments.emplace_back(experiment);

	memset(connections, 0, sizeof(connections));
	listen_fd = socket(inet_family, SOCK_STREAM, 0);
	if (listen_fd == -1) {
		log(NORMAL, "FATAL: couldn't open server socket: %s\n",
				strerror(errno));
		fatal();
	}
	int option_value = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option_value,
			sizeof(option_value)) != 0) {
		log(NORMAL, "FATAL: couldn't set SO_REUSEADDR on listen "
				"socket: %s",
				strerror(errno));
		fatal();
	}
	if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) != 0) {
		log(NORMAL, "FATAL: couldn't set O_NONBLOCK on listen "
				"socket: %s",
				strerror(errno));
		fatal();
	}
	sockaddr_in_union addr;
	if (inet_family == AF_INET) {
		addr.in4.sin_family = AF_INET;
		addr.in4.sin_port = htons(port);
		addr.in4.sin_addr.s_addr = INADDR_ANY;
	} else {
		addr.in6.sin6_family = AF_INET6;
		addr.in6.sin6_port = htons(port);
		addr.in6.sin6_addr = in6addr_any;
	}
	if (bind(listen_fd, &addr.sa, sizeof(addr)) == -1) {
		log(NORMAL, "FATAL: couldn't bind to port %d: %s\n", port,
				strerror(errno));
		fatal();
	}
	if (listen(listen_fd, 1000) == -1) {
		log(NORMAL, "FATAL: couldn't listen on socket: %s",
				strerror(errno));
		fatal();
	}

	epoll_fd = epoll_create(10);
	if (epoll_fd < 0) {
		log(NORMAL, "FATAL: couldn't create epoll instance for "
				"TCP server: %s\n",
				strerror(errno));
		fatal();
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
		log(NORMAL, "FATAL: couldn't add listen socket to epoll: %s\n",
				strerror(errno));
		fatal();
	}

	metrics = new server_metrics(experiment);
	::metrics.push_back(metrics);

	for (int i = 0; i < num_threads; i++)
		threads.emplace_back(&tcp_server::server, this, i);
	kfreeze_count = 0;
}

/**
 * tcp_server::~tcp_server() - Destructor for TCP servers. Terminates the
 * server's background thread.
 */
tcp_server::~tcp_server()
{
	int fds[2];

	stop = true;

	/* In order to wake up the background threads, open a file that is
	 * readable and add it to the epoll set.
	 */
	if (pipe2(fds, 0) < 0) {
		log(NORMAL, "FATAL: couldn't create pipe to shutdown TCP "
				"server: %s\n", strerror(errno));
		fatal();
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fds[0];
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[0], &ev);
	if (write(fds[1], "xxxx", 4) < 0) {
		log(NORMAL, "FATAL: couldn't write to TCP shutdown pipe: %s\n",
				strerror(errno));
		fatal();
	}

	for (size_t i = 0; i < threads.size(); i++)
		threads[i].join();
	close(listen_fd);
	close(epoll_fd);
	close(fds[0]);
	close(fds[1]);
	for (unsigned i = 0; i < MAX_FDS; i++) {
		if (connections[i] != NULL) {
			if (close(i) < 0)
				log(NORMAL, "Error closing TCP connection to "
						"%s: %s\n",
						print_address(
						&connections[i]->peer),
						strerror(errno));
			delete connections[i];
			log(NORMAL, "Deleted connection at 0x%p, size %lu\n",
				connections[i], sizeof(*connections[i]));
			connections[i] = NULL;
		}
	}
}

/**
 * tcp_server::server() - Handles incoming TCP requests on a listen socket
 * and all of the connections accepted via that socket. Normally invoked as
 * top-level method in a thread. There can potentially be multiple instances
 * of this function running simultaneously.
 * @thread_id:  Unique id for this particular thread among all of the
 *              threads in this server.
 */
void tcp_server::server(int thread_id)
{
	char thread_name[50];

	snprintf(thread_name, sizeof(thread_name), "S%d.%d", id, thread_id);
	time_trace::thread_buffer thread_buffer(thread_name);
	int pid = syscall(__NR_gettid);
	if (server_core >= 0) {
		printf("Pinning thread %s to core %d\n", thread_name,
				server_core);
		pin_thread(server_core);
	}

	/* Each iteration through this loop processes a batch of epoll events. */
	while (1) {
#define MAX_EVENTS 20
		struct epoll_event events[MAX_EVENTS];
		int num_events;

		while (1) {
			num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
			if (stop)
				return;
			if (num_events >= 0)
				break;
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			log(NORMAL, "FATAL: epoll_wait failed: %s\n",
					strerror(errno));
			fatal();
		}
		tt("epoll_wait returned %d events in server pid %d",
				num_events, pid);
		for (int i = 0; i < num_events; i++) {
			int fd = events[i].data.u32;
			if (fd == listen_fd) {
				spin_lock lock_guard(&mutex);
				accept(epoll_fd);
			} else {
				spin_lock lock_guard(&fd_locks[fd]);
				if ((events[i].events & EPOLLIN) &&
						(connections[fd] != NULL))
					read(fd, pid);
				if ((events[i].events & EPOLLOUT) &&
						(connections[fd] != NULL)) {
					if (connections[fd]->xmit())
						connections[fd]->set_epoll_events(
								epoll_fd,
								EPOLLIN|epollet);
				}
			}
		}
	}
	log(NORMAL, "TCP server thread %s exiting\n", thread_name);
}

/**
 * tcp_server::accept() - Accepts a new incoming TCP connection and
 * initializes state for that connection.
 * @epoll_fd:   Used to arrange for epolling on the new connection.
 */
void tcp_server::accept(int epoll_fd)
{
	int fd;
	sockaddr_in_union client_addr;
	socklen_t addr_len = sizeof(client_addr);

	fd = ::accept4(listen_fd, &client_addr.sa, &addr_len, SOCK_NONBLOCK);
	if (fd < 0) {
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
			return;
		log(NORMAL, "FATAL: couldn't accept incoming TCP connection: "
				"%s\n", strerror(errno));
		fatal();
	}

	/* Make sure the connection appears to be coming from someone
	 * we trust (as of August 2023, at CloudLab, external sites
	 * could open connections).
	 */
	if (client_addr.in4.sin_family == AF_INET) {
		uint8_t *ipaddr = (uint8_t *) &client_addr.in4.sin_addr;
		bool is_internet = true;

		if (ipaddr[0] == 10) {
			is_internet = false;
		}
		else if (ipaddr[0] == 172 && (ipaddr[1] >= 16 && ipaddr[1] <= 31)) {
			is_internet = false;
		}
		else if (ipaddr[0] == 192 && ipaddr[1] == 168) {
			is_internet = false;
		}

		if (is_internet) {
			log(NORMAL, "ERROR: tcp_server::accept rejecting "
					"rogue TCP connection from %s\n",
					print_address(&client_addr));
			::close(fd);
			return;
		}
	}
	log(NORMAL, "tcp_server on port %d accepted connection from %s, fd %d\n",
			port, print_address(&client_addr), fd);
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
	if (fd >= MAX_FDS) {
		log(NORMAL, "FATAL: TCP socket fd %d is greater than MAX_FDS\n",
				fd);
		fatal();
	}
	spin_lock lock_guard(&fd_locks[fd]);
	tcp_connection *connection = new tcp_connection(fd, fd, port,
			client_addr);
	connections[fd] = connection;
	connection->set_epoll_events(epoll_fd, EPOLLIN|epollet);
}

/**
 * tcp_server::read() - Reads available data from a TCP connection; once an
 * entire request has been read, sends an appropriate response.
 * @fd:        File descriptor for connection; connections must hold
 *             state information for this descriptor.
 * @pid:       Pid for the thread (for messages).
 */
void tcp_server::read(int fd, int pid)
{
	int error = connections[fd]->read(epollet,
			[this, fd, pid](message_header *header) {
		metrics->requests++;
		metrics->bytes_in += header->length;
		tt("Received TCP request, cid 0x%08x, id %u, length %d, pid %d",
				header->cid, header->msg_id, header->length,
				pid);
		if ((header->freeze) && !time_trace::frozen) {
			tt("Freezing timetrace because of request on "
					"cid 0x%08x", header->cid);
			log(NORMAL, "Freezing timetrace because of request on "
					"cid 0x%08x", int(header->cid));
			time_trace::freeze();
			kfreeze();
		}
		if ((header->short_response) && (header->length > 100))
			header->length = 100;
		metrics->bytes_out += header->length;
		if (!connections[fd]->send_message(header))
			connections[fd]->set_epoll_events(epoll_fd,
					EPOLLIN|EPOLLOUT|epollet);
	});
	if (error) {
		log(NORMAL, "Closing client connection: %s\n",
				connections[fd]->error_message);
		spin_lock lock_guard(&mutex);
		if (close(fd) < 0) {
			log(NORMAL, "Error closing TCP connection to %s: %s\n",
					print_address(&connections[fd]->peer),
					strerror(errno));
		}
		delete connections[fd];
		connections[fd] = NULL;
	}
}

/**
 * class client - Holds information that is common to both Homa clients
 * and TCP clients.
 */
class client {
public:
	/**
	 * struct rinfo - Holds information about a request that we will
	 * want when we get the response.
	 */
	struct rinfo {
		/** @start_time: rdtsc time when the request was sent. */
		uint64_t start_time;

		/** @request_length: number of bytes in the request message. */
		int request_length;

		/**
		 * @active: true means the request has been sent but
		 * a response hasn't yet been received.
		 */
		bool active;

		/**
		 * @id: RPC identifier for the request (only for Homa requests).
		 */
		uint64_t id;

		rinfo() : start_time(0), request_length(0), active(false) {}
	};

	client(int id, std::string& experiment);
	virtual ~client();
	void check_completion(const char *protocol);
	int get_rinfo();
	void record(uint64_t end_time, message_header *header);
	virtual void stop_sender(void) {}

	/**
	 * @id: unique identifier for this client (index starting at
	 * 0 for the first client.
	 */
	int id;

	/**  @experiment: name of the experiment this client is running. */
	string experiment;

	/**
	 * @server_addrs: Internet addresses for each of the server ports
	 * where this client will send RPCs.
	 */
	std::vector<sockaddr_in_union> server_addrs;

	/**
	 * @server_conns: for each entry in @server_addrs, a connection
	 * identifier with all fields filled in except client_port, which
	 * will be 0.
	 */
	std::vector<conn_id> server_conns;

	/**
	 * @freeze: one entry for each node index; 1 means messages to that
	 * node should contain a flag telling the node to freeze its time trace.
	 */
	std::vector<int> freeze;

	/**
	 * @first_id: entry i contains the index in server_addrs of the first
	 * entry for the server ports on node i.
	 */
	std::vector<int> first_id;

	/**
	 * @rinfos: storage for more than enough rinfos to handle all of the
	 * outstanding requests.
	 */
	std::vector<rinfo> rinfos;

	/** @last_rinfo: index into rinfos of last slot that was allocated. */
	int last_rinfo;

	/**
	 * @receivers_running: number of receiving threads that have
	 * initialized and are ready to receive responses.
	 */
	std::atomic<size_t> receivers_running;

	/**
	 * @cycles_per_second: caches get_cycles_per_second() result.
	 */
	uint64_t cycles_per_second;

	/** @server_dist: used to select server indexes for outgoing RPCs. */
	std::uniform_int_distribution<int> server_dist;

        /**
	 * @interval_dist: generator for the time intervals between RPCs.
	 */
	std::exponential_distribution<double> interval_dist;

	/** @length_dist: Generator of message lengths. */
	dist_point_gen length_dist;

	/**
	 * @actual_lengths: a circular buffer that holds the actual payload
	 * sizes used for the most recent RPCs.
	 */
	std::vector<int> actual_lengths;

	/**
	 * @actual_rtts: a circular buffer that holds the actual round trip
	 * times (measured in rdtsc cycles) for the most recent RPCs. Entries
	 * in this array correspond to those in @actual_lengths.
	 */
	std::vector<uint64_t> actual_rtts;

	/**
	 * define NUM_CLENT_STATS: number of records in actual_lengths
	 * and actual_rtts.
	 */
#define NUM_CLIENT_STATS 500000

	/** @requests: total number of RPCs issued so far for each server. */
	std::vector<uint64_t> requests;

	/** @responses: total number of responses received so far from
	 * each server. Dynamically allocated (as of 3/2020, can't use
	 * vector with std::atomic).
	 */
	std::atomic<uint64_t> *responses;

	/**
	 * @total_requests: total number of RPCs issued so far across all
	 * servers.
	 */
	uint64_t total_requests;

	/**
	 * @total_responses: total number of responses received so far from all
	 * servers.
	 */
	std::atomic<uint64_t> total_responses;

	/**
	 * @request_bytes: total amount of data sent in all requests for
	 * which responses have been received.
	 */
	std::atomic<uint64_t> request_bytes;

	/**
	 * @response_bytes: total amount of data in all response messages
	 * received so far.
	 */
	std::atomic<uint64_t> response_bytes;

	/**
	 * @total_rtt: sum of round-trip times (in rdtsc cycles) for
	 * all responses received so far.
	 */
	std::atomic<uint64_t> total_rtt;

	/**
	 * @lag: time in rdtsc cycles by which we are running behind
	 * because client_port_max was exceeded (i.e., the request
	 * we just sent should have been sent @lag cycles ago).
	 */
	uint64_t lag;
};

/** @clients: keeps track of all existing clients. */
std::vector<client *> clients;

/**
 * client::client() - Constructor for client objects. Uses configuration
 * information from global variables to initialize.
 *
 * @id:         Unique identifier for this client (index starting at 0?)
 * @experiment: Name of experiment in which this client will participate.
 */
client::client(int id, std::string& experiment)
	: id(id)
	, experiment(experiment)
	, server_addrs()
	, server_conns()
	, freeze()
	, first_id()
        , last_rinfo(0)
	, receivers_running(0)
	, cycles_per_second(get_cycles_per_sec())
	, server_dist()
	, length_dist(workload, HOMA_MAX_MESSAGE_LENGTH)
	, actual_lengths(NUM_CLIENT_STATS, 0)
	, actual_rtts(NUM_CLIENT_STATS, 0)
	, total_requests(0)
	, total_responses(0)
	, request_bytes(0)
	, response_bytes(0)
        , total_rtt(0)
        , lag(0)
{
	if (std::find(experiments.begin(), experiments.end(), experiment)
			== experiments.end())
		experiments.emplace_back(experiment);

	server_addrs.clear();
	server_conns.clear();
	freeze.clear();
	first_id.clear();
	for (int node: server_ids) {
		char host[100];
		struct addrinfo hints;
		struct addrinfo *matching_addresses;
		sockaddr_in_union *dest;

		if (node == node_id)
			continue;
		snprintf(host, sizeof(host), "node%d", node);
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = inet_family;
		hints.ai_socktype = SOCK_DGRAM;
		int status = getaddrinfo(host, NULL, &hints,
				&matching_addresses);
		if (status != 0) {
			log(NORMAL, "FATAL: couldn't look up address "
					"for %s: %s\n",
					host, gai_strerror(status));
			fatal();
		}
		dest = reinterpret_cast<sockaddr_in_union *>
				(matching_addresses->ai_addr);
		while (((int) first_id.size()) < node)
			first_id.push_back(-1);
		first_id.push_back((int) server_addrs.size());
		for (int thread = 0; thread < server_ports; thread++) {
			dest->in4.sin_port = htons(first_port + thread);
			server_addrs.push_back(*dest);
			server_conns.emplace_back(node, thread, node_id, 0);
		}
		while (((int) freeze.size()) <= node)
			freeze.push_back(0);
		freeaddrinfo(matching_addresses);
	}

	server_dist.param(std::uniform_int_distribution<>::param_type(0,
			static_cast<int>(server_addrs.size() - 1)));

	rinfos.resize(2*client_port_max + 5);
	double avg_length = length_dist.get_mean();
	double rate = 1e09*(net_gbps/8.0)/(avg_length*client_ports);
	interval_dist = std::exponential_distribution<double>(rate);
	requests.resize(server_addrs.size());
	responses = new std::atomic<uint64_t>[server_addrs.size()];
	for (size_t i = 0; i < server_addrs.size(); i++)
		responses[i] = 0;
	log(NORMAL, "Average message length %.1f KB, rate %.2f K/sec, "
			"expected BW %.1f Gbps\n",
			avg_length*1e-3, rate*1e-3, avg_length*rate*8e-9);
	kfreeze_count = 0;
}

/**
 * Destructor for clients.
 */
client::~client()
{
	delete[] responses;
}

/**
 * check_completion() - Make sure that all outstanding requests have
 * completed; if not, generate log messages.
 * @protocol:  String that identifies the current protocol for the log
 *             message, if any.
 */
void client::check_completion(const char *protocol)
{
	string server_info;
	int incomplete = total_requests - total_responses;
	for (size_t i = 0; i < requests.size(); i++) {
		char buffer[100];
		int diff = requests[i] - responses[i];
		if (diff == 0)
			continue;
		if (!server_info.empty())
			server_info.append(", ");
		snprintf(buffer, sizeof(buffer), "node%d.%d: %d",
				server_conns[i].server,
				server_conns[i].server_port, diff);
		server_info.append(buffer);
	}
	if ((incomplete != 0) || !server_info.empty())
		log(NORMAL, "ERROR: %d incomplete %s requests (%s)\n",
				incomplete, protocol, server_info.c_str());
}

/**
 * get_rinfo() - Find an available rinfo slot and return its index in
 * rinfos.
 */
int client::get_rinfo()
{
	int next = last_rinfo;

	while (true) {
		next++;
		if (next >= static_cast<int>(rinfos.size()))
			next = 0;
		if (!rinfos[next].active) {
			rinfos[next].active = true;
			last_rinfo = next;
			return next;
		}
		if (next == last_rinfo) {
			log(NORMAL, "FATAL: ran out of rinfos (%lu in use, "
					"total_requests %ld, "
					"total_responses %ld, last_rinfo %d)\n",
					rinfos.size(), total_requests,
				        total_responses.load(), last_rinfo);
			fatal();
		}
	}
}

/**
 * record() - Records statistics about a particular request.
 * @end_time:   Completion time for the request, in rdtsc cycles.
 * @header:     The header from the response.
 */
void client::record(uint64_t end_time, message_header *header)
{
	int server_id;
	int slot = total_responses.fetch_add(1) % NUM_CLIENT_STATS;
	int64_t rtt;

	if (header->msg_id >= rinfos.size()) {
		log(NORMAL, "ERROR: msg_id (%u) exceed rinfos.size (%lu)\n",
			header->msg_id, rinfos.size());
		return;
	}
	rinfo *r = &rinfos[header->msg_id];
	if (!r->active) {
		log(NORMAL, "ERROR: response arrived for inactive msg_id %u\n",
			header->msg_id);
		return;
	}
	rtt = end_time - r->start_time;
	r->active = false;

	int kcycles = rtt>>10;
	tt("Received response, cid 0x%08x, id %u, length %d, "
			"rtt %d kcycles",
			header->cid, header->msg_id,
			header->length, kcycles);
	if ((kcycles > debug[0]) && (kcycles < debug[1])
			&& (header->length < 1500) && !time_trace::frozen) {
		freeze[header->cid.server] = 1;
		tt("Freezing timetrace because of long RTT for "
				"cid 0x%08x, id %u, length %d, kcycles %d",
				header->cid, header->msg_id, header->length,
				kcycles);
		log(NORMAL, "Freezing timetrace because of long RTT for "
				"cid 0x%08x, id %u",
				int(header->cid), header->msg_id);
		time_trace::freeze();
		kfreeze();
	}

	server_id = first_id[header->cid.server];
	if (server_id == -1) {
		log(NORMAL, "WARNING: response received from unknown "
				"cid 0x%08x\n", (int) header->cid);
		return;
	}
	server_id += header->cid.server_port;
	responses[server_id].fetch_add(1);
	request_bytes += r->request_length;
	response_bytes += header->length;
	total_rtt += rtt;
	actual_lengths[slot] = header->length;
	actual_rtts[slot] = rtt;
}

/**
 * class homa_client - Holds information about a single Homa client,
 * which consists of one thread issuing requests and one or more threads
 * receiving responses.
 */
class homa_client : public client {
public:
	homa_client(int id, std::string& experiment);
	virtual ~homa_client();
	void measure_unloaded(int count);
	uint64_t measure_rtt(int server, int length, char *buffer,
			homa::receiver *receiver);
	void receiver(int id);
	void sender(void);
	virtual void stop_sender(void);
	void timeout(homa::receiver *receiver);
	bool wait_response(homa::receiver *receiver, uint64_t rpc_id);

	/** @fd: file descriptor for Homa socket. */
	int fd;

	/**
	 * @buf_region: mmapped region of memory in which receive buffers
	 * are alloocated.
	 */
	char *buf_region;

	/** @buf_size: number of bytes available at @buf_region. */
	size_t buf_size;

	/** @stop_sending: true means the sending thread should exit ASAP. */
	bool exit_sender;

	/** @stop: true means receiving threads should exit ASAP. */
	bool exit_receivers;

	/** @server_exited:  just what you'd guess from the name. */
	bool sender_exited;

	/**
	 * @sender_buffer: used by the sender to send requests, and also
	 * by measure_unloaded; malloced, size HOMA_MAX_MESSAGE_LENGTH.
	 */
	char *sender_buffer;

	/** @receiver: threads that receive responses. */
	std::vector<std::thread> receiving_threads;

	/**
	 * @sender: thread that sends requests (may also receive
	 * responses if port_receivers is 0).
	 */
	std::optional<std::thread> sending_thread;
};

/**
 * homa_client::homa_client() - Constructor for homa_client objects.
 *
 * @id:          Unique identifier for this client (index starting at 0?).
 * @experiment:  Name of experiment in which this client will participate.
 */
homa_client::homa_client(int id, std::string& experiment)
	: client(id, experiment)
	, fd(-1)
        , buf_region(nullptr)
	, buf_size(buf_bpages*HOMA_BPAGE_SIZE)
        , exit_sender(false)
        , exit_receivers(false)
        , sender_exited(false)
        , sender_buffer(new char[HOMA_MAX_MESSAGE_LENGTH])
        , receiving_threads()
        , sending_thread()
{
	struct homa_rcvbuf_args arg;

	fd = socket(inet_family, SOCK_DGRAM, IPPROTO_HOMA);
	if (fd < 0) {
		log(NORMAL, "Couldn't open Homa socket: %s\n", strerror(errno));
		fatal();
	}

	buf_region = (char *) mmap(NULL, buf_size, PROT_READ|PROT_WRITE,
			MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
	if (buf_region == MAP_FAILED) {
		printf("Couldn't mmap buffer region for homa_client id %d: %s\n",
				id, strerror(errno));
		fatal();
	}
	arg.start = (uintptr_t)buf_region;
	arg.length = buf_size;
	int status = setsockopt(fd, IPPROTO_HOMA, SO_HOMA_RCVBUF, &arg,
			sizeof(arg));
	if (status < 0) {
		printf("FATAL: error in setsockopt(SO_HOMA_RCVBUF): %s\n",
				strerror(errno));
		fatal();
	}

	if (unloaded) {
		measure_unloaded(unloaded);
		sender_exited = true;
	} else {
		for (int i = 0; i < port_receivers; i++) {
			receiving_threads.emplace_back(&homa_client::receiver,
					this, i);
		}
		while (receivers_running < receiving_threads.size()) {
			/* Wait for the receivers to begin execution before
			 * starting the sender; otherwise the initial RPCs
			 * may appear to take a long time.
			 */
		}
		sending_thread.emplace(&homa_client::sender, this);
	}
}

/**
 * homa_client::~homa_client() - Destructor for homa_client objects;
 * will terminate threads created for this client.
 */
homa_client::~homa_client()
{
	uint64_t start = rdtsc();
	exit_sender = true;
	exit_receivers = true;
	while (!sender_exited || (total_responses != total_requests)) {
		if (to_seconds(rdtsc() - start) > 2.0)
			break;
	}
	shutdown(fd, SHUT_RDWR);
	close(fd);
	delete[] sender_buffer;
	if (sending_thread)
		sending_thread->join();
	for (std::thread &thread: receiving_threads)
		thread.join();
	munmap(buf_region, buf_size);
	check_completion("homa");
}

/**
 * homa_client::stop_sender() - Ask the sending thread to stop sending,
 * and wait until it exits (but give up if that takes too long).
 */
void homa_client::stop_sender(void)
{
	uint64_t start = rdtsc();
	exit_sender = true;
	while (1) {
		if (sender_exited) {
			if (sending_thread) {
				sending_thread->join();
				sending_thread.reset();
			}
		}
		if (to_seconds(rdtsc() - start) > 0.5)
			break;
	}
}

/**
 * homa_client::wait_response() - Wait for a response to arrive and
 * update statistics.
 * @receiver: Use this for receiving the response and managing its buffers.
 * @rpc_id:   Id of a specific RPC to wait for, or 0 for "any response".
 * Return:    True means that a response was received; false means the client
 *            has been stopped and the socket has been shut down.
 */
bool homa_client::wait_response(homa::receiver *receiver, uint64_t rpc_id)
{
	message_header *header;

	rpc_id = 0;
	ssize_t length;
	do {
		length = receiver->receive(0, rpc_id);
	} while ((length < 0) && ((errno == EAGAIN) || (errno == EINTR)));
	if (length < 0) {
		if (exit_receivers)
			return false;
		if (errno == ETIMEDOUT) {
			timeout(receiver);
			return true;
		}
		log(NORMAL, "FATAL: error in Homa recvmsg: %s (id %lu, "
				"server %s)\n",
				strerror(errno), receiver->id(),
				print_address((union sockaddr_in_union *)
					      receiver->src_addr()));
		fatal();
	}
	header = receiver->get<message_header>(0);
	if (header == nullptr) {
		log(NORMAL, "FATAL: Homa response message contained %lu bytes; "
			"need at least %lu", length, sizeof(*header));
		fatal();
	}
	uint64_t end_time = rdtsc();
	tt("Received response, cid 0x%08x, id %x, %d bytes",
			header->cid, header->msg_id, length);
	record(end_time, header);
	return true;
}


/**
 * timeout() - Invoked to process Homa timeouts (free up the rinfo struct).
 * @receiver:  Holds information about the failed RPC.
 */
void homa_client::timeout(homa::receiver *receiver)
{
	uint64_t id = receiver->id();
	for (struct rinfo &r: rinfos) {
		if (r.id == id) {
			log(NORMAL, "ERROR: Homa RPC timed out, id %lu, "
					"length %d, server %s\n",
					id, r.request_length,
					print_address((union sockaddr_in_union *)
						      receiver->src_addr()));
			r.active = false;
			return;
		}
	}
	log(NORMAL, "FATAL: couldn't find rinfo for timed out RPC id %lu\n", id);
	fatal();
}

/**
 * homa_client::sender() - Invoked as the top-level method in a thread;
 * invokes a pseudo-random stream of RPCs continuously.
 */
void homa_client::sender()
{
	message_header *header = reinterpret_cast<message_header *>(sender_buffer);
	uint64_t next_start = rdtsc();
	char thread_name[50];
	homa::receiver receiver(fd, buf_region);
	struct homa_sendmsg_args homa_args;
	struct msghdr msghdr;
	struct iovec vec[2];
	int num_vecs;

	snprintf(thread_name, sizeof(thread_name), "C%d", id);
	time_trace::thread_buffer thread_buffer(thread_name);

	while (1) {
		uint64_t now;
		int server;
		int status;
		int slot = get_rinfo();

		/* Wait until (a) we have reached the next start time
		 * and (b) there aren't too many requests outstanding.
		 */
		while (1) {
			if (exit_sender) {
				sender_exited = true;
				rinfos[slot].active = false;
				return;
			}
			now = rdtsc();
			if (now < next_start)
				continue;
			if ((total_requests - total_responses) < client_port_max)
				break;
		}

		rinfos[slot].start_time = now;
		server = server_dist(rand_gen);
		header->length = length_dist(rand_gen);
		if (header->length > HOMA_MAX_MESSAGE_LENGTH)
			header->length = HOMA_MAX_MESSAGE_LENGTH;
		if (header->length < sizeof32(*header))
			header->length = sizeof32(*header);
		rinfos[slot].request_length = header->length;
		header->cid = server_conns[server];
		header->cid.client_port = id;
		header->freeze = freeze[header->cid.server];
		header->short_response = one_way;
		header->msg_id = slot;
		tt("sending request, cid 0x%08x, id %u, length %d",
				header->cid, header->msg_id, header->length);

		if (client_iovec && (header->length > 20)) {
			vec[0].iov_base = sender_buffer;
			vec[0].iov_len = 20;
			vec[1].iov_base = sender_buffer + 20;
			vec[1].iov_len = header->length - 20;
			num_vecs = 2;
		} else {
			vec[0].iov_base = sender_buffer;
			vec[0].iov_len = header->length;
			num_vecs = 1;
		}
		init_sendmsg_hdrs(&msghdr, &homa_args, vec, num_vecs,
				  &server_addrs[server].sa,
				  sockaddr_size(&server_addrs[server].sa));
		status = sendmsg(fd, &msghdr, 0);
		if (status < 0) {
			log(NORMAL, "FATAL: error in Homa sendmsg: %s (request "
					"length %d)\n", strerror(errno),
					header->length);
			fatal();
		}
		rinfos[slot].id = homa_args.id;
		requests[server]++;
		total_requests++;
		lag = now - next_start;
		next_start += interval_dist(rand_gen)*cycles_per_second;
		if (receivers_running == 0) {
			/* There isn't a separate receiver thread; wait for
			 * the response here. */
			wait_response(&receiver, homa_args.id);
		}
	}
}

/**
 * homa_client::receiver() - Invoked as the top-level method in a thread
 * that waits for RPC responses and then logs statistics about them.
 * @receiver_id:   Unique id for this receiver within its client.
 */
void homa_client::receiver(int receiver_id)
{
	char thread_name[50];
	snprintf(thread_name, sizeof(thread_name), "R%d.%d", node_id, receiver_id);
	time_trace::thread_buffer thread_buffer(thread_name);
	homa::receiver receiver(fd, buf_region);

	receivers_running++;
	while (wait_response(&receiver, 0)) {}
}

/**
 * homa_client::measure_rtt() - Make a single request to a given server and
 * return the RTT.
 * @server:      Identifier of server to use for the request.
 * @length:      Number of message bytes in the request.
 * @buffer:      Block of memory to use for request; must
 *               contain HOMA_MAX_MESSAGE_LENGTH bytes.
 * @receiver:    Use this to receive responses.
 *
 * Return:       Round-trip time to service the request, in rdtsc cycles.
 */
uint64_t homa_client::measure_rtt(int server, int length, char *buffer,
		homa::receiver *receiver)
{
	message_header *header = reinterpret_cast<message_header *>(buffer);
	struct homa_sendmsg_args homa_args;
	struct msghdr msghdr;
	struct iovec vec;
	uint64_t start;
	int status;

	header->length = length;
	if (header->length > HOMA_MAX_MESSAGE_LENGTH)
		header->length = HOMA_MAX_MESSAGE_LENGTH;
	if (header->length < sizeof32(*header))
		header->length = sizeof32(*header);
	header->cid = server_conns[server];
	header->cid.client_port = id;
	start = rdtsc();

	vec.iov_base = buffer;
	vec.iov_len = header->length;
	init_sendmsg_hdrs(&msghdr, &homa_args, &vec, 1,
			  &server_addrs[server].sa,
			  sockaddr_size(&server_addrs[server].sa));
	status = sendmsg(fd, &msghdr, 0);
	if (status < 0) {
		log(NORMAL, "FATAL: error in Homa sendmsg: %s (request "
				"length %d)\n", strerror(errno),
				header->length);
		fatal();
	}
	do {
		status = receiver->receive(0, homa_args.id);
	} while ((status < 0) && ((errno == EAGAIN) || (errno == EINTR)));
	if (status < 0) {
		log(NORMAL, "FATAL: measure_rtt got error in recvmsg: %s "
				"(id %llu, server %s)\n",
				strerror(errno), homa_args.id,
				print_address((union sockaddr_in_union *)
					      receiver->src_addr()));
		fatal();
	}
	return rdtsc() - start;
}

/**
 * homa_client::measure_unloaded() - Gather baseline measurements of Homa
 * under best-case conditions. This method will fill in the actual_lengths
 * and actual_rtts arrays with several measurements for each message length
 * in the current workload.
 * @count:    How many samples to measure for each length in the distribution.
 */
void homa_client::measure_unloaded(int count)
{
	dist_point_gen length_dist(workload, HOMA_MAX_MESSAGE_LENGTH);
	std::vector<int> dist_sizes = length_dist.values();
	int server = 0;
	int slot;
	uint64_t ms100 = get_cycles_per_sec()/10;
	uint64_t end;
	homa::receiver receiver(fd, buf_region);

	/* Make one request for each size in the distribution, just to warm
	 * up the system.
	 */
	for (int length: dist_sizes)
		measure_rtt(server, length, sender_buffer, &receiver);

	/* Now do the real measurements. Stop with each size after 10
	 * measurements if more than 0.1 second has elapsed (otherwise
	 * this takes too long).
	 */
	slot = 0;
	for (int length: dist_sizes) {
		end = rdtsc() + ms100;
		log(NORMAL, "Starting unloaded measurement for length %d\n", length);
		for (int i = 0; i < count; i++) {
			if ((rdtsc() >= end) && (i >= 10))
				break;
			actual_lengths[slot] = length;
			actual_rtts[slot] = measure_rtt(server, length,
					sender_buffer, &receiver);
			slot++;
			if (slot >= NUM_CLIENT_STATS) {
				log(NORMAL, "WARNING: not enough space to "
						"record all unloaded RTTs\n");
				slot = 0;
			}
		}
	}
}

/**
 * class tcp_client - Holds information about a single TCP client,
 * which consists of one thread issuing requests and one thread receiving
 * responses.
 */
class tcp_client : public client {
public:
	tcp_client(int id, std::string& experiment);
	virtual ~tcp_client();
	void read(tcp_connection *connection, int pid);
	void receiver(int id);
	void sender(void);

	/**
	 * @connections: One entry for each server in server_addrs; used to
	 * communicate with that server.
	 */
	std::vector<tcp_connection *> connections;

	/**
	 * @blocked: Contains all of the connections for which there is
	 * pending output that couldn't be sent because the connection
	 * was backed up.
	 */
	std::vector<tcp_connection *> blocked;

	/** @requests: total number of message bytes sent to each server. */
	std::vector<uint64_t> bytes_sent;

	/**
	 *  @requests: total number of message bytes received from each server.
	 */
	std::atomic<uint64_t> *bytes_rcvd;

	/**
	 * @backups: total number of times that a stream was congested
	 * (many bytes queued for a server, but no response received yet)
	 * when a new message was added to the stream.
	 */
	uint64_t backups;

	/**
	 * @epoll_fd: File descriptor used by @receiving_thread to
	 * wait for epoll events.
	 */
	int epoll_fd;

	/**
	 * @epollet: EPOLLET if this flag should be used, or 0 otherwise.
	 * We only use edge triggering if there are multiple receiving
	 * threads (it's unneeded if there's only a single thread, and
	 * it's faster not to use it).
	 */
	int epollet;

	/** @stop:  True means background threads should exit. */
	bool stop;

	/** @receiver: threads that receive responses. */
	std::vector<std::thread> receiving_threads;

	/**
	 * @sender: thread that sends requests (may also receive
	 * responses if port_receivers is 0).
	 */
	std::optional<std::thread> sending_thread;
};

/**
 * tcp_client::tcp_client() - Constructor for tcp_client objects.
 *
 * @id:         Unique identifier for this client (index starting at 0?)
 * @experiment: Name of experiment in which this client will participate.
 */
tcp_client::tcp_client(int id, std::string& experiment)
	: client(id, experiment)
	, connections()
        , blocked()
        , bytes_sent()
        , bytes_rcvd(NULL)
        , backups(0)
        , epoll_fd(-1)
	, epollet((port_receivers > 1) ? EPOLLET : 0)
        , stop(false)
        , receiving_threads()
        , sending_thread()
{
	bytes_rcvd = new std::atomic<uint64_t>[server_addrs.size()];
	for (size_t i = 0; i < server_addrs.size(); i++) {
		bytes_sent.push_back(0);
		bytes_rcvd[i] = 0;
	}
	epoll_fd = epoll_create(10);
	if (epoll_fd < 0) {
		log(NORMAL, "FATAL: tcp_client couldn't create epoll "
				"instance: %s\n", strerror(errno));
		fatal();
	}

	for (uint32_t i = 0; i < server_addrs.size(); i++) {
		int fd = socket(inet_family, SOCK_STREAM, 0);
		if (fd == -1) {
			log(NORMAL, "FATAL: couldn't open TCP client "
					"socket: %s\n",
					strerror(errno));
			fatal();
		}
		if (connect(fd, reinterpret_cast<struct sockaddr *>(
				&server_addrs[i]),
				sizeof(server_addrs[i])) == -1) {
			log(NORMAL, "FATAL: client couldn't connect "
					"to %s: %s\n",
					print_address(&server_addrs[i]),
					strerror(errno));
			fatal();
		}
		int flag = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
		if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
			log(NORMAL, "FATAL: couldn't set O_NONBLOCK on socket "
					"to server %s: %s",
					print_address(&server_addrs[i]),
					strerror(errno));
			fatal();
		}
		sockaddr_in_union addr;
		socklen_t length = sizeof(addr);
		if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr),
				&length)) {
			log(NORMAL, "FATAL: getsockname failed for TCP client: "
					"%s\n", strerror(errno));
			fatal();
		}
		connections.emplace_back(new tcp_connection(fd, i,
				ntohs(addr.in4.sin_port), server_addrs[i]));
		connections[connections.size()-1]->set_epoll_events(epoll_fd,
				EPOLLIN|epollet);
	}

	for (int i = 0; i < port_receivers; i++) {
		receiving_threads.emplace_back(&tcp_client::receiver, this, i);
	}
	while (receivers_running < receiving_threads.size()) {
		/* Wait for the receivers to begin execution before
		 * starting the sender; otherwise the initial RPCs
		 * may appear to take a long time.
		 */
	}
	sending_thread.emplace(&tcp_client::sender, this);
}

/**
 * tcp_client::~tcp_client() - Destructor for tcp_client objects;
 * will terminate threads created for this client.
 */
tcp_client::~tcp_client()
{
	int fds[2];

	stop = true;

	/* In order to wake up the background thread, open a file that is
	 * readable and add it to the epoll set.
	 */
	if (pipe2(fds, 0) < 0) {
		log(NORMAL, "FATAL: couldn't create pipe to shutdown TCP "
				"server: %s\n", strerror(errno));
		fatal();
	}
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fds[0];
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[0], &ev);
	if (write(fds[1], "xxxx", 4) < 0) {
		log(NORMAL, "FATAL: couldn't write to TCP shutdown "
				"pipe: %s\n", strerror(errno));
		fatal();
	}

	if (sending_thread)
		sending_thread->join();
	for (std::thread& thread: receiving_threads)
		thread.join();

	close(fds[0]);
	close(fds[1]);
	close(epoll_fd);
	for (tcp_connection *connection: connections) {
		close(connection->fd);
		delete connection;
	}
	delete[] bytes_rcvd;
}

/**
 * tcp_client::sender() - Invoked as the top-level method in a thread;
 * invokes a pseudo-random stream of RPCs continuously.
 */
void tcp_client::sender()
{
	char thread_name[50];
	int pid = syscall(__NR_gettid);

	snprintf(thread_name, sizeof(thread_name), "C%d", id);
	time_trace::thread_buffer thread_buffer(thread_name);

	uint64_t next_start = rdtsc();
	message_header header;
	size_t max_pending = 1;

	/* Index of the next connection in blocked on which to try sending. */
	size_t next_blocked = 0;

	while (1) {
		uint64_t now;
		int server;
		int slot = get_rinfo();

		/* Wait until (a) we have reached the next start time
		 * and (b) there aren't too many requests outstanding.
		 */
		while (1) {
			if (stop) {
				rinfos[slot].active = false;
				return;
			}
			now = rdtsc();
			if ((now >= next_start)
					&& ((total_requests - total_responses)
					< client_port_max))
				break;

			/* Try to finish I/O on backed up connections. */
			if (blocked.size() == 0)
				continue;
			if (next_blocked >= blocked.size())
				next_blocked = 0;
			if (blocked[next_blocked]->xmit())
				blocked.erase(blocked.begin() + next_blocked);
			else
				next_blocked++;
		}

		rinfos[slot].start_time = now;
		server = server_dist(rand_gen);
		header.length = length_dist(rand_gen);
		if ((header.length > HOMA_MAX_MESSAGE_LENGTH) && tcp_trunc)
			header.length = HOMA_MAX_MESSAGE_LENGTH;
		rinfos[slot].request_length = header.length;
		header.cid = server_conns[server];
		header.cid.client_port = id;
		header.msg_id = slot;
		header.freeze = freeze[header.cid.server];
		header.short_response = one_way;
		size_t old_pending = connections[server]->pending();
		tt("Sending TCP request, cid 0x%08x, id %u, length %d, pid %d",
				header.cid, header.msg_id, header.length,
				pid);
		if ((!connections[server]->send_message(&header))
				&& (old_pending == 0)) {
			blocked.push_back(connections[server]);
			if (connections[server]->pending() > max_pending) {
				max_pending = connections[server]->pending();
				log(NORMAL, "max_pending now %lu for "
						"tcp_client %d\n",
						max_pending, id);
			}
		}
		if (verbose)
			log(NORMAL, "tcp_client %d.%d sent request to server %d, "
					"port %d, length %d\n",
					header.cid.client,
					header.cid.client_port,
					header.cid.server,
					header.cid.server_port,
					header.length);
		requests[server]++;
		total_requests++;
		if ((bytes_sent[server] - bytes_rcvd[server]) > 100000)
			backups++;
		bytes_sent[server] += header.length;
		lag = now - next_start;
		next_start += interval_dist(rand_gen)*cycles_per_second;
	}
}

/**
 * tcp_client::receiver() - Invoked as the top-level method in a thread
 * that waits for RPC responses and then logs statistics about them.
 * @receiver_id:  Id of this receiver (among those for the same port).
 */
void tcp_client::receiver(int receiver_id)
{
	char thread_name[50];

	snprintf(thread_name, sizeof(thread_name), "R%d.%d", id, receiver_id);
	time_trace::thread_buffer thread_buffer(thread_name);
	receivers_running++;
	int pid = syscall(__NR_gettid);

	/* Each iteration through this loop processes a batch of incoming
	 * responses
	 */
	while (1) {
#define MAX_EVENTS 20
		struct epoll_event events[MAX_EVENTS];
		int num_events;

		tt("calling epoll_wait");
		while (1) {
			num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
			if (stop)
				return;
			if (num_events > 0)
				break;
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			log(NORMAL, "FATAL: epoll_wait failed in tcp_client: "
					"%s\n",
					strerror(errno));
			fatal();
		}
		tt("epoll_wait returned %d events in client pid %d",
				num_events, pid);
		for (int i = 0; i < num_events; i++) {
			int fd = events[i].data.fd;
			tcp_connection *connection = connections[fd];
			if (events[i].events & EPOLLIN) {
				spin_lock lock_guard(&fd_locks[fd]);
				read(connection, pid);
			}
		}
	}
}

/**
 * tcp_client::read() - Is available data from a TCP connection; if an
 * entire response has now been read, records statistics for that request.
 * @connection:  TCP connection that has data available to read.
 * @pid:         Identifier for current process; used for messages.
 */
void tcp_client::read(tcp_connection *connection, int pid)
{
	int error = connection->read(epollet, [this, pid]
			(message_header *header) {
		uint64_t end_time = rdtsc();
		record(end_time, header);
		tt("Response for cid 0x%08x received by pid %d", pid);
		bytes_rcvd[first_id[header->cid.server]
				+ header->cid.server_port] += header->length;
	});
	if (error) {
		log(NORMAL, "FATAL: %s (client)\n",
				connection->error_message);
		fatal();
	}
}

/**
 * server_stats() -  Prints recent statistics collected from all
 * servers.
 * @now:   Current time in rdtsc cycles (used to compute rates for
 *         statistics).
 */
void server_stats(uint64_t now)
{
	last_per_server_rpcs.resize(metrics.size(), 0);
	last_server_rpcs.resize(experiments.size(), 0);
	last_server_bytes_in.resize(experiments.size(), 0);
	last_server_bytes_out.resize(experiments.size(), 0);

	for (size_t i = 0; i < experiments.size(); i++) {
		std::string& exp = experiments[i];
		char details[10000];
		int offset = 0;
		int length;
		uint64_t server_rpcs = 0;
		uint64_t server_bytes_in = 0;
		uint64_t server_bytes_out = 0;

		details[0] = 0;
		for (uint32_t i = 0; i < metrics.size(); i++) {
			server_metrics *smetrics = metrics[i];
			if (smetrics->experiment != exp)
				continue;
			server_rpcs += smetrics->requests;
			server_bytes_in += smetrics->bytes_in;
			server_bytes_out += smetrics->bytes_out;
			length = snprintf(details + offset,
					sizeof(details) - offset,
					"%s%lu", (offset != 0) ? " " : "",
					smetrics->requests - last_per_server_rpcs[i]);
			offset += length;
			last_per_server_rpcs[i] = smetrics->requests;
		}
		if ((last_stats_time != 0) && (server_bytes_in
				!= last_server_bytes_in[i])) {
			double elapsed = to_seconds(now - last_stats_time);
			double rpcs = (double) (server_rpcs
					- last_server_rpcs[i]);
			double in_delta = (double) (server_bytes_in
					- last_server_bytes_in[i]);
			double out_delta = (double) (server_bytes_out
					- last_server_bytes_out[i]);
			log(NORMAL, "%s servers: %.2f Kops/sec, %.2f Gbps in, "
					"%.2f Gbps out, avg. req. length "
					"%.1f bytes\n",
					exp.c_str(),
					rpcs/(1000.0*elapsed),
					8.0*in_delta/(1e09*elapsed),
					8.0*out_delta/(1e09*elapsed),
					in_delta/rpcs);
			log(NORMAL, "RPCs per %s server thread: %s\n",
					exp.c_str(), details);
		}
		last_server_rpcs[i] = server_rpcs;
		last_server_bytes_in[i] = server_bytes_in;
		last_server_bytes_out[i] = server_bytes_out;
	}
}

/**
 * client_stats() -  Prints recent statistics collected by all existing
 * clients (either TCP or Homa).
 * @now:       Current time in rdtsc cycles (used to compute rates for
 *             statistics).
 */
void client_stats(uint64_t now)
{
#define CDF_VALUES 100000
	std::vector<int> num_clients(sizeof(experiments), 0);
	size_t i;

	for (client *client: clients) {
		for (i = 0; i < experiments.size(); i++) {
			if (experiments[i] == client->experiment)
				break;
		}
		if (i == experiments.size())
			experiments.emplace_back(client->experiment);
		num_clients[i]++;
	}

	last_client_rpcs.resize(experiments.size(), 0);
	last_client_bytes_out.resize(experiments.size(), 0);
	last_client_bytes_in.resize(experiments.size(), 0);
	last_total_rtt.resize(experiments.size(), 0);
	last_lag.resize(experiments.size(), 0);
	last_backups.resize(experiments.size(), 0);

	for (i = 0; i < experiments.size(); i++) {
		std::string& exp = experiments[i];
		uint64_t client_rpcs = 0;
		uint64_t request_bytes = 0;
		uint64_t response_bytes = 0;
		uint64_t total_rtt = 0;
		uint64_t lag = 0;
		uint64_t outstanding_rpcs = 0;
		uint64_t cdf_times[CDF_VALUES];
		uint64_t backups = 0;
		int times_per_client;
		int cdf_index = 0;

		if (num_clients[i] == 0)
			continue;

		times_per_client = CDF_VALUES/num_clients[i];
		if (times_per_client > NUM_CLIENT_STATS)
			times_per_client = NUM_CLIENT_STATS;
		for (client *client: clients) {
			if (client->experiment != exp)
				continue;
			for (size_t j = 0; j < client->server_addrs.size(); j++)
				client_rpcs += client->responses[j];
			request_bytes += client->request_bytes;
			response_bytes += client->response_bytes;
			total_rtt += client->total_rtt;
			lag += client->lag;
			outstanding_rpcs += client->total_requests
				- client->total_responses;
			for (int i = 1; i <= times_per_client; i++) {
				/* Collect the most recent RTTs from the client
				 * for computing a CDF.
				 */
				int src = (client->total_responses - i)
						% NUM_CLIENT_STATS;
				if (client->actual_rtts[src] == 0) {
					/* Client hasn't accumulated
					 * times_per_client entries yet; just
					 * use what it has.
					 */
					break;
				}
				cdf_times[cdf_index] = client->actual_rtts[src];
				cdf_index++;
			}
			tcp_client *tclient = dynamic_cast<tcp_client *>(client);
			if (tclient)
				backups += tclient->backups;
		}
		std::sort(cdf_times, cdf_times + cdf_index);
		if ((last_stats_time != 0) && ((request_bytes
					!= last_client_bytes_out[i])
					|| (outstanding_rpcs != 0))){
			double elapsed = to_seconds(now - last_stats_time);
			double rpcs = (double) (client_rpcs - last_client_rpcs[i]);
			double delta_out = (double) (request_bytes
					- last_client_bytes_out[i]);
			double delta_in = (double) (response_bytes
					- last_client_bytes_in[i]);
			log(NORMAL, "%s clients: %.2f Kops/sec, %.2f Gbps out, "
					"%.2f Gbps in, RTT (us) P50 %.2f "
					"P99 %.2f P99.9 %.2f, avg. req. length "
					"%.1f bytes\n",
					exp.c_str(),
					rpcs/(1000.0*elapsed),
					8.0*delta_out/(1e09*elapsed),
					8.0*delta_in/(1e09*elapsed),
					to_seconds(cdf_times[cdf_index/2])*1e06,
					to_seconds(cdf_times[99*cdf_index/100])*1e06,
					to_seconds(cdf_times[999*cdf_index/1000])*1e06,
					delta_out/rpcs);
			double lag_fraction;
			if (lag > last_lag[i])
				lag_fraction = (to_seconds(lag
					- last_lag[i])/elapsed)
					/ num_clients[i];
			else
				lag_fraction = -(to_seconds(last_lag[i]
					- lag)/elapsed) / num_clients[i];
			if (lag_fraction >= .01)
				log(NORMAL, "Lag due to overload for %s "
						"experiment: %.1f%%\n",
						exp.c_str(), lag_fraction*100.0);
			if (backups != 0) {
				log(NORMAL, "Backed-up %s sends: %lu/%lu (%.1f%%)\n",
						exp.c_str(),
						backups - last_backups[i],
						client_rpcs - last_client_rpcs[i],
						100.0*(backups - last_backups[i])
						/(client_rpcs - last_client_rpcs[i]));
			}
		}
		if (outstanding_rpcs != 0)
			log(NORMAL, "Outstanding client RPCs for %s "
					"experiment: %lu\n",
					exp.c_str(), outstanding_rpcs);
		last_client_rpcs[i] = client_rpcs;
		last_client_bytes_out[i] = request_bytes;
		last_client_bytes_in[i] = response_bytes;
		last_total_rtt[i] = total_rtt;
		last_lag[i] = lag;
		last_backups[i] = backups;
	}
}

/**
 * log_stats() - Enter an infinite loop printing statistics to the
 * log every second. This function never returns.
 */
void log_stats()
{
	while (1) {
		sleep(1);
		std::lock_guard<std::mutex> lock(cmd_lock);
		uint64_t now = rdtsc();
		server_stats(now);
		client_stats(now);

		last_stats_time = now;
	}
}

/**
 * client_cmd() - Parse the arguments for a "client" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int client_cmd(std::vector<string> &words)
{
	int first_server = 1;
	int server_nodes = 1;
	std::string servers;
	std::string experiment;

	buf_bpages = 1000;
	client_iovec = false;
	client_max = 1;
	client_ports = 1;
	first_port = -1;
	inet_family = AF_INET;
	net_gbps = 0.0;
	port_receivers = 1;
	protocol = "homa";
	tcp_trunc = true;
	one_way = false;
	unloaded = 0;
	workload = "100";
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();

		if (strcmp(option, "--buf-bpages") == 0) {
			if (!parse(words, i+1, &buf_bpages, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--client-max") == 0) {
			if (!parse(words, i+1, (int *) &client_max,
					option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--exp") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			experiment = words[i+1];
			i++;
		} else if (strcmp(option, "--first-port") == 0) {
			if (!parse(words, i+1, &first_port, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--first-server") == 0) {
			if (!parse(words, i+1, &first_server, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--gbps") == 0) {
			if (!parse(words, i+1, &net_gbps, option, "float"))
				return 0;
			i++;
		} else if (strcmp(option, "--id") == 0) {
			if (!parse(words, i+1, &node_id, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--iovec") == 0) {
			client_iovec = true;
		} else if (strcmp(option, "--ipv6") == 0) {
			inet_family = AF_INET6;
		} else if (strcmp(option, "--no-trunc") == 0) {
			tcp_trunc = false;
		} else if (strcmp(option, "--one-way") == 0) {
			one_way = true;
		} else if (strcmp(option, "--ports") == 0) {
			if (!parse(words, i+1, &client_ports, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--port-receivers") == 0) {
			if (!parse(words, i+1, &port_receivers, option,
					"integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--protocol") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			protocol_string = words[i+1];
			protocol = protocol_string.c_str();
			i++;
		} else if (strcmp(option, "--server-nodes") == 0) {
			if (!parse(words, i+1, &server_nodes, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--server-ports") == 0) {
			if (!parse(words, i+1, &server_ports, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--servers") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n", option);
				return 0;
			}
			servers = words[i+1];
			i++;
		} else if (strcmp(option, "--unloaded") == 0) {
			if (!parse(words, i+1, &unloaded, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--workload") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			workload_string = words[i+1];
			workload = workload_string.c_str();
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}
	if (experiment.empty()) {
		experiment = protocol;
		experiment += "_";
		experiment += workload;
	}

	/* Figure out which nodes to use for servers (--servers,
	 * --server-nodes, --first-server).
	 */
	server_ids.clear();
	if (!servers.empty()) {
		std::vector<string> ids;

		split(servers.c_str(), ',', ids);
		for (std::string &id_string: ids) {
			char *end;
			int id = strtoul(id_string.c_str(), &end, 10);
			if (*end != 0) {
				printf("Bad server id '%s' in --servers "
						"option '%s'\n",
						id_string.c_str(),
						servers.c_str());
				return 0;
			}
			server_ids.push_back(id);
		}
	} else {
		for (int i = 0; i < server_nodes; i++)
			server_ids.push_back(first_server + i);
	}

	client_port_max = client_max/client_ports;
	if (client_port_max < 1)
		client_port_max = 1;

	/* Create clients. */
	for (int i = 0; i < client_ports; i++) {
		if (strcmp(protocol, "homa") == 0) {
			if (first_port == -1)
				first_port = 4000;
			clients.push_back(new homa_client(i, experiment));
		} else {
			if (first_port == -1)
				first_port = 5000;
			clients.push_back(new tcp_client(i, experiment));
		}
	}
	last_stats_time = 0;
	time_trace::cleanup();
	return 1;
}

/**
 * debug_cmd() - Parse the arguments for a "debug" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int debug_cmd(std::vector<string> &words)
{
	int64_t value;
	size_t num_debug = sizeof(debug)/sizeof(*debug);

	if (words.size() > (num_debug + 1)) {
		printf("Too many debug values; at most %lu allowed\n",
			num_debug);
	}
	for (size_t i = 1; i < words.size(); i++) {
		if (!parse(words, i, &value, "debug", "64-bit integer"))
			return 0;
		debug[i-1] = value;
	}
	return 1;
}

/**
 * dump_times_cmd() - Parse the arguments for a "dump_times" command and
 * execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int dump_times_cmd(std::vector<string> &words)
{
	FILE *f;
	time_t now;
	char time_buffer[100];
	std::string exp;

	if (words.size() == 3)
		exp = words[2];
	else if (words.size() != 2) {
		printf("Wrong # args; must be 'dump_times file [experiment]'\n");
		return 0;
	}
	f = fopen(words[1].c_str(), "w");
	if (f == NULL) {
		printf("Couldn't open file %s: %s\n", words[1].c_str(),
				strerror(errno));
		return 0;
	}

	time(&now);
	strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
			localtime(&now));
	fprintf(f, "# Round-trip times measured by cp_node at %s for "
			"experiment %s\n",
			time_buffer, exp.empty() ? "<none>" : exp.c_str());
	fprintf(f, "# --protocol %s, --workload %s, --gpbs %.1f --threads %d,\n",
			protocol, workload, net_gbps, client_ports);
	fprintf(f, "# --server-nodes %lu --server-ports %d, --client-max %d\n",
			server_ids.size(), server_ports, client_max);
	fprintf(f, "# Length   RTT (usec)\n");
	for (client *client: clients) {
		if (!exp.empty() && (client->experiment != exp))
			continue;
		uint32_t start = client->total_responses % NUM_CLIENT_STATS;
		uint32_t i = start;
		while (1) {
			if (client->actual_rtts[i] != 0) {
				fprintf(f, "%8d %12.2f\n",
						client->actual_lengths[i],
						1e06*to_seconds(
						client->actual_rtts[i]));
				client->actual_rtts[i] = 0;
			}
			i++;
			if (i >= client->actual_rtts.size())
				i = 0;
			if (i == start)
				break;
		}
	}
	fclose(f);
	return 1;
}

/**
 * info_cmd() - Parse the arguments for an "info" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int info_cmd(std::vector<string> &words)
{
	const char *workload;
	char *end;
	int mtu;

	if (words.size() != 3) {
		printf("Usage: info workload mtu\n");
		return 0;
	}
	workload = words[1].c_str();
	mtu = strtol(words[2].c_str(), &end, 0);
	if (*end != 0) {
		printf("Bad value '%s' for mtu; must be integer\n",
				words[2].c_str());
		return 0;
	}
	dist_point_gen length_dist(workload, HOMA_MAX_MESSAGE_LENGTH);
	printf("Workload %s: mean %.1f bytes, overhead %.3f\n",
			workload, length_dist.get_mean(),
			length_dist.dist_overhead(mtu));
	return 1;
}

/**
 * log_cmd() - Parse the arguments for a "log" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int log_cmd(std::vector<string> &words)
{
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();

		if (strncmp(option, "--", 2) != 0) {
			string message;
			for (unsigned j = i; j < words.size(); j++) {
				if (j != i)
					message.append(" ");
				message.append(words[j]);
			}
			message.append("\n");
			log(NORMAL, "%s", message.c_str());
			return 1;
		}

		if (strcmp(option, "--file") == 0) {
			FILE *f;
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			const char *name = words[i+1].c_str();
			if (strcmp(name, "-") == 0)
				f = stdout;
			else {
				f = fopen(name, "w");
				if (f == NULL) {
					printf("Couldn't open %s: %s\n", name,
							strerror(errno));
					return 0;
				}
				setlinebuf(f);
			}
			if (log_file != stdout)
				fclose(log_file);
			log_file = f;
			i++;
		} else if (strcmp(option, "--level") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			if (words[i+1].compare("normal") == 0)
				log_level = NORMAL;
			else if (words[i+1].compare("verbose") == 0)
				log_level = VERBOSE;
			else {
				printf("Unknown log level '%s'; must be "
						"normal or verbose\n",
						words[i+1].c_str());
				return 0;
			}
			log(NORMAL, "Log level is now %s\n",
					words[i+1].c_str());
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}
	return 1;
}

/**
 * server_cmd() - Parse the arguments for a "server" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int server_cmd(std::vector<string> &words)
{
	std::string experiment;
	buf_bpages = 1000;
	first_port = -1;
	inet_family = AF_INET;
        protocol = "homa";
	port_threads = 1;
	server_core = -1;
	server_ports = 1;
	server_iovec = false;

	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();

		if (strcmp(option, "--buf-bpages") == 0) {
			if (!parse(words, i+1, &buf_bpages, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--exp") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			experiment = words[i+1];
			i++;
		} else if (strcmp(option, "--first-port") == 0) {
			if (!parse(words, i+1, &first_port, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--iovec") == 0) {
			server_iovec = true;
		} else if (strcmp(option, "--ipv6") == 0) {
			inet_family = AF_INET6;
		} else if (strcmp(option, "--pin") == 0) {
			if (!parse(words, i+1, &server_core, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--port-threads") == 0) {
			if (!parse(words, i+1, &port_threads, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--ports") == 0) {
			if (!parse(words, i+1, &server_ports, option, "integer"))
				return 0;
			i++;
		} else if (strcmp(option, "--protocol") == 0) {
			if ((i + 1) >= words.size()) {
				printf("No value provided for %s\n",
						option);
				return 0;
			}
			protocol_string = words[i+1];
			protocol = protocol_string.c_str();
			i++;
		} else {
			printf("Unknown option '%s'\n", option);
			return 0;
		}
	}
	if (experiment.empty()) {
		experiment = protocol;
		experiment += "_";
		experiment += workload;
	}

	if (strcmp(protocol, "homa") == 0) {
		if (first_port == -1)
			first_port = 4000;
		for (int i = 0; i < server_ports; i++) {
			homa_server *server = new homa_server(first_port + i,
					i, inet_family, port_threads,
					experiment);
			homa_servers.push_back(server);
		}
	} else {
		if (first_port == -1)
			first_port = 5000;
		for (int i = 0; i < server_ports; i++) {
			tcp_server *server = new tcp_server(first_port + i,
					i, port_threads, experiment);
			tcp_servers.push_back(server);
		}
	}
	last_stats_time = 0;
	return 1;
}

/**
 * stop_cmd() - Parse the arguments for a "stop" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int stop_cmd(std::vector<string> &words)
{
	for (unsigned i = 1; i < words.size(); i++) {
		const char *option = words[i].c_str();
		if (strcmp(option, "clients") == 0) {
			for (client *client: clients)
				delete client;
			clients.clear();
		} else if (strcmp(option, "senders") == 0) {
			for (client *client: clients)
				client->stop_sender();
		} else if (strcmp(option, "servers") == 0) {
			log(NORMAL, "stop command deleting servers\n");
			for (homa_server *server: homa_servers)
				delete server;
			homa_servers.clear();
			for (tcp_server *server: tcp_servers)
				delete server;
			tcp_servers.clear();
			last_per_server_rpcs.clear();
			for (server_metrics *m: metrics)
				delete m;
			metrics.clear();
		} else {
			printf("Unknown option '%s'; must be clients, senders, "
				"or servers\n", option);
			return 0;
		}
	}
	return 1;
}

/**
 * tt_cmd() - Parse the arguments for a "tt" command and execute it.
 * @words:  Command arguments (including the command name as @words[0]).
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int tt_cmd(std::vector<string> &words)
{
	if (words.size() < 2) {
		printf("tt command requires an option\n");
		return 0;
	}
	const char *option = words[1].c_str();
	if (strcmp(option, "freeze") == 0) {
		tt("Freezing timetrace because of tt freeze command");
		time_trace::freeze();
	} else if (strcmp(option, "freezeboth") == 0) {
		tt("Freezing timetrace because of tt freezeboth command");
		time_trace::freeze();
		kfreeze();
	} else if (strcmp(option, "kfreeze") == 0) {
		kfreeze();
	} else if (strcmp(option, "print") == 0) {
		if (words.size() < 3) {
			printf("No file name provided for %s\n", option);
			return 0;
		}
		int error = time_trace::print_to_file(words[2].c_str());
		if (error) {
			printf("Couldn't open time trace file '%s': %s",
				words[2].c_str(), strerror(error));
			return 0;
		}
	} else {
		printf("Unknown option '%s'; must be freeze, freezeboth, "
				"kfreeze or print\n",
				option);
		return 0;
	}
	return 1;
}

/**
 * exec_words() - Given a command that has been parsed into words,
 * execute the command corresponding to the words.
 * @words:  Each entry represents one word of the command, like argc/argv.
 *
 * Return:  Nonzero means success, zero means there was an error.
 */
int exec_words(std::vector<string> &words)
{
	std::lock_guard<std::mutex> lock(cmd_lock);
	if (words.size() == 0)
		return 1;
	if (words[0].compare("client") == 0) {
		return client_cmd(words);
	} else if (words[0].compare("debug") == 0) {
		return debug_cmd(words);
	} else if (words[0].compare("dump_times") == 0) {
		return dump_times_cmd(words);
	} else if (words[0].compare("info") == 0) {
		return info_cmd(words);
	} else if (words[0].compare("log") == 0) {
		return log_cmd(words);
	} else if (words[0].compare("exit") == 0) {
		if (log_file != stdout)
			log(NORMAL, "cp_node exiting (exit command)\n");
		exit(0);
	} else if (words[0].compare("server") == 0) {
		return server_cmd(words);
	} else if (words[0].compare("stop") == 0) {
		return stop_cmd(words);
	} else if (words[0].compare("tt") == 0) {
		return tt_cmd(words);
	} else {
		printf("Unknown command '%s'\n", words[0].c_str());
		return 0;
	}
}

/**
 * exec_string() - Given a string, parse it into words and execute the
 * resulting command.
 * @cmd:  Command to execute.
 */
void exec_string(const char *cmd)
{
	const char *p = cmd;
	std::vector<string> words;

	if (log_file != stdout)
		log(NORMAL, "Command: %s\n", cmd);

	while (1) {
		int word_length = strcspn(p, " \t\n");
		if (word_length > 0)
			words.emplace_back(p, word_length);
		p += word_length;
		if (*p == 0)
			break;
		p++;
	}
	exec_words(words);
}

/**
 * error_handler() - This method is invoked after a terminal error such
 * as a segfault; it logs a backtrace and exits.
 * @signal    Signal number that caused this method to be invoked.
 * @info      Details about the cause of the signal; used to find the
 *            faulting address for segfaults.
 * @ucontext  CPU context at the time the signal occurred.
 */
void error_handler(int signal, siginfo_t* info, void* ucontext)
{
	ucontext_t* uc = static_cast<ucontext_t*>(ucontext);
	void* caller_address = reinterpret_cast<void*>(
			uc->uc_mcontext.gregs[REG_RIP]);

	log(NORMAL, "ERROR: Signal %d (%s) at address %p from %p\n",
			signal, strsignal(signal), info->si_addr,
			caller_address);
	tt("ERROR: Signal %d; freezing timetrace", signal);
	time_trace::freeze();

	const int max_frames = 128;
	void* return_addresses[max_frames];
	int frames = backtrace(return_addresses, max_frames);

	// Overwrite sigaction with caller's address.
	return_addresses[1] = caller_address;

	char** symbols = backtrace_symbols(return_addresses, frames);
	if (symbols == NULL) {
		/* If the malloc failed we might be able to get the backtrace out
		 * to stderr still.
		 */
		log(NORMAL, "backtrace_symbols failed; trying "
				"backtrace_symbols_fd\n");
		backtrace_symbols_fd(return_addresses, frames, 2);
		return;
	}

	log(NORMAL, "Backtrace:\n");
	for (int i = 1; i < frames; ++i)
		log(NORMAL, "%s\n", symbols[i]);
	log(NORMAL, "Writing time trace to error.tt\n");
	if (time_trace::print_to_file("error.tt"))
		log(NORMAL, "ERROR: couldn't write time trace %s\n",
				strerror(errno));
	fflush(log_file);
	while(1) {}

	/* Use abort, rather than exit, to dump core/trap in gdb. */
	abort();
}

int main(int argc, char** argv)
{
	time_trace::thread_buffer thread_buffer("main");
	setlinebuf(stdout);
	signal(SIGPIPE, SIG_IGN);
	struct rlimit limits;
	if (getrlimit(RLIMIT_NOFILE, &limits) != 0) {
		log(NORMAL, "FATAL: couldn't read file descriptor limits: "
				"%s\n", strerror(errno));
		fatal();
	}
	limits.rlim_cur = limits.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &limits) != 0) {
		log(NORMAL, "FATAL: couldn't increase file descriptor limit: "
				"%s\n", strerror(errno));
		fatal();
	}
	struct sigaction action;
	action.sa_sigaction = error_handler;
	action.sa_flags = SA_RESTART | SA_SIGINFO;
	if (sigaction(SIGSEGV, &action, NULL) != 0)
		log(VERBOSE, "Couldn't set signal handler for SIGSEGV; "
				"continuing anyway\n");

	if ((argc >= 2) && (strcmp(argv[1], "--help") == 0)) {
		print_help(argv[0]);
		exit(0);
	}

	if (argc > 1) {
		std::vector<string> words;
		for (int i = 1; i < argc; i++)
			words.emplace_back(argv[i]);
		if (!exec_words(words))
			fatal();

		/* Instead of going interactive, just print stats.
		 * every second.
		 */
		log_stats();
	}

//	cpu_set_t cores;
//	CPU_ZERO(&cores);
//	for (int i = 2; i < 18; i++)
//		CPU_SET(i, &cores);
//	if (sched_setaffinity(0, sizeof(cores), &cores) != 0)
//		log(NORMAL, "ERROR: couldn't set core affinity: %s\n",
//				strerror(errno));

	std::thread logger(log_stats);
	while (1) {
		string line;

		printf("%% ");
		fflush(stdout);
		if (!std::getline(std::cin, line)) {
			if (log_file != stdout)
				log(NORMAL, "cp_node exiting (EOF on stdin)\n");
			exit(0);
		}
		exec_string(line.c_str());
	}
}
