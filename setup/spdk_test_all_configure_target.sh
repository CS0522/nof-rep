#!/usr/bin/env bash
# Author: CS0522
# Setup Target node
# !!! Run with root !!!
# Steps:
# 1. Re-run ./configure with RDMA and with target latency log;
# 2. Re-build;
# 3. Configure Target env: attach ctrlr, create subsys, add ns and so on;
# 4. Add listener.

# pwd: spdk_dir

set -eu

function rebuild_target_spdk_with_latency_test() {
    # configure with rdma and target latency log
    ./configure --with-rdma --with-target-latency-log
    # make
    # define TARGET_LATENCY_LOG
    make -j4
}

function setup_spdk_env() {
    ./scripts/setup.sh
    sleep 5s
}

function configure_target_fn() {
    rebuild_target_spdk_with_latency_test
    setup_spdk_env
}

### run
configure_target_fn