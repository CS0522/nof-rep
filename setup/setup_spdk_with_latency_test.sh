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

function install_dependencies() {
    # -r: with RDMA dependencies
    ./scripts/pkgdep.sh -r
}

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

function unit_test() {
    # unit test
    local unit_test_result=`./test/unit/unittest.sh | tail -n 10 | grep "All unit tests passed"`
    echo ${unit_test_result}
    # check test result
    if [[ "All unit tests passed" == ${unit_test_result} ]]; then
        echo "Setup SPDK with latency_test succeeded. "
    else
        echo "Setup SPDK with latency_test failed. "
    fi
}

function usage() {
    echo "Params:               <sh_name=setup_spdk_with_latency_test.sh>"
    echo "sh_name:              shell script name"
    echo "skip_verify:          skip verify the original spdk and its unittest"
}

### check params ###
if [ $# -ne 1 ]; then
    usage
    exit
fi
### end check ######

skip_verify_original_spdk_and_unit_test=$1

function setup_spdk_with_latency_test_fn() {
    install_dependencies
    if [ ${skip_verify_original_spdk_and_unit_test} -eq 0 ]; then
        build_spdk_with_latency_test
        unit_test
    fi
}

### run
setup_spdk_with_latency_test_fn