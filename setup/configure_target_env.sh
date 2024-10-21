#!/usr/bin/env bash
# Author: CS0522
# Setup Target node
# !!! Run with root !!!
# Steps:
# 1. Re-run ./configure with RDMA and with target latency log;
# 2. Re-build;
# 3. Start nvmf_tgt.

# pwd: spdk_dir

set -eu

function usage() {
    echo "Params:                 <sh_name=configure_target_env.sh> <is_100g>"
    echo "sh_name:              shell script name"
    echo "is_100g:              100 Gbps or normal?"
}

### check args ###
if [ $# -ne 1 ]; then
    usage
    exit
fi
### end check ####

is_100g=$1

function rebuild_target_spdk_with_latency_test() {
    # configure with rdma and target latency log
    ./configure --with-rdma --with-target-latency-log
    # make
    # define TARGET_LATENCY_LOG
    make -j4
}

function setup_spdk_env() {
    ./scripts/setup.sh
}

function run_nvmf_tgt() {
    # it needs more than 2 cores
    ./build/bin/nvmf_tgt -m 0x3 &
}

function configure_target_env_fn() {
    rebuild_target_spdk_with_latency_test
    echo "Starting nvmf_tgt..."
    # setup spdk env
    setup_spdk_env
    sleep 5s
    # run nvmf_tgt and let it background running
	run_nvmf_tgt
    sleep 1s

    echo "Done."
    exit
}

### run
configure_target_env_fn