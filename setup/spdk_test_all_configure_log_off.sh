#!/usr/bin/env bash
# Author: CS0522
# Setup SPDK with latency_test
# !!! Run with root !!!
# Steps:
# 1. Install the dependencies according to SPDK instruction;
# 2. Build;
# 3. Unit test.

# pwd: spdk_dir

set -eu

function build_spdk_with_latency_test() {
    # configure with rdma
    ./configure --with-rdma
    # make
    # default:  not define PERF_LATENCY_LOG
    #           not define TARGET_LATENCY_LOG
    # to turn on LATENCY_LOG, 
    # add args '--with-perf-latency-log', '--with-target-latency-log' while executing './configure'
    make -j4
}

function setup_spdk_env() {
    ./scripts/setup.sh
    sleep 5s
}

function setup_spdk_with_latency_test_fn() {
    build_spdk_with_latency_test
    setup_spdk_env
}

### run
setup_spdk_with_latency_test_fn