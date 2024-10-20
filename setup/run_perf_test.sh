#!/usr/bin/env bash
# Author: CS0522
# Run perf or perf_rep test on Host node.
# !!! Run with root !!!
# !!! Run on local machine !!!
# Steps: 
# 1. Read nodes_local_ip.txt file;
# 2. Run perf or perf_rep.

# perf command:
# ./build/bin/spdk_nvme_perf -r 'trtype:rdma adrfam:IPv4 traddr:192.168.246.130 trsvcid:4420' -r 'trtype:rdma adrfam:IPv4 traddr:192.168.246.131 trsvcid:4420' -r 'trtype:rdma adrfam:IPv4 traddr:192.168.246.132 trsvcid:4420' -q 256 -o 4096 -w randwrite -t 5 -P 1 -G -LL -l --transport-stats

# pwd: spdk_dir

set -eu

# help function
function usage() {
    echo "Author: CS0522"
    echo "Run perf or perf_rep test on Host node."
    echo "!!! Run with root !!!"
    echo ""
    echo "You should input as:  <sh_name=run_perf_test.sh> <cloudlab_username> <file_path> "
    echo "sh_name:              bash script name"
    echo "cloudlab_username:    CS0522, or smthing like this"
    echo "hostname:             Host node's hostname, \"ms0805.utah.cloudlab.us\" or smthing like this"
    echo "run_app:              val: < perf | perf_rep >"
    echo "node_num:             the number of nodes (includes Host and Target)"
    echo "file_path:            nodes_local_ip file path"
    echo "io_queue_depth:       io queue depth. number of tasks"
    echo "io_size:              io size in bytes"
    echo "workload:             val: < randrw | randwrite | randread >"
    echo "run_time:             value in seconds"
    echo ""

    echo "help --help -h:       show script usage"
    exit
}

# need for help
if [[ $# -lt 4 ]] || [[ "$1" == "help" ]] || [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    usage
    exit
fi

# store perf params
cloudlab_username=$1
hostname=$2
run_app=$3
node_num=$4
nodes_local_ip_file_path=$5
io_queue_depth=$6
io_size=$7
workload=$8
run_time=$9
transport_ids=""

declare -A nodes_local_ip
# local_ip=""

### check params ###
error_params=0
if [[ "${run_app}" != "perf" ]] && [[ "${run_app}" != "perf_rep" ]]; then
    echo "run_app param is invalid."
    error_params=`expr ${error_params} + 1`
fi
if [[ -z "${io_queue_depth}" ]]; then
    echo "io_queue_depth value is empty."
    error_params=`expr ${error_params} + 1`
fi
if [[ -z "${io_size}" ]]; then
    echo "io_size value is empty."
    error_params=`expr ${error_params} + 1`
fi
if [[ -z "${workload}" ]]; then
    echo "workload value is empty."
    error_params=`expr ${error_params} + 1`
fi
if [[ -z "${run_time}" ]]; then
    echo "run_time value is empty."
    error_params=`expr ${error_params} + 1`
fi
if [ ${error_params} -ne 0 ]; then
    usage
    exit
fi
### end check ######

# read nodes local ip from file
function get_nodes_local_ip() {
    local curr_node=0
    for line in $(cat ${nodes_local_ip_file_path}); do
        # echo ${line}
        nodes_local_ip[${curr_node}]=${line}

        curr_node=`expr ${curr_node} + 1`
    done
    if [ ${node_num} -ne ${curr_node} ]; then
        echo "node_num is not equal to the number of nodes local ip. "
        exit
    fi
}

# get local IP address
# function get_local_ip() {
#     # local_ip=`ifconfig -a | grep inet | grep -v 127.0.0.1 | grep -v inet6 | awk '{print $2}' | tr -d "addr:"`
#     local_ip=`hostname -I | awk '{print $1}'`
#     echo "local_ip=${local_ip}"
# }

### set params
# set transport ids
function set_transport_ids() {
    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        # if IP is host's IP, skip the loop
        # if [[ "${local_ip}" == "${nodes_local_ip[${curr_node}]}" ]]; then
        if [ ${curr_node} -eq 0 ]; then
            curr_node=`expr ${curr_node} + 1`
            continue
        fi
        # target's IP
        transport_ids="${transport_ids} -r 'trtype:rdma adrfam:IPv4 traddr:${nodes_local_ip[${curr_node}]} trsvcid:4420'"

        curr_node=`expr ${curr_node} + 1`
    done
    if [[ -z "${transport_ids}" ]]; then
        echo "transport_ids value is empty."
        exit
    fi
}

# set io_queue_depth
function set_io_queue_depth() {
    io_queue_depth="-q ${io_queue_depth}"
}

# set io_size
function set_io_size() {
    io_size="-o ${io_size}"
}

# set workload
function set_workload() {
    # randrw should add -M
    if [[ "${workload}" == "randrw" ]]; then
        workload="${workload} -M 50"
    fi
    workload="-w ${workload}"
}

# set run_time
function set_run_time() {
    io_size="-t ${run_time}"
}

# set params funtion
function set_params() {
    get_nodes_local_ip
    # get_local_ip
    set_transport_ids
    set_io_queue_depth
    set_io_size
    set_workload
    set_run_time
}

function run_perf() {
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
        sudo su
        cd ${spdk_dir}
        ./build/bin/spdk_nvme_perf ${transport_ids} ${io_queue_depth} ${io_size} ${workload} ${run_time} -P 1 >> ${workspace_dir}/output/perf_output.log
        exit
ENDSSH
}

function run_perf_rep() {
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
        sudo su
        cd ${spdk_dir}
        ./build/bin/spdk_nvme_perf_rep ${transport_ids} ${io_queue_depth} ${io_size} ${workload} ${run_time} -P 1 >> ${workspace_dir}/output/perf_rep_output.log
        exit
ENDSSH
}

function run_perf_test_fn() {
    set_params
    if [[ "${run_app}" == "perf" ]]; then
        run_perf
    else
        run_perf_rep
    fi
}

### run
run_perf_test_fn