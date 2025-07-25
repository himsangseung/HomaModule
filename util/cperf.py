#!/usr/bin/python3

# Copyright (c) 2020-2023 Homa Developers
# SPDX-License-Identifier: BSD-1-Clause

# This file contains library functions used to run cluster performance
# tests for the Linux kernel implementation of Homa.

import argparse
from collections import defaultdict
import copy
import datetime
import glob
import math
import matplotlib
import matplotlib.pyplot as plt
import numpy as np
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import traceback

# Avoid Type 3 fonts (conferences don't tend to like them).
matplotlib.rcParams['pdf.fonttype'] = 42
matplotlib.rcParams['ps.fonttype'] = 42

if platform.system() != "Windows":
    import fcntl

# If a server's id appears as a key in this dictionary, it means we
# have started cp_node running on that node. The value of each entry is
# a Popen object that can be used to communicate with the node.
active_nodes = {}

# If a server's id appears as a key in this dictionary, it means we
# have started homa_prio running on that node. The value of each entry is
# a Popen object for the homa_prio instance; if this is terminated, then
# the homa_prio process will end.
homa_prios = {}

# The range of nodes currently running cp_node servers.
server_nodes = range(0,0)

# Directory containing log files.
log_dir = ''

# Open file (in the log directory) where log messages should be written.
log_file = 0

# True means use new slowdown calculation, where denominator is calculated
# using best-case Homa unloaded RTT plus link bandwidth; False means use
# original calculation where the denominator is Homa P50 unloaded latency.
old_slowdown = False

# Indicates whether we should generate additional log messages for debugging
verbose = False

# The --delete-rtts command-line option.
delete_rtts = False

# The CloudLab node type for this node (e.g. xl170)
node_type = None

# Value of the "--stripped" option.
stripped = False

# Speed of host uplinks.
link_mbps = None

# Defaults for command-line options; assumes that servers and clients
# share nodes.
default_defaults = {
    'gbps':                0.0,
    # Note: very large numbers for client_max hurt Homa throughput with
    # unlimited load (throttle queue inserts take a long time).
    'client_max':          200,
    'client_ports':        3,
    'log_dir':             'logs/' + time.strftime('%Y%m%d%H%M%S'),
    'mtu':                 0,
    'no_trunc':            '',
    'protocol':            'homa',
    'port_receivers':      3,
    'port_threads':        3,
    'seconds':             30,
    'server_ports':        3,
    'tcp_client_ports':    4,
    'tcp_port_receivers':  1,
    'tcp_server_ports':    8,
    'tcp_port_threads':    1,
    'unloaded':            0,
    'unsched':             0,
    'unsched_boost':       0.0,
    'workload':            ''
}

# Keys are experiment names, and each value is the digested data for that
# experiment.  The digest is itself a dictionary containing some or all of
# the following keys:
# rtts:            A dictionary with message lengths as keys; each value is
#                  a list of the RTTs (in usec) for all messages of that length.
# total_messages:  Total number of samples in rtts.
# lengths:         Sorted list of message lengths, corresponding to buckets
#                  chosen for plotting
# cum_frac:        Cumulative fraction of all messages corresponding to each length
# counts:          Number of RTTs represented by each bucket
# p50:             List of 50th percentile rtts corresponding to each length
# p99:             List of 99th percentile rtts corresponding to each length
# p999:            List of 999th percentile rtts corresponding to each length
# slow_50:         List of 50th percentile slowdowns corresponding to each length
# slow_99:         List of 99th percentile slowdowns corresponding to each length
# slow_999:        List of 999th percentile slowdowns corresponding to each length
# avg_slowdown:    Average slowdown across all messages of all sizes
digests = {}

# A dictionary where keys are message lengths, and each value is the median
# unloaded RTT (usecs) for messages of that length.
unloaded_p50 = {}

# Minimum RTT for any measurement in the unloaded dataset
min_rtt = 1e20;

# Keys are filenames, and each value is a dictionary containing data read
# from that file. Within that dictionary, each key is the name of a column
# within the file, and the value is a list of numbers read from the given
# column of the given file.
data_from_files = {}

# Time when this benchmark was run.
date_time = str(datetime.datetime.now())

# Standard colors for plotting
tcp_color =      '#00B000'
tcp_color2 =     '#5BD15B'
tcp_color3 =     '#96E296'
homa_color =     '#1759BB'
homa_color2 =    '#6099EE'
homa_color3 =    '#A6C6F6'
dctcp_color =    '#7A4412'
dctcp_color2 =   '#CB701D'
dctcp_color3 =   '#EAA668'
unloaded_color = '#d62728'

# Default bandwidths to use when running all of the workloads.
load_info = [["w1", 1.4], ["w2", 3.2], ["w3", 14], ["w4", 20], ["w5", 20]]

# PyPlot color circle colors:
pyplot_colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd',
        '#8c564b', '#e377c2', '#7f7f7f', '#bcbd22', '#17becf']

def boolean(s):
    """
    Used as a "type" in argparse specs; accepts Boolean-looking things.
    """
    map = {'true': True, 'yes': True, 'ok': True, "1": True, 'y': True,
        't': True, 'false': False, 'no': False, '0': False, 'f': False,
        'n': False}
    lc = s.lower()
    if lc not in map:
        raise ValueError("Expected boolean value, got %s" % (s))
    return map[lc]

def log(message):
    """
    Write the a log message both to stdout and to the cperf log file.

    message:  The log message to write; a newline will be appended.
    """
    global log_file
    print(message)
    log_file.write("%.9f " % (time.time()))
    log_file.write(message)
    log_file.write("\n")

def vlog(message):
    """
    Log a message, like log, but if verbose logging isn't enabled, then
    log only to the cperf log file, not to stdout.

    message:  The log message to write; a newline will be appended.
    """
    global log_file, verbose
    if verbose:
        print(message)
    log_file.write("%.9f " % (time.time()))
    log_file.write(message)
    log_file.write("\n")

