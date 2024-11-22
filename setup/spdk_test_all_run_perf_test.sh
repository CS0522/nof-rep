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
    echo "!!! Run on local machine !!!"
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
    echo "host_status:          host status: < 0:The host side only runs the perf program; 1:The host side runs the target and perf program simultaneously; 2:The host side connects to the local PCIe NVMe SSD>"
    echo "send_main_rep_finally:is main_rep the last transmission"
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
host_status=${10}
send_main_rep_finally=${11}
io_limit=${12}
io_num_per_second=${13}
batch=${14}
transport_ids=""
ssh_arg="-o StrictHostKeyChecking=no"
spdk_dir="/opt/Workspace/spdk-24.05.x-host"
workspace_dir="/opt/Workspace"

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
if [[ -z "${host_status}" ]]; then
    host_status=0
fi
if [[ -z "${send_main_rep_finally}" ]]; then
    send_main_rep_finally=0
fi
if [[ -z "${io_limit}" ]]; then
    io_limit=0
fi
if [[ -z "${io_num_per_second}" ]]; then
    io_num_per_second=0
fi
if [[ -z "${batch}" ]]; then
    batch=0
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
    if [ ${node_num} -gt ${curr_node} ]; then
        echo "node_num is greater than the number of nodes local ip. "
        exit
    fi
}

bdf=""
bdf_array=()

function get_bdf() {
    bdf=$(
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
        sudo su
        cd ${spdk_dir}
        ./scripts/setup.sh status 2>&1 | grep NVMe | tail -n 3 | awk '{print \$2}'
ENDSSH
    )
    mapfile -t bdf_array <<< `echo "$bdf" | tail -n 3`
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
        # if IP is host's IP
        if [[ ${host_status} -eq 4 ]]; then
            transport_ids="${transport_ids} -r 'trtype:PCIe traddr:${bdf_array[${curr_node}]}'"
        else
            if [[ ${curr_node} -eq 0 ]]; then
                if [[ ${host_status} -eq 1 ]]; then
                    # 走本地环回, IP 为自身 IP (相当于走网卡) 
                    transport_ids="${transport_ids} -r 'trtype:rdma adrfam:IPv4 traddr:${nodes_local_ip[${curr_node}]} trsvcid:4420'"
                elif [[ ${host_status} -eq 2 ]]; then
                    # 走本地环回, IP 为 127.0.0.1
                    transport_ids="${transport_ids} -r 'trtype:rdma adrfam:IPv4 traddr:127.0.0.1 trsvcid:4420'"
                elif [[ ${host_status} -eq 3 ]]; then
                    # 走 PCIe
                    transport_ids="${transport_ids} -r 'trtype:PCIe traddr:${bdf}'"
                fi
                curr_node=$((curr_node + 1))
                continue
            fi
            # target's IP
            transport_ids="${transport_ids} -r 'trtype:rdma adrfam:IPv4 traddr:${nodes_local_ip[${curr_node}]} trsvcid:4420'"
        fi
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
    run_time="-t ${run_time}"
}

function set_send_main_rep_finally() {
    if [[ ${send_main_rep_finally} -eq 0 ]]; then
        send_main_rep_finally=""
    else
        send_main_rep_finally="-f"
    fi
}

function set_io_limit() {
    if [[ ${io_limit} -eq 0 ]]; then
        io_limit=""
    else
        io_limit="-K ${io_limit}"
    fi
}

function set_io_num_per_second() {
    if [[ ${io_num_per_second} -eq 0 ]]; then
        io_num_per_second=""
    else
        io_num_per_second="-E ${io_num_per_second}"
    fi
}

function set_batch() {
    if [[ ${batch} -eq 0 ]]; then
        batch=""
    else
        batch="-B ${batch}"
    fi
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
    set_send_main_rep_finally
    set_io_limit
    set_io_num_per_second
    set_batch
}

function run_perf() {
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
        sudo su
        cd ${spdk_dir}
        ./build/bin/spdk_nvme_perf ${transport_ids} ${io_queue_depth} ${io_size} ${workload} ${run_time} -P 1 -c 0xc ${io_limit} ${io_num_per_second} ${batch} > ${workspace_dir}/output/perf_output.log
        exit
ENDSSH
}

function run_perf_rep() {
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
        sudo su
        cd ${spdk_dir}
        ./build/bin/spdk_nvme_perf_rep ${transport_ids} ${io_queue_depth} ${io_size} ${workload} ${run_time} -P 1 -c 0xc ${send_main_rep_finally} ${io_limit} ${io_num_per_second} ${batch} > ${workspace_dir}/output/perf_output.log
        exit
ENDSSH
}

function run_perf_test_fn() {
    get_bdf
    set_params
    if [[ "${run_app}" == "perf" ]]; then
        run_perf
    else
        run_perf_rep
    fi
}

### run
run_perf_test_fn
