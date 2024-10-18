#!/usr/bin/env bash
# Author: CS0522
# Setup Host node
# !!! Run with root !!!
# Steps:
# 1. Re-run ./configure with RDMA and with perf latency log;
# 2. Re-build;
# 3. Configure Host env;
# 4. Run simple test.

# pwd: spdk_dir

function rebuild_host_spdk_with_latency_test() {
    # configure with rdma and perf latency log
    ./configure --with-rdma --with-perf-latency-log
    # make
    # define PERF_LATENCY_LOG
    make -j4
}

function configure_host_env() {
    echo "Configuring Host Environment..."
    # setup spdk env
    ./scripts/setup.sh
    sleep 5s
    echo "Done."
}

function configure_host_fn() {
    rebuild_host_spdk_with_latency_test
    configure_host_env
}