def get_parser(description, usage, defaults = {}):
    """
    Returns an ArgumentParser for options that are commonly used in
    performance tests.

    description:    A string describing the overall functionality of this
                    particular performance test
    usage:          A command synopsis (passed as usage to ArgumentParser)
    defaults:       A dictionary whose keys are option names and whose values
                    are defaults; used to modify the defaults for some of the
                    options (there is a default default for each option).
    """
    for key in default_defaults:
        if not key in defaults:
            defaults[key] = default_defaults[key]
    parser = argparse.ArgumentParser(description=description + ' The options '
            'below may include some that are not used by this particular '
            'benchmark', usage=usage, add_help=False,
            conflict_handler='resolve')
    parser.add_argument('-b', '--gbps', type=float, dest='gbps',
            metavar='B', default=defaults['gbps'],
            help='Generate a total of B Gbits/sec of bandwidth on the most '
            'heavily loaded machines; 0 means run as fast as possible '
            '(default: %.2f)' % (defaults['gbps']))
    parser.add_argument('--client-max', type=int, dest='client_max',
            metavar='count', default=defaults['client_max'],
            help='Maximum number of Homa requests each client machine can have '
            'outstanding at a time (divided evenly among the Homa ports) '
            '(default: %d)' % (defaults['client_max']))
    parser.add_argument('--client-ports', type=int, dest='client_ports',
            metavar='count', default=defaults['client_ports'],
            help='Number of ports on which each client should issue requests '
            '(default: %d)' % (defaults['client_ports']))
    parser.add_argument('--cperf-log', dest='cperf_log',
            metavar='F', default='cperf.log',
            help='Name to use for the cperf log file (default: cperf.log)')
    parser.add_argument('-d', '--debug', dest='debug', action='store_true',
            help='Pause after starting servers to enable debugging setup')
    parser.add_argument('--delete-rtts', dest='delete_rtts', action='store_true',
            help='Delete .rtt files after reading, in order to save disk space')
    parser.add_argument('-h', '--help', action='help',
            help='Show this help message and exit')
    parser.add_argument('-6', '--ipv6', dest='ipv6', action='store_const',
            const='--ipv6', default='',
            help='Use IPv6 for communication (default: use IPv4)')
    parser.add_argument('-l', '--log-dir', dest='log_dir',
            metavar='D', default=defaults['log_dir'],
            help='Directory to use for logs and metrics')
    parser.add_argument('--mtu', type=int, dest='mtu',
            required=False, metavar='M', default=defaults['mtu'],
            help='Maximum allowable packet size (0 means use existing, '
            'default: %d)' % (defaults['mtu']))
    parser.add_argument('-n', '--nodes', type=int, dest='num_nodes',
            required=True, metavar='N',
            help='Total number of nodes to use in the cluster')
    parser.add_argument('--no-homa-prio', dest='no_homa_prio',
            action='store_true', default=False,
            help='Don\'t run homa_prio on nodes to adjust unscheduled cutoffs')
    parser.add_argument('--old-slowdown', dest='old_slowdown',
            action='store_true', default=False,
            help='Compute slowdowns using the approach of the Homa ATC '
            'paper (default: use 15 usec RTT and 100%% link throughput as '
            'reference)')
    parser.add_argument('--plot-only', dest='plot_only', action='store_true',
            help='Don\'t run experiments; generate plot(s) with existing data')
    parser.add_argument('--port-receivers', type=int, dest='port_receivers',
            metavar='count', default=defaults['port_receivers'],
            help='Number of threads listening for responses on each Homa '
            'client port (default: %d)'% (defaults['port_receivers']))
    parser.add_argument('--port-threads', type=int, dest='port_threads',
            metavar='count', default=defaults['port_threads'],
            help='Number of threads listening on each Homa server port '
            '(default: %d)'% (defaults['port_threads']))
    parser.add_argument('-p', '--protocol', dest='protocol',
            choices=['homa', 'tcp', 'dctcp'], default=defaults['protocol'],
            help='Transport protocol to use (default: %s)'
            % (defaults['protocol']))
    parser.add_argument('-s', '--seconds', type=int, dest='seconds',
            metavar='S', default=defaults['seconds'],
            help='Run each experiment for S seconds (default: %.1f)'
            % (defaults['seconds']))
    parser.add_argument('--server-ports', type=int, dest='server_ports',
            metavar='count', default=defaults['server_ports'],
            help='Number of ports on which each server should listen '
            '(default: %d)'% (defaults['server_ports']))
    parser.add_argument('--set-ids', dest='set_ids', type=boolean,
            default=True, metavar="T/F", help="Boolean value: if true, the "
            "next_id sysctl parameter will be set on each node in order to "
            "avoid conflicting RPC ids on different nodes (default: true)")
    parser.add_argument('--skip', dest='skip',
            metavar='nodes',
            help='List of node numbers not to use in the experiment; can '
            ' contain ranges, such as "3,5-8,12"')
    parser.add_argument('--stripped', dest='stripped', type=boolean,
            default=False, metavar="T/F", help='Boolean value: true means '
            'Homa has been stripped for upstreaming, which means some '
            'facilities are not available (default: false)')
    parser.add_argument('--tcp-client-max', dest='tcp_client_max', type=int,
            metavar='count', default=0, help="Maximum number of TCP requests "
            "that can be outstanding from a client node at once (divided evenly "
            "among the TCP ports); if zero, the "
            "--client-max option is used for TCP as well (i.e. each protocol "
            "can have that many outstanding requests) (default: 0)")
    parser.add_argument('--tcp-client-ports', type=int, dest='tcp_client_ports',
            metavar='count', default=defaults['tcp_client_ports'],
            help='Number of ports on which each TCP client should issue requests '
            '(default: %d)'% (defaults['tcp_client_ports']))
    parser.add_argument('--tcp-port-receivers', type=int,
            dest='tcp_port_receivers', metavar='count',
            default=defaults['tcp_port_receivers'],
            help='Number of threads listening for responses on each TCP client '
            'port (default: %d)'% (defaults['tcp_port_receivers']))
    parser.add_argument('--tcp-port-threads', type=int, dest='tcp_port_threads',
            metavar='count', default=defaults['tcp_port_threads'],
            help='Number of threads listening on each TCP server port '
            '(default: %d)'% (defaults['tcp_port_threads']))
    parser.add_argument('--tcp-server-ports', type=int, dest='tcp_server_ports',
            metavar='count', default=defaults['tcp_server_ports'],
            help='Number of ports on which TCP servers should listen '
            '(default: %d)'% (defaults['tcp_server_ports']))
    parser.add_argument('--tt-freeze', dest='tt_freeze', type=boolean,
            default=True, metavar="T/F", help="Boolean value: if true, "
            "timetraces will be frozen on all nodes at the end of the "
            "Homa benchmark run (default: true)")
    parser.add_argument('--unsched', type=int, dest='unsched',
            metavar='count', default=defaults['unsched'],
            help='If nonzero, homa_prio will always use this number of '
            'unscheduled priorities, rather than computing from workload'
            '(default: %d)'% (defaults['unsched']))
    parser.add_argument('--unsched-boost', type=float, dest='unsched_boost',
            metavar='float', default=defaults['unsched'],
            help='Increase the number of unscheduled priorities that homa_prio '
            'assigns by this (possibly fractional) amount (default: %.2f)'
            % (defaults['unsched_boost']))
    parser.add_argument('-v', '--verbose', dest='verbose', action='store_true',
            help='Enable verbose output in node logs')
    parser.add_argument('-w', '--workload', dest='workload',
            metavar='W', default=defaults['workload'],
            help='Workload to use for benchmark: w1-w5 or number, empty '
            'means try each of w1-w5 (default: %s)'
            % (defaults['workload']))
    return parser

def init(options):
    """
    Initialize various global state, such as the log file.
    """
    global old_slowdown, log_dir, log_file, verbose, delete_rtts, link_mbps
    global stripped
    log_dir = options.log_dir
    old_slowdown = options.old_slowdown
    if not options.plot_only:
        if os.path.exists(log_dir):
            shutil.rmtree(log_dir)
        os.makedirs(log_dir)
        os.makedirs(log_dir + "/reports")
    log_file = open("%s/reports/%s" % (log_dir, options.cperf_log), "a")
    verbose = options.verbose
    vlog("cperf starting at %s" % (date_time))
    s = ""

    # Figure out which nodes to use for the experiment
    skips = {}
    if options.skip:
        for spec in options.skip.split(","):
            nodes = spec.split("-")
            if len(nodes) == 1:
                skips[int(spec)] = 1
            elif len(nodes) == 2:
                for i in range(int(nodes[0]), int(nodes[1])+1):
                    skips[i] = 1
            else:
                raise Exception("Bad skip range '%s': must be either id "
                       "or id1-id2" % (spec))
    nodes = []
    id = 0
    while len(nodes) != options.num_nodes:
        if not id in skips:
            nodes.append(id)
        id += 1
    options.nodes = nodes
    options.servers = options.nodes
    options.clients = options.nodes

    # Log configuration information, including options here as well
    # as Homa's configuration parameters.
    opts = vars(options)
    for name in sorted(opts.keys()):
        if len(s) != 0:
            s += ", "
        s += ("--%s: %s" % (name, str(opts[name])))
    vlog("Options: %s" % (s))
    vlog("Homa configuration (node%d):" % (options.nodes[0]))
    result = subprocess.run(['ssh', 'node%d' % (options.nodes[0]),
            'sysctl', '-a'], capture_output=True, encoding="utf-8")
    if (result.returncode != 0):
        log("sysctl -a on node%d exited with status %d:" %
                (options.nodes[0], result.returncode))
        log(result.stderr.rstrip())
    for line in result.stdout.splitlines():
        match = re.match('.*net.homa.([^ ]+) = (.*)', line)
        if match:
            name = match.group(1)
            value = match.group(2)
            vlog("  %-20s %s" % (name, value))
            if name == 'link_mbps':
                link_mbps = float(value)
    if link_mbps == None:
        link_mbps = 25000

    if options.mtu != 0:
        log("Setting MTU to %d" % (options.mtu))
        do_ssh(["config", "mtu", str(options.mtu)], options.nodes)

    if options.delete_rtts:
        delete_rtts = True

def wait_output(string, nodes, cmd, time_limit=10.0):
    """
    This method waits until a particular string has appeared on the stdout of
    each of the nodes in the list given by nodes. If a long time goes by without
    the string appearing, an exception is thrown.
    string:      The value to wait for
    nodes:       List of node ids from which output is expected
    cmd:         Used in error messages to indicate the command that failed
    time_limit:  An error will be generated if this much time goes by without
                 the desired string appearing
    """
    global active_nodes
    outputs = []
    printed = False
    bad_node = -1

    for id in nodes:
        while len(outputs) <= id:
            outputs.append("")
    start_time = time.time()
    while True:
        if time.time() > (start_time + time_limit):
            raise Exception("timeout (%.1fs) exceeded for command '%s' on node%d"
                    % (time_limit, cmd, bad_node))
        for id in nodes:
            data = active_nodes[id].stdout.read(1000)
            if data != None:
                print_data = data
                if print_data.endswith(string):
                    print_data = print_data[:(len(data) - len(string))]
                if print_data != "":
                    log("extra output from node%d: '%s'" % (id, print_data))
                outputs[id] += data
        bad_node = -1
        for id in nodes:
            if not string in outputs[id]:
                bad_node = id
                break
        if bad_node < 0:
            return
        if (time.time() > (start_time + time_limit)) and not printed:
            log("expected output from node%d not yet received "
            "after command '%s': expecting '%s', got '%s'"
            % (bad_node, cmd, string, outputs[bad_node]))
            printed = True
        time.sleep(0.1)
    raise Exception("bad output from node%d after command '%s': "
            "expected '%s', got '%s'"
            % (bad_node, cmd, string, outputs[bad_node]))

