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

function rebuild_target_spdk_with_latency_test() {
    # configure with rdma and target latency log
    ./configure --with-rdma --with-target-latency-log
    # make
    # define TARGET_LATENCY_LOG
    make -j4
}

subsys_nqn="nqn.2016-06.io.spdk:cnode1"
bdf=""
nvme_ctrlr=""
local_ip=""

function setup_spdk_env() {
    ./scripts/setup.sh
}

function get_bdf() {
    bdf=`./scripts/setup.sh status 2>&1 | grep NVMe | tail -n 1 | awk '{print $2}'`
	echo "bdf=${bdf}"
}

function run_nvmf_tgt() {
    ./build/bin/nvmf_tgt &
}

function create_transport() {
    ./scripts/rpc.py nvmf_create_transport -t RDMA -u 8192 -i 131072 -c 8192
}

function attach_controller() {
    nvme_ctrlr=`./scripts/rpc.py bdev_nvme_attach_controller -b NVMe1 -t PCIe -a ${bdf} | grep NVMe`
	# output:
	# NVMe1n1
	echo "nvme_ctrlr=${nvme_ctrlr}"
}

function create_subsystem() {
    ./scripts/rpc.py nvmf_create_subsystem ${subsys_nqn} -a -s SPDK00000000000001 -d SPDK_Controller1
}

function delete_subsystem() {
    ./scripts/rpc.py nvmf_delete_subsystem ${subsys_nqn}
}

function get_subsystems() {
    ./scripts/rpc.py nvmf_get_subsystems
}

function add_ns() {
    ./scripts/rpc.py nvmf_subsystem_add_ns ${subsys_nqn} ${nvme_ctrlr}
}

function remove_ns() {
    # get subsystems
    local ns_id=`./scripts/rpc.py nvmf_get_subsystems | grep nsid | awk -F: '{print $2}' | awk -F, '{print $1}'`
    ./scripts/rpc.py nvmf_remove_ns ${ns_id}
}

function configure_target_env() {
    echo "Configuring Target Environment..."
    # setup spdk env
    setup_spdk_env
    sleep 5s
    # get bdf addr
	get_bdf
	sleep 1s
    # 1. run nvmf_tgt and let it background running
	run_nvmf_tgt
    sleep 1s
	# 2. create transport
	create_transport
	sleep 1s
	# 3. attach nvme controller
	attach_controller
	sleep 1s
	# 4. create subsystem
	create_subsystem
	sleep 1s
	# 5. add namespace
	add_ns

    echo "Done."
}

# get local IP address
function get_local_ip() {
    # local_ip=`ifconfig -a | grep inet | grep -v 127.0.0.1 | grep -v inet6 | awk '{print $2}' | tr -d "addr:"`
    local_ip=`hostname -I | awk '{print $1}'`
    echo "local_ip=${local_ip}"
}

function add_listener() {
    sleep 3s
    ./scripts/rpc.py nvmf_subsystem_add_listener ${subsys_nqn} -t rdma -a ${local_ip} -s 4420
}

function check_listening_status() {
    curr_status=`./scripts/rpc.py nvmf_subsystem_get_listeners ${subsys_nqn}`
    echo "curr_status=${curr_status}"
    if [[ ${curr_status} == "[]" ]]; then
        echo "Add listener failed. "
        echo "Configure Target failed. "
    else
        echo "Add listener succeeded. "
        echo "Configure Target succeeded. "
    fi
}

function remove_listener() {
    ./scripts/rpc.py nvmf_subsystem_remove_listener ${subsys_nqn} -t rdma -a ${local_ip} -s 4420
    echo "Listener of ${subsys_nqn} ${local_ip} removed."
}

function configure_target_fn() {
    rebuild_target_spdk_with_latency_test
    configure_target_env
    get_local_ip
    add_listener
    check_listening_status
}

### run
configure_target_fn