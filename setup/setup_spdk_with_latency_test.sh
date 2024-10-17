#!/usr/bin/env bash
# Author: CS0522
# Setup SPDK with latency_test
# !!! Run with root !!!
# Steps:
# 1. Install the dependencies according to SPDK instruction;
# 2. Build;
# 3. Unit test.

function install_dependencies() {
    # -r: with RDMA dependencies
    ./scripts/pkgdep.sh -r
}

function build_spdk_with_latency_test() {
    # configure with rdma
    ./configure --with-rdma --enable-debug
    # make
    # default:  define PERF_LATENCY_LOG
    #           notdef LANTENCY_LOG
    make -j4
}

function unit_test() {
    # unit test
    local unit_test_result=`./test/unit/unittest.sh | tail -n 10 | grep "All unit tests passed"`
    # check test result
    if [ "All unit tests passed" = ${unit_test_result} ]; then
        echo "Setup SPDK with latency_test succeeded. "
    else
        echo "Setup SPDK with latency_test failed. "
    fi
}

function usage() {
    echo "Args:                 <sh_name=setup_rdma.sh> <is_mlnx>"
    echo "sh_name:              shell script name"
}

function setup_spdk_with_latency_test_fn() {
    cd ../
    install_dependencies
    build_spdk_with_latency_test
    unit_test
}

### run
setup_spdk_with_latency_test_fn