def start_nodes(ids, options):
    """
    Start up cp_node on a group of nodes. Also starts homa_prio on the
    nodes, if protocol is "homa".

    ids:      List of node ids on which to start cp_node, if it isn't already
              running
    options:  Command-line options that may affect node configuration
    """
    global active_nodes, homa_prios, verbose
    started = []
    for id in ids:
        if not id in active_nodes:
            vlog("Starting cp_node on node%d" % (id))
            node = subprocess.Popen(["ssh", "-o", "StrictHostKeyChecking=no",
                    "node%d" % (id), "cp_node"], encoding="utf-8",
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT)
            fl = fcntl.fcntl(node.stdin, fcntl.F_GETFL)
            fcntl.fcntl(node.stdin, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            fl = fcntl.fcntl(node.stdout, fcntl.F_GETFL)
            fcntl.fcntl(node.stdout, fcntl.F_SETFL, fl | os.O_NONBLOCK)
            active_nodes[id] = node
            started.append(id)
        if options.protocol == "homa":
            if options.set_ids:
                set_sysctl_parameter(".net.homa.next_id",
                        str(100000000*(id+1)), [id])
            if not options.no_homa_prio:
                f = open("%s/homa_prio-%d.log" % (log_dir,id), "w")
                homa_prios[id] = subprocess.Popen(["ssh", "-o",
                        "StrictHostKeyChecking=no", "node%d" % (id), "sudo",
                        "bin/homa_prio", "--interval", "500", "--unsched",
                        str(options.unsched), "--unsched-boost",
                        str(options.unsched_boost)], encoding="utf-8",
                        stdout=f, stderr=subprocess.STDOUT)
                f.close
    wait_output("% ", started, "ssh")
    log_level = "verbose" if verbose else "normal"
    command = "log --file node.log --level %s" % (log_level)
    for id in started:
        active_nodes[id].stdin.write(command + "\n")
        active_nodes[id].stdin.flush()
    wait_output("% ", started, command)

def stop_nodes():
    """
    Exit all of the nodes that are currently active.
    """
    global active_nodes, server_nodes
    for id, popen in homa_prios.items():
        do_subprocess(["ssh", "-o", "StrictHostKeyChecking=no",
                "node%d" % id, "sudo", "pkill", "homa_prio"])
        try:
            popen.wait(5.0)
        except subprocess.TimeoutExpired:
            log("Timeout killing homa_prio on node%d" % (id))
    for id, node in active_nodes.items():
        node.stdin.write("exit\n")
        try:
            node.stdin.flush()
        except BrokenPipeError:
            log("Broken pipe to node%d" % (id))
    for node in active_nodes.values():
        node.wait(5.0)
    for id in active_nodes:
        do_subprocess(["rsync", "-rtvq", "node%d:node.log" % (id),
                "%s/node%d.log" % (log_dir, id)])
    active_nodes.clear()
    server_nodes = range(0,0)

def do_cmd(command, ids, ids2 = []):
    """
    Execute a cp_node command on a given group of nodes.

    command:    A command to execute on each node
    ids:        List of node ids on which to run the command
    ids2:       An optional additional list of node ids on which to run the
                command; if a node is present in both r and r2, the
                command will only be performed once
    """
    global active_nodes
    nodes = []
    for id in ids:
        nodes.append(id)
    for id in ids2:
        if id not in ids:
            nodes.append(id)
    for id in nodes:
        vlog("Command for node%d: %s" % (id, command))
        active_nodes[id].stdin.write(command + "\n")
        try:
            active_nodes[id].stdin.flush()
        except BrokenPipeError:
            log("Broken pipe to node%d" % (id))
    wait_output("% ", nodes, command)

def do_ssh(command, nodes):
    """
    Use ssh to execute a particular shell command on a group of nodes.

    command:  command to execute on each node (a list of argument words)
    nodes:    specifies ids of the nodes on which to execute the command:
              should be a range, list, or other object that supports "in"
    """
    vlog("ssh command on nodes %s: %s" % (str(nodes), " ".join(command)))
    for id in nodes:
        do_subprocess(["ssh", "node%d" % id] + command)

def get_sysctl_parameter(name):
    """
    Retrieve the value of a particular system parameter using sysctl on
    the current host, and return the value as a string.

    name:      name of the desired configuration parameter
    """
    output = do_subprocess(["sysctl", name])
    match = re.match('.*= (.*)', output)
    if not match:
         raise Exception("Couldn't parse sysctl output: %s" % output)
    return match.group(1)

def set_sysctl_parameter(name, value, nodes):
    """
    Modify the value of a system parameter on a group of nodes.

    name:     name of the sysctl configuration parameter to modify
    value:    desired value for the parameter
    nodes:    specifies ids of the nodes on which to execute the command:
              should be a range, list, or other object that supports "in"
    """
    global stripped
    if stripped:
        vlog("Skipping set of Homa %s parameter to %s on nodes %s (Homa is stripped)"
                % (name, value, str(nodes)))
        return
    vlog("Setting Homa parameter %s to %s on nodes %s" % (name, value,
            str(nodes)))
    for id in nodes:
        do_subprocess(["ssh", "node%d" % id, "sudo", "sysctl",
                "%s=%s" % (name, value)])

def get_node_type():
    """
    Returns the node type for this machine.
    """

    global node_type
    if node_type:
        return node_type
    f = open("/var/emulab/boot/nodetype")
    node_type = f.read().strip()
    f.close()
    return node_type

def do_subprocess(words):
    """
    Invoke subprocess.run to run args in a child process and then
    check the results. Log any errors that are detected. Returns
    stdout from the child (with trailing newlines removed).

    words:   List of words for the command to run.
    """
    result = subprocess.run(words, capture_output=True, encoding="utf-8")
    if (result.returncode != 0):
        log("Command %s exited with status %d" % (words, result.returncode))
    if (result.stderr != ""):
        log("Error output from %s: %s" % (words, result.stderr.rstrip()))
    return result.stdout.rstrip()

def start_servers(exp, ids, options):
    """
    Starts cp_node servers running on a group of nodes

    exp:     Name of experiment these servers will be part of
    ids:     A list of node ids on which to start cp_node servers
    options: A namespace that must contain at least the following
             keys, which will be used to configure the servers:
                 server_ports
                 port_threads
                 protocol
    """
    global server_nodes, stripped
    stripped = options.stripped
    log("Starting servers for %s experiment on nodes %s" % (exp, ids))
    if len(server_nodes) > 0:
        do_cmd("stop servers", server_nodes)
        server_nodes = []
    start_nodes(ids, options)
    if options.protocol == "homa":
        do_cmd("server --ports %d --port-threads %d --protocol %s --exp %s %s"
               % (options.server_ports, options.port_threads,
                options.protocol, exp, options.ipv6), ids)
    else:
        do_cmd("server --ports %d --port-threads %d --protocol %s --exp %s %s"
               % (options.tcp_server_ports, options.tcp_port_threads,
                options.protocol, exp, options.ipv6), ids)
    server_nodes = ids
    if options.debug:
        input("Pausing for debug setup, type <Enter> to continue: ")

def run_experiment(name, clients, options):
    """
    Starts cp_node clients running on a group of nodes, lets the clients run
    for an amount of time given by options.seconds, and gathers statistics.

    name:     Identifier for this experiment, which is used in the names
              of files created in the log directory.
    clients:  List of node numbers on which to run clients
    options:  A namespace that must contain at least the following attributes,
              which control the experiment:
                  client_max
                  client_ports
                  gbps
                  port_receivers
                  protocol
                  seconds
                  server_ports
                  servers
                  tcp_client_ports
                  tcp_server_ports
                  workload
    """

    global active_nodes, stripped
    exp_nodes = list(set(options.servers + list(clients)))
    start_nodes(clients, options)
    nodes = []
    log("Starting clients for %s experiment on nodes %s" % (name, clients))
    for id in clients:
        if options.protocol == "homa":
            command = "client --ports %d --port-receivers %d --server-ports %d " \
                    "--workload %s --servers %s --gbps %.3f --client-max %d " \
                    "--protocol %s --id %d --exp %s %s" % (
                    options.client_ports,
                    options.port_receivers,
                    options.server_ports,
                    options.workload,
                    ",".join([str(x) for x in options.servers]),
                    options.gbps,
                    options.client_max,
                    options.protocol,
                    id,
                    name,
                    options.ipv6)
            if "unloaded" in options:
                command += " --unloaded %d" % (options.unloaded)
        else:
            if "no_trunc" in options:
                trunc = '--no-trunc'
            else:
                trunc = ''
            client_max = options.tcp_client_max
            if not client_max:
                client_max = options.client_max
            command = "client --ports %d --port-receivers %d --server-ports %d " \
                    "--workload %s --servers %s --gbps %.3f %s --client-max %d " \
                    "--protocol %s --id %d --exp %s %s" % (
                    options.tcp_client_ports,
                    options.tcp_port_receivers,
                    options.tcp_server_ports,
                    options.workload,
                    ",".join([str(x) for x in options.servers]),
                    options.gbps,
                    trunc,
                    client_max,
                    options.protocol,
                    id,
                    name,
                    options.ipv6)
        active_nodes[id].stdin.write(command + "\n")
        try:
            active_nodes[id].stdin.flush()
        except BrokenPipeError:
            log("Broken pipe to node%d" % (id))
        nodes.append(id)
        vlog("Command for node%d: %s" % (id, command))
    wait_output("% ", nodes, command, 40.0)
    if not "unloaded" in options:
        if options.protocol == "homa":
            # Wait a bit so that homa_prio can set priorities appropriately
            time.sleep(2)
            if stripped:
                vlog("Skipping initial read of metrics (Homa is stripped)")
            else:
                vlog("Recording initial metrics")
                for id in exp_nodes:
                    do_subprocess(["ssh", "node%d" % (id), "metrics.py"])
        if not "no_rtt_files" in options:
            do_cmd("dump_times /dev/null %s" % (name), clients)
        do_cmd("log Starting measurements for %s experiment" % (name),
                server_nodes, clients)
        log("Starting measurements")
        debug_delay = 0
        if debug_delay > 0:
            time.sleep(debug_delay)
        if False and "dctcp" in name:
            log("Setting debug info")
            do_cmd("debug 2000 3000", clients)
            log("Finished setting debug info")
        time.sleep(options.seconds - debug_delay)
        if options.protocol == "homa" and options.tt_freeze:
            log("Freezing timetraces via node%d" % nodes[0])
            set_sysctl_parameter(".net.homa.action", "7", nodes[0:1])
        do_cmd("log Ending measurements for %s experiment" % (name),
               server_nodes, clients)
    log("Retrieving data for %s experiment" % (name))
    if not "no_rtt_files" in options:
        do_cmd("dump_times rtts %s" % (name), clients)
    if (options.protocol == "homa") and not "unloaded" in options:
        if stripped:
                vlog("Skipping final read of metrics (Homa is stripped)")
        else:
            vlog("Recording final metrics from nodes %s" % (exp_nodes))
            for id in exp_nodes:
                f = open("%s/%s-%d.metrics" % (options.log_dir, name, id), 'w')
                subprocess.run(["ssh", "node%d" % (id), "metrics.py"], stdout=f)
                f.close()
            shutil.copyfile("%s/%s-%d.metrics" %
                    (options.log_dir, name, options.servers[0]),
                    "%s/reports/%s-%d.metrics" %
                    (options.log_dir, name, options.servers[0]))
            shutil.copyfile("%s/%s-%d.metrics" %
                    (options.log_dir, name, clients[0]),
                    "%s/reports/%s-%d.metrics" %
                    (options.log_dir, name, clients[0]))
    do_cmd("stop senders", clients)
    if False and "dctcp" in name:
        do_cmd("tt print cp.tt", clients)
    # do_ssh(["sudo", "sysctl", ".net.homa.log_topic=3"], clients)
    # do_ssh(["sudo", "sysctl", ".net.homa.log_topic=2"], clients)
    # do_ssh(["sudo", "sysctl", ".net.homa.log_topic=1"], clients)
    do_cmd("stop clients", clients)
    if not "no_rtt_files" in options:
        for id in clients:
            do_subprocess(["rsync", "-rtvq", "node%d:rtts" % (id),
                    "%s/%s-%d.rtts" % (options.log_dir, name, id)])

def run_experiments(*args):
    """
    Run multiple experiments simultaneously and collect statistics.

    args:    Each argument is a namespace describing an experiment to
             run. The namespace must contain the following values:
             name:       The name of the experiment; used to create files
                         with the experiment's results.
             clients:    List of node numbers on which to run clients for the
                         experiment.
             servers:    List of node numbers on which to run servers for the
                         experiment (if the same server is in multiple
                         experiments, the parameters from the first experiment
                         are used to start the server).
             protocol:   tcp or homa
             gbps
             seconds
             workload

             For Homa experiments the following values must be present:
             client_max
             client_ports
             port_receivers
             server_ports
             port_threads

             For TCP experiments the following values must be present:
             tcp_client_max (or client_max)
             tcp_client_ports
             tcp_server_ports

             There may be additional optional values that used if present.
    """

    global active_nodes, stripped

    homa_nodes = []
    homa_clients = []
    homa_servers= []
    tcp_nodes = []
    for exp in args:
        if exp.protocol == "homa":
            homa_clients.extend(exp.clients)
            homa_nodes.extend(exp.clients)
            homa_servers.extend(exp.servers)
            homa_nodes.extend(exp.servers)
        elif exp.protocol == "tcp":
            tcp_nodes.extend(exp.clients)
            tcp_nodes.extend(exp.servers)
    homa_clients = sorted(list(set(homa_clients)))
    homa_servers = sorted(list(set(homa_servers)))
    homa_nodes = sorted(list(set(homa_nodes)))
    tcp_nodes = sorted(list(set(tcp_nodes)))
    all_nodes = sorted(list(set(homa_nodes + tcp_nodes)))

    # Start servers for all experiments
    stop_nodes()
    for exp in args:
        if exp.servers:
            log("Starting servers for %s experiment on nodes %s" % (exp.name,
                    exp.servers))
            start_nodes(exp.servers, exp)
            if exp.protocol == "homa":
                do_cmd("server --ports %d --port-threads %d --protocol homa "
                       "--exp %s %s"
                        % (exp.server_ports, exp.port_threads,
                        exp.name, exp.ipv6), exp.servers)
            else:
                do_cmd("server --ports %d --port-threads %d --protocol tcp "
                       "--exp %s %s"
                        % (exp.tcp_server_ports, exp.tcp_port_threads,
                        exp.name, exp.ipv6), exp.servers)

    # Start clients for all experiments
    for exp in args:
        log("Starting clients for %s experiment on nodes %s" % (exp.name,
                exp.clients))
        start_nodes(exp.clients, exp)
        for id in exp.clients:
            if exp.protocol == "homa":
                command = "client --ports %d --port-receivers %d --server-ports %d " \
                        "--workload %s --servers %s --gbps %.3f --client-max %d " \
                        "--protocol homa --id %d --exp %s %s" % (
                        exp.client_ports,
                        exp.port_receivers,
                        exp.server_ports,
                        exp.workload,
                        ",".join([str(x) for x in exp.servers]),
                        exp.gbps,
                        exp.client_max,
                        id,
                        exp.name,
                        exp.ipv6)
            else:
                client_max = exp.tcp_client_max
                if not client_max:
                    client_max = exp.client_max
                command = "client --ports %d --port-receivers %d --server-ports %d " \
                        "--workload %s --servers %s --gbps %.3f --client-max %d " \
                        "--protocol tcp --id %d --exp %s %s" % (
                        exp.tcp_client_ports,
                        exp.tcp_port_receivers,
                        exp.tcp_server_ports,
                        exp.workload,
                        ",".join([str(x) for x in exp.servers]),
                        exp.gbps,
                        client_max,
                        id,
                        exp.name,
                        exp.ipv6)
            active_nodes[id].stdin.write(command + "\n")
            try:
                active_nodes[id].stdin.flush()
            except BrokenPipeError:
                log("Broken pipe to node%d while starting %s client" % (id,
                        exp.protocol))
            vlog("Command for node%d: %s" % (id, command))
        wait_output("% ", exp.clients, command, 40.0)
    if homa_clients:
        # Wait a bit so that homa_prio can set priorities appropriately
        time.sleep(2)
    if homa_nodes:
        if stripped:
            vlog("Skipping metrics initialization (Homa is stripped)")
        else:
            vlog("Initializing metrics")
            do_ssh(["metrics.py > /dev/null"], homa_nodes)
    do_cmd("dump_times /dev/null", all_nodes)
    do_cmd("log Starting measurements", all_nodes)
    log("Starting measurements")

    time.sleep(exp.seconds)

    # Collect results
    if homa_nodes and exp.tt_freeze:
        log("Freezing timetraces via node%d" % all_nodes[0])
        set_sysctl_parameter(".net.homa.action", "7", all_nodes[0:1])
    do_cmd("log Ending measurements", all_nodes)
    log("Retrieving data")
    for exp in args:
        do_cmd("dump_times %s.rtts %s" % (exp.name, exp.name), exp.clients)
    if homa_nodes:
        if stripped:
                vlog("Skipping final read of metrics (Homa is stripped)")
        else:
            vlog("Recording final metrics from nodes %s" % (homa_nodes))
            for id in homa_nodes:
                f = open("%s/node%d.metrics" % (exp.log_dir, id), 'w')
                subprocess.run(["ssh", "node%d" % (id), "metrics.py"], stdout=f)
                f.close()
            shutil.copyfile("%s/node%d.metrics" %
                    (exp.log_dir, homa_clients[0]),
                    "%s/reports/node%d.metrics" %
                    (exp.log_dir, homa_clients[0]))
            shutil.copyfile("%s/node%d.metrics" %
                    (exp.log_dir, homa_servers[0]),
                    "%s/reports/node%d.metrics" %
                    (exp.log_dir, homa_servers[0]))
    do_cmd("stop senders", all_nodes)
    do_cmd("stop clients", all_nodes)
    for exp in args:
        for id in exp.clients:
            do_subprocess(["rsync", "-rtvq", "node%d:%s.rtts" % (id, exp.name),
                    "%s/%s-%d.rtts" % (exp.log_dir, exp.name, id)])

def scan_log(file, node, experiments):
    """
    Read a log file and extract various useful information, such as fatal
    error messages or interesting statistics.

    file:         Name of the log file to read
    node:         Name of the node that generated the log, such as "node1".
    experiments:  Info from the given log file is added to this structure
                  * At the top level it is a dictionary indexed by experiment
                    name, where
                  * Each value is dictionary indexed by node name, where
                  * Each value is a dictionary with keys such as client_kops,
                    client_gbps, client_latency, server_kops, or server_Mbps,
                    each of which is
                  * A list of values measured at regular intervals for that node
    """
    exited = False
    experiment = ""
    node_data = None
    active = False
    timeouts = 0

    for line in open(file):
        if "FATAL:" in line:
            log("%s: %s" % (file, line[:-1]))
            exited = True
        if "ERROR:" in line:
            if "Homa RPC timed out" in line:
                timeouts += 1
                if timeouts > 1:
                    continue
            log("%s: %s" % (file, line[:-1]))
            continue
        if "cp_node exiting" in line:
            exited = True

        match = re.match('.*Starting measurements', line)
        if match:
            active = True
            continue

        match = re.match('.*Ending measurements', line)
        if match:
            active = False
            continue

        if active:
            match = re.match('[0-9.]+ (.*) clients: ([0-9.]+) Kops/sec, '
                        '([0-9.]+) Gbps.*P50 ([0-9.]+)', line)
            if match:
                node_data = experiments[match.group(1)][node]
                gbps = float(match.group(3))
                if gbps >= 0.0:
                    if not "client_kops" in node_data:
                        node_data["client_kops"] = []
                    node_data["client_kops"].append(float(match.group(2)))
                    if not "client_gbps" in node_data:
                        node_data["client_gbps"] = []
                    node_data["client_gbps"].append(gbps)
                    if not "client_latency" in node_data:
                        node_data["client_latency"] = []
                    node_data["client_latency"].append(float(match.group(4)))
                continue

            match = re.match('[0-9.]+ (.*) servers: ([0-9.]+) Kops/sec, '
                    '([0-9.]+) Gbps', line)
            if match:
                node_data = experiments[match.group(1)][node]
                gbps = float(match.group(3))
                if gbps >= 0.0:
                    if not "server_kops" in node_data:
                        node_data["server_kops"] = []
                    node_data["server_kops"].append(float(match.group(2)))
                    if not "server_gbps" in node_data:
                        node_data["server_gbps"] = []
                    node_data["server_gbps"].append(gbps)
                continue

            match = re.match('.*Outstanding client RPCs for (.*) '
                    'experiment: ([0-9.]+)', line)
            if match:
                node_data = experiments[match.group(1)][node]
                if not "outstanding_rpcs" in node_data:
                    node_data["outstanding_rpcs"] = []
                node_data["outstanding_rpcs"].append(int(match.group(2)))
                continue

            match = re.match('.*Backed-up (.*) sends: ([0-9.]+)/([0-9.]+)',
                    line)
            if match:
                node_data = experiments[match.group(1)][node]
                if not "backups" in node_data:
                    node_data["backups"] = []
                total = float(match.group(3))
                if total > 0:
                    node_data["backups"].append(float(match.group(2))/total)
                continue
    if not exited:
        log("%s appears to have crashed (didn't exit)" % (node))
    if timeouts > 1:
        log("%s: %d additional Homa RPC timeouts" % (file, timeouts-1))

def scan_logs():
    """
    Read all of the node-specific log files produced by a run, and
    extract useful information.
    """
    global log_dir, verbose

    # Data collected so far for all experiments. See scan_log header
    # comment for more info.
    experiments = defaultdict(lambda : defaultdict(dict))

    for file in sorted(glob.glob(log_dir + "/node*.log")):
        node = re.match(r'.*/(node[0-9]+)\.log', file).group(1)
        scan_log(file, node, experiments)

    for name, exp in experiments.items():
        totals = {}
        nodes = {}
        nodes["client"] = {}
        nodes["server"] = {}
        nodes["all"] = {}

        for type in ['client', 'server']:
            gbps_key = type + "_gbps"
            kops_key = type + "_kops"
            averages = []
            vlog("\n%ss for %s experiment:" % (type.capitalize(), name))
            for node in sorted(exp.keys()):
                if not gbps_key in exp[node]:
                    if name.startswith("unloaded"):
                        exp[node][gbps_key] = [0.0]
                        exp[node][kops_key] = [0.0]
                    else:
                        continue
                gbps = exp[node][gbps_key]
                avg = sum(gbps)/len(gbps)
                vlog("%s: %.2f Gbps (%s)" % (node, avg,
                    ", ".join(map(lambda x: "%.1f" % (x), gbps))))
                averages.append(avg)
                nodes["all"][node] = 1
                nodes[type][node] = 1
            if len(averages) > 0:
                totals[gbps_key] = sum(averages)
                vlog("%s average: %.2f Gbps\n"
                        % (type.capitalize(), totals[gbps_key]/len(averages)))

            averages = []
            for node in sorted(exp.keys()):
                key = type + "_kops"
                if not kops_key in exp[node]:
                    continue
                kops = exp[node][kops_key]
                avg = sum(kops)/len(kops)
                vlog("%s: %.1f Kops/sec (%s)" % (node, avg,
                    ", ".join(map(lambda x: "%.1f" % (x), kops))))
                averages.append(avg)
                nodes["all"][node] = 1
                nodes[type][node] = 1
            if len(averages) > 0:
                totals[kops_key] = sum(averages)
                vlog("%s average: %.1f Kops/sec"
                        % (type.capitalize(), totals[kops_key]/len(averages)))

        for key in ["client_gbps", "client_kops", "server_gbps", "server_kops"]:
            if not key in totals:
                log("%s missing in node log files" % (key))
                totals[key] = 0

        log("\nClients for %s experiment: %d nodes, %.2f Gbps, %.1f Kops/sec "
                "(avg per node)" % (name, len(nodes["client"]),
                totals["client_gbps"]/len(nodes["client"]),
                totals["client_kops"]/len(nodes["client"])))
        if len(nodes["server"]) > 0:
            log("Servers for %s experiment: %d nodes, %.2f Gbps, %.1f Kops/sec "
                    "(avg per node)" % (name, len(nodes["server"]),
                    totals["server_gbps"]/len(nodes["server"]),
                    totals["server_kops"]/len(nodes["server"])))
        log("Overall for %s experiment: %d nodes, %.2f Gbps, %.1f Kops/sec "
                "(avg per node)" % (name, len(nodes["all"]),
                (totals["client_gbps"] + totals["server_gbps"])/len(nodes["all"]),
                (totals["client_kops"] + totals["server_kops"])/len(nodes["all"])))

        for node in sorted(exp.keys()):
            if "outstanding_rpcs" in exp[node]:
                counts = exp[node]["outstanding_rpcs"]
                log("Outstanding RPCs for %s: %s" % (node,
                        ", ".join(map(lambda x: "%d" % (x), counts))))
                break

        backups = []
        for node in sorted(exp.keys()):
            if "backups" in exp[node]:
                fracs = exp[node]["backups"]
                vlog("Backed-up RPCs for %s: %s" % (node,
                        ", ".join(map(lambda x: "%.1f%%" % (100.0*x), fracs))))
                backups.extend(fracs)
        if len(backups) > 0:
            log("Average rate of backed-up RPCs: %.1f%%"
                    % (100.0*sum(backups)/len(backups)))
    log("")

def scan_metrics(experiment):
    """
    Reads in all of the .metrics files generated by an experiment,
    extracts a few interesting statistics, and logs message if some
    nodes appear to have significantly different behavior than
    others (to detect flakey nodes)
    """

    metrics_files = sorted(glob.glob(log_dir + ("/%s-*.metrics" % (experiment))))
    if len(metrics_files) == 0:
        return

    metric_names = ({'packets_sent_RESEND', 'packets_rcvd_RESEND'})
    docs = {'cores': 'core utilization',
            'packets_sent_RESEND': 'outgoing resend requests',
            'packets_rcvd_RESEND': 'incoming resend requests'}
    units = {'cores': '',
            'packets_sent_RESEND': '/s',
            'packets_rcvd_RESEND': '/s'}
    thresholds = {'cores': 2,
            'packets_sent_RESEND': 5,
            'packets_rcvd_RESEND': 5}
    # Keys are same as in docs above, values are dictionaries, in which
    # keys are metric file names and values are the value of the corresponding
    # metric name in that metrics file.
    metrics = {}
    for name in docs.keys():
        metrics[name] = {}
    for file in metrics_files:
        f = open(file)
        for name in docs.keys():
            metrics[name][file] = 0
        for line in f:
            match = re.match('Total Core Utilization *([0-9.]+)', line)
            if match:
                metrics['cores'][file] = float(match.group(1))
                continue
            match = re.match(r'([^ ]+) +([0-9]+) +\( *([0-9.]+ *[MKG]?)/s', line)
            if not match:
                continue
            name = match.group(1)
            if name in metric_names:
                metrics[name][file] = unscale_number(match.group(3))
        f.close()
    outlier_count = 0
    for name in metrics:
        values = sorted(metrics[name].values())
        median = values[len(values)//2]
        if (median == 0) and name == 'cores':
            log("Couldn't find core utilization in metrics files")
            continue
        for file, value in metrics[name].items():
            if (value >= thresholds[name]) and (value > 1.5*median):
                log("Outlier %s in %s: %s vs. %s median"
                        % (docs[name], file, scale_number(value, units[name]),
                        scale_number(median, units[name])))

def read_rtts(file, rtts, min_rtt = 0.0, link_mbps = 0.0):
    """
    Read a file generated by cp_node's "dump_times" command and add its
    data to the information present in rtts. Also computes average slowdown
    across all the data in this file.

    file:       Name of the log file.
    rtts:       Dictionary whose keys are message lengths; each value is a
                list of all of the rtts recorded for that message length
                (in usecs)
    min_rtt:    If nonzero, gives the minimum possible RTT for a short RPC
                (used to compute slowdowns)
    link_mbps:  Speed of the host's uplink in Mbps.
    Returns:    The total number of rtts read from the file, and also the
                average slowdown from this file. If min_rtt is zero, then
                the slowdown will be zero.
    """

    num_rtts = 0
    slowdown_sum = 0
    f = open(file, "r")
    for line in f:
        stripped = line.strip()
        if stripped[0] == '#':
            continue
        words = stripped.split()
        if (len(words) < 2):
            log("Line in %s too short (need at least 2 columns): '%s'" %
                    (file, line))
            continue
        length = int(words[0])
        usec = float(words[1])
        if length in rtts:
            rtts[length].append(usec)
        else:
            rtts[length] = [usec]
        if min_rtt > 0:
            slowdown_sum += usec/(min_rtt + length*8/link_mbps)
        num_rtts += 1
    f.close()
    if num_rtts == 0:
        return 0, 0
    return num_rtts, slowdown_sum/num_rtts

def get_buckets(rtts, total):
    """
    Generates buckets for histogramming the information in rtts.

    rtts:     A collection of message rtts, as returned by read_rtts
    total:    Total number of samples in rtts
    Returns:  A list of <length, cum_frac> pairs, in sorted order. The length
              is the largest message size for a bucket, and cum_frac is the
              fraction of all messages with that length or smaller.
    """
    buckets = []
    cumulative = 0
    for length in sorted(rtts.keys()):
        cumulative += len(rtts[length])
        buckets.append([length, cumulative/total])
    return buckets

def set_unloaded(experiment):
    """
    Collect measurements from an unloaded system to use in computing slowdowns.

    experiment:   Name of experiment that measured RTTs under low load
    """
    global unloaded_p50, min_rtt

    # Find (or generate) unloaded data for comparison.
    files = sorted(glob.glob("%s/%s-*.rtts" % (log_dir, experiment)))
    if len(files) == 0:
        raise Exception("Couldn't find %s RTT data" % (experiment))
    rtts = {}
    for file in files:
        read_rtts(file, rtts)
    unloaded_p50.clear()
    min_rtt = 1e20
    for length in rtts.keys():
        sorted_rtts = sorted(rtts[length])
        unloaded_p50[length] = sorted_rtts[len(rtts[length])//2]
        min_rtt = min(min_rtt, sorted_rtts[0])
    vlog("Computed unloaded_p50: %d entries" % len(unloaded_p50))

def get_digest(experiment):
    """
    Returns an element of digest that contains data for a particular
    experiment; if this is the first request for a given experiment, the
    method reads the data for experiment and generates the digest. For
    each new digest generated, a .data file is generated in the "reports"
    subdirectory of the log directory.

    experiment:  Name of the desired experiment
    """
    global old_slowdown, digests, log_dir, min_rtt, unloaded_p50, delete_rtts
    global link_mbps

    if experiment in digests:
        return digests[experiment]
    digest = {}
    digest["rtts"] = {}
    digest["total_messages"] = 0
    digest["lengths"] = []
    digest["cum_frac"] = []
    digest["counts"] = []
    digest["p50"] = []
    digest["p99"] = []
    digest["p999"] = []
    digest["slow_50"] = []
    digest["slow_99"] = []
    digest["slow_999"] = []

    avg_slowdowns = []

    # Read in the RTT files for this experiment.
    files = sorted(glob.glob(log_dir + ("/%s-*.rtts" % (experiment))))
    if len(files) == 0:
        raise Exception("Couldn't find RTT data for %s experiment"
                % (experiment))
    sys.stdout.write("Reading RTT data for %s experiment: " % (experiment))
    sys.stdout.flush()
    for file in files:
        count, slowdown = read_rtts(file, digest["rtts"], min_rtt, link_mbps)
        digest["total_messages"] += count
        avg_slowdowns.append([file, slowdown])
        sys.stdout.write("#")
        sys.stdout.flush()

        if delete_rtts and not ("unloaded" in file):
            os.remove(file)
    log("")

    # See if some nodes have anomalous performance.
    overall_avg = 0.0
    for info in avg_slowdowns:
        overall_avg += info[1]
    overall_avg = overall_avg/len(avg_slowdowns)
    for info in avg_slowdowns:
        if (info[1] < 0.8*overall_avg) or (info[1] > 1.2*overall_avg):
            log("Outlier alt-slowdown in %s: %.1f vs. %.1f overall average"
                    % (info[0], info[1], overall_avg))

    if old_slowdown and (len(unloaded_p50) == 0):
        raise Exception("No unloaded data: must invoke set_unloaded")

    rtts = digest["rtts"]
    buckets = get_buckets(rtts, digest["total_messages"])
    bucket_length, bucket_cum_frac = buckets[0]
    next_bucket = 1
    bucket_rtts = []
    bucket_slowdowns = []
    bucket_count = 0
    slowdown_sum = 0.0
    lengths = sorted(rtts.keys())
    lengths.append(999999999)            # Force one extra loop iteration
    if old_slowdown:
        optimal = unloaded_p50[min(unloaded_p50.keys())]
    else:
        optimal = 15 + lengths[0]*8/link_mbps
    for length in lengths:
        if length > bucket_length:
            digest["lengths"].append(bucket_length)
            digest["cum_frac"].append(bucket_cum_frac)
            digest["counts"].append(bucket_count)
            if len(bucket_rtts) == 0:
                bucket_rtts.append(0)
                bucket_slowdowns.append(0)
            bucket_rtts = sorted(bucket_rtts)
            digest["p50"].append(bucket_rtts[bucket_count//2])
            digest["p99"].append(bucket_rtts[bucket_count*99//100])
            digest["p999"].append(bucket_rtts[bucket_count*999//1000])
            bucket_slowdowns = sorted(bucket_slowdowns)
            digest["slow_50"].append(bucket_slowdowns[bucket_count//2])
            digest["slow_99"].append(bucket_slowdowns[bucket_count*99//100])
            digest["slow_999"].append(bucket_slowdowns[bucket_count*999//1000])
            if next_bucket >= len(buckets):
                break
            bucket_rtts = []
            bucket_slowdowns = []
            bucket_count = 0
            bucket_length, bucket_cum_frac = buckets[next_bucket]
            next_bucket += 1
        if old_slowdown:
            optimal = unloaded_p50[length]
        else:
            optimal = 15 + length*8/link_mbps
        bucket_count += len(rtts[length])
        for rtt in rtts[length]:
            bucket_rtts.append(rtt)
            slowdown = rtt/optimal
            bucket_slowdowns.append(slowdown)
            slowdown_sum += slowdown

    # Get stats for shortest 10% of messages
    small_rtts = []
    small_count = 0
    for length in lengths:
        small_rtts.extend(rtts[length])
        if len(small_rtts)/digest["total_messages"] > 0.1:
            break
    small_rtts.sort()
    digest["avg_slowdown"] = slowdown_sum/digest["total_messages"]
    log("%s has %d RPCs, avg slowdown %.2f, %d messages < %d bytes "
            "(min %.1f us P50 %.1f us P99 %.1f us)" % (experiment,
            digest["total_messages"], digest["avg_slowdown"], len(small_rtts),
            length, small_rtts[0], small_rtts[len(small_rtts)//2],
            small_rtts[99*len(small_rtts)//100]))

    dir = "%s/reports" % (log_dir)
    f = open("%s/reports/%s.data" % (log_dir, experiment), "w")
    f.write("# Digested data for %s experiment, run at %s\n"
            % (experiment, date_time))
    f.write("# length  cum_frac  samples     p50      p99     p999   "
            "s50    s99    s999\n")
    for i in range(len(digest["lengths"])):
        f.write(" %7d %9.6f %8d %7.1f %8.1f %8.1f %5.1f %6.1f %7.1f\n"
                % (digest["lengths"][i], digest["cum_frac"][i],
                digest["counts"][i], digest["p50"][i], digest["p99"][i],
                digest["p999"][i], digest["slow_50"][i],
                digest["slow_99"][i], digest["slow_999"][i]))
    f.close()

    digests[experiment] = digest
    return digest

def start_slowdown_plot(title, max_y, x_experiment, size=10,
        show_top_label=True, show_bot_label=True, figsize=[6,4],
        y_label="Slowdown", show_upper_x_axis=True):
    """
    Create a pyplot graph that will be used for slowdown data. Returns the
    Axes object for the plot.

    title:             Title for the plot; may be empty
    max_y:             Maximum y-coordinate
    x_experiment:      Name of experiment whose rtt distribution will be used to
                       label the x-axis of the plot. None means don't label the
                       x-axis (caller will presumably invoke cdf_xaxis to do it).
    size:              Size to use for fonts
    show_top_label:    True means display title text for upper x-axis
    show_bot_label:    True means display title text for lower x-axis
    figsize:           Dimensions of plot
    y_label:           Label for the y-axis
    show_upper_x_axis: Display upper x-axis ticks and labels (percentiles)
    """

    fig = plt.figure(figsize=figsize)
    ax = fig.add_subplot(111)
    if title != "":
        ax.set_title(title, size=size)
    ax.set_xlim(0, 1.0)
    ax.set_yscale("log")
    ax.set_ylim(1, max_y)
    ax.tick_params(right=True, which="both", direction="in", length=5)
    ticks = []
    labels = []
    y = 1
    while y <= max_y:
        ticks.append(y)
        labels.append("%d" % (y))
        y = y*10
    ax.set_yticks(ticks)
    ax.set_yticklabels(labels, size=size)
    if show_bot_label:
        ax.set_xlabel("Message Length (bytes)", size=size)
    ax.set_ylabel(y_label, size=size)
    ax.grid(which="major", axis="y")

    if show_upper_x_axis:
        top_axis = ax.twiny()
        top_axis.tick_params(axis="x", direction="in", length=5)
        top_axis.set_xlim(0, 1.0)
        top_ticks = []
        top_labels = []
        for x in range(0, 11, 2):
            top_ticks.append(x/10.0)
            top_labels.append("%d%%" % (x*10))
        top_axis.set_xticks(top_ticks)
        top_axis.set_xticklabels(top_labels, size=size)
        if show_top_label:
            top_axis.set_xlabel("Cumulative % of Messages", size=size)
        top_axis.xaxis.set_label_position('top')

    if x_experiment != None:
        # Generate x-axis labels
        ticks = []
        labels = []
        cumulative_count = 0
        target_count = 0
        tick = 0
        digest = get_digest(x_experiment)
        rtts = digest["rtts"]
        total = digest["total_messages"]
        for length in sorted(rtts.keys()):
            cumulative_count += len(rtts[length])
            while cumulative_count >= target_count:
                ticks.append(target_count/total)
                if length < 1000:
                    labels.append("%.0f" % (length))
                elif length < 100000:
                    labels.append("%.1fK" % (length/1000))
                elif length < 1000000:
                    labels.append("%.0fK" % (length/1000))
                else:
                    labels.append("%.1fM" % (length/1000000))
                tick += 1
                target_count = (total*tick)/10
        ax.set_xticks(ticks)
        ax.set_xticklabels(labels, size=size)
    return ax

def cdf_xaxis(ax, x_values, counts, num_ticks, size=10):
    """
    Generate labels for an x-axis that is scaled nonlinearly to reflect
    a particular distribution of samples.

    ax:       matplotlib Axes object for the plot
    x:        List of x-values
    counts:   List giving the number of samples for each point in x
    ticks:    Total number of ticks go generate (including axis ends)
    size:     Font size to use for axis labels
    """

    ticks = []
    labels = []
    total = sum(counts)
    cumulative_count = 0
    target_count = 0
    tick = 0
    for (x, count) in zip(x_values, counts):
        cumulative_count += count
        while cumulative_count >= target_count:
            ticks.append(target_count/total)
            if x < 1000:
                labels.append("%.0f" % (x))
            elif x < 100000:
                labels.append("%.1fK" % (x/1000))
            elif x < 1000000:
                labels.append("%.0fK" % (x/1000))
            else:
                labels.append("%.1fM" % (x/1000000))
            tick += 1
            target_count = (total*tick)/(num_ticks-1)
    ax.set_xticks(ticks)
    ax.set_xticklabels(labels, size=size)


def make_histogram(x, y, init=None, after=True):
    """
    Given x and y coordinates, return new lists of coordinates that describe
    a histogram (transform (x1,y1) and (x2,y2) into (x1,y1), (x2,y1), (x2,y2)
    to make steps.

    x:        List of x-coordinates
    y:        List of y-coordinates corresponding to x
    init:     An optional initial point (x and y coords) which will be
              plotted before x and y
    after:    True means the horizontal line corresponding to each
              point occurs to the right of that point; False means to the
              left
    Returns:  A list containing two lists, one with new x values and one
              with new y values.
    """
    x_new = []
    y_new = []
    if init:
        x_new.append(init[0])
        y_new.append(init[1])
    for i in range(len(x)):
        if i != 0:
            if after:
                x_new.append(x[i])
                y_new.append(y[i-1])
            else:
                x_new.append(x[i-1])
                y_new.append(y[i])
        x_new.append(x[i])
        y_new.append(y[i])
    return [x_new, y_new]

def plot_slowdown(ax, experiment, percentile, label, **kwargs):
    """
    Add a slowdown histogram to a plot.

    ax:            matplotlib Axes object: info will be plotted here.
    experiment:    Name of the experiment whose data should be graphed.
    percentile:    While percentile of slowdown to graph: must be "p50", "p99",
                   or "p999"
    label:         Text to display in the graph legend for this curve
    kwargs:        Additional keyword arguments to pass through to plt.plot
    """

    digest = get_digest(experiment)
    if percentile == "p50":
        x, y = make_histogram(digest["cum_frac"], digest["slow_50"],
                init=[0, digest["slow_50"][0]], after=False)
    elif percentile == "p99":
        x, y = make_histogram(digest["cum_frac"], digest["slow_99"],
                init=[0, digest["slow_99"][0]], after=False)
    elif percentile == "p999":
        x, y = make_histogram(digest["cum_frac"], digest["slow_999"],
                init=[0, digest["slow_999"][0]], after=False)
    else:
        raise Exception("Bad percentile selector %s; must be p50, p99, or p999"
                % (percentile))
    ax.plot(x, y, label=label, **kwargs)

def start_cdf_plot(title, min_x, max_x, min_y, x_label, y_label,
        figsize=[5, 4], size=10, xscale="log", yscale="log"):
    """
    Create a pyplot graph that will be display a complementary CDF with
    log axes.

    title:      Overall title for the graph (empty means no title)
    min_x:       Smallest x-coordinate that must be visible
    max_x:       Largest x-coordinate that must be visible
    min_y:       Smallest y-coordinate that must be visible (1.0 is always
                 the largest value for y)
    x_label:     Label for the x axis (empty means no label)
    y_label:     Label for the y axis (empty means no label)
    figsize:     Dimensions of plot
    size:        Size to use for fonts
    xscale:      Scale for x-axis: "linear" or "log"
    yscale:      Scale for y-axis: "linear" or "log"
    """
    plt.figure(figsize=figsize)
    if title != "":
        plt.title(title, size=size)
    plt.axis()
    plt.xscale(xscale)
    ax = plt.gca()

    # Round out the x-axis limits to even powers of 10.
    exp = math.floor(math.log(min_x , 10))
    min_x = 10**exp
    exp = math.ceil(math.log(max_x, 10))
    max_x = 10**exp
    plt.xlim(min_x, max_x)
    ticks = []
    tick = min_x
    while tick <= max_x:
        ticks.append(tick)
        tick = tick*10
    plt.xticks(ticks)
    plt.tick_params(top=True, which="both", direction="in", labelsize=size,
            length=5)

    plt.yscale(yscale)
    plt.ylim(min_y, 1.0)
    # plt.yticks([1, 10, 100, 1000], ["1", "10", "100", "1000"])
    if x_label:
        plt.xlabel(x_label, size=size)
    if y_label:
        plt.ylabel(y_label, size=size)
    plt.grid(which="major", axis="y")
    plt.grid(which="major", axis="x")
    plt.plot([min_x, max_x*1.2], [0.5, 0.5], linestyle= (0, (5, 3)),
            color="red", clip_on=False)
    plt.text(max_x*1.3, 0.5, "P50", fontsize=16, horizontalalignment="left",
            verticalalignment="center", color="red")
    plt.plot([min_x, max_x*1.2], [0.01, 0.01], linestyle= (0, (5, 3)),
            color="red", clip_on=False)
    plt.text(max_x*1.3, 0.01, "P99", fontsize=16, horizontalalignment="left",
            verticalalignment="center", color="red")

def get_short_cdf(experiment):
    """
    Return a complementary CDF histogram for the RTTs of short messages in
    an experiment. Short messages means all messages shorter than 1500 bytes
    that are also among the 10% of shortest messages (if there are no messages
    shorter than 1500 bytes, then extract data for the shortest message
    length available). This function also saves the data in a file in the
    reports directory.

    experiment:  Name of the experiment containing the data to plot
    Returns:     A list with two elements (a list of x-coords and a list
                 of y-coords) that histogram the complementary cdf.
    """
    global log_dir, date_time
    short = []
    digest = get_digest(experiment)
    rtts = digest["rtts"]
    messages_left = digest["total_messages"]//10
    longest = 0
    for length in sorted(rtts.keys()):
        if (length >= 1500) and (len(short) > 0):
            break
        short.extend(rtts[length])
        messages_left -= len(rtts[length])
        longest = length
        if messages_left < 0:
            break
    vlog("Largest message used for short CDF for %s: %d"
            % (experiment, longest))
    x = []
    y = []
    total = len(short)
    remaining = total
    f = open("%s/reports/%s_cdf.data" % (log_dir, experiment), "w")
    f.write("# Fraction of RTTS longer than a given time for %s experiment\n"
            % (experiment))
    f.write("# Includes messages <= %d bytes; measured at %s\n"
            % (longest, date_time))
    f.write("# Data collected at %s \n" % (date_time))
    f.write("#       usec        frac\n")

    # Reduce the volume of data by waiting to add new points until there
    # has been a significant change in either coordinate. "prev" variables hold
    # the last point actually graphed.
    prevx = 0
    prevy = 1.0
    for rtt in sorted(short):
        remaining -= 1
        frac = remaining/total
        if (prevy != 0) and (prevx != 0) and (abs((frac - prevy)/prevy) < .01) \
                and (abs((rtt - prevx)/prevx) < .01):
            continue
        if len(x) > 0:
            x.append(rtt)
            y.append(prevy)
        x.append(rtt)
        y.append(frac)
        f.write("%12.3f  %.8f\n" % (rtt, frac))
        prevx = rtt
        prevy = frac
    f.close()
    return [x, y]

def read_file_data(file):
    """
    Reads data from a file and returns a dict whose keys are column names
    and whose values are lists of values from the given column.

    file:   Path to the file containing the desired data. The file consists
            of initial line containing space-separated column names, followed
            any number of lines of data. Blank lines and lines starting with
            "#" are ignored.
    """
    columns = {}
    names = None
    f = open(file)
    for line in f:
        fields = line.strip().split()
        if len(fields) == 0:
            continue
        if fields[0] == '#':
            continue
        if not names:
            names = fields
            for n in names:
                columns[n] = []
        else:
            if len(fields) != len(names):
                print("Bad line in %s: %s (expected %d columns, got %d)"
                        % (file, line.rstrip(), len(columns), len(fields)))
                continue
            for i in range(0, len(names)):
                try:
                    value = float(fields[i])
                except ValueError:
                    value = fields[i]
                columns[names[i]].append(value)
    f.close()
    return columns

def column_from_file(file, column):
    """
    Return a list containing a column of data from a given file.

    file:    Path to the file containing the desired data.
    column:  Name of the column within the file.
    """

    global data_from_files
    if file in data_from_files:
        return data_from_files[file][column]

    data = {}
    last_comment = ""
    columns = []
    for line in open(file):
        fields = line.strip().split()
        if len(fields) == 0:
            continue
        if fields[0] == '#':
            last_comment = line
            continue
        if len(columns) == 0:
            # Parse column names
            if len(last_comment) == 0:
                raise Exception("no columns headers in data file '%s'" % (file))
            columns = last_comment.split()
            columns.pop(0)
            for c in columns:
                data[c] = []
        for i in range(0, len(columns)):
                data[columns[i]].append(float(fields[i]))
    data_from_files[file] = data
    return data[column]

def scale_number(number, units):
    """
    Return a string describing a number, but with a "K", "M", or "G"
    suffix to keep the number small and readable.

    number: number to scale
    units:  additional units designation, such as "bps" or "/s" to add
    """

    if number > 1000000000:
        return "%.1f G%s" % (number/1000000000.0, units)
    if number > 1000000:
        return "%.1f M%S" % (number/1000000.0, units)
    elif (number > 1000):
        return "%.1f K%s" % (number/1000.0, units)
    else:
        if units == "":
            space = ""
        else:
            space = " "
        return "%.1f%s%s" % (number, space, units)

def unscale_number(number):
    """
    Given a string representation of a number, which may have a "K",
    "M", or "G" scale factor (e.g. "1.2 M"), return the actual number
    (e.g. 1200000).
    """
    match = re.match("([0-9.]+) *([GMK]?)$", number)
    if not match:
        raise Exception("Couldn't unscale '%s': bad syntax" % (number))
    mantissa = float(match.group(1))
    scale = match.group(2)
    if scale == 'G':
        return mantissa * 1e09
    elif scale == 'M':
        return mantissa * 1e06
    elif scale == 'K':
        return mantissa * 1e03
    else:
        return mantissa