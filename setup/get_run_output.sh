#!/usr/bin/env bash
# Author: CS0522
# Get outputs of Host and Target.
# !!! Run with root !!!
# !!! Run on local machine !!!
# Steps: 
# 1. Get latency logs;
# 2. Get output logs.

# 远程节点的 outputs 都保存在 /opt/Workspace/output 下
# 本地电脑获取的结果保存在 ./setup/setup_output 和 ./setup/run_output 下

# pwd: spdk_dir

set -eu

# help function
function usage() {
    echo "Author: CS0522"
    echo "Get outputs from all nodes."
    echo "!!! Run with root !!!"
    echo "!!! Run on local machine !!!"
    echo "All scripts are supposed to be executed under spdk root dir."
    echo ""
    echo "You should input as:  <sh_name=get_run_output.sh> <cloudlab_username> <node_num> [hostnames...]"
    echo "sh_name:              shell script name"
    echo "cloudlab_username:    CS0522, or smthing like this"
    # echo "run_app:              val: < perf | perf_rep >"
    echo "node_num:             default 1 + 3 = 4, each Target has one NS"
    echo "                      if not 4, it means certain Target has more than one NS"
    echo "hostnames:            \"ms0805.utah.cloudlab.us\" or smthing like this"
    echo "                      the first one must be the host"
    echo "help --help -h:       show script usage"
    exit
}

# need for help
if [[ $# -lt 3 ]] || [[ $# -gt 6 ]] || [[ "$1" == "help" ]] || [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    usage
    exit
fi

# store params
cloudlab_username=$1
# run_app=$2
node_num=$2
# hostnames array
declare -A hostnames

### check params ###
# if [[ "${run_app}" != "perf" ]] && [[ "${run_app}" != "perf_rep" ]]; then
#     echo "run_app param is invalid."
#     usage
#     exit
# fi
# check if hostname_num == node_num
hostname_num=0
# arg_pos moves left 2
# means the previous $3 turns into the current $1
shift 2
for param in "$@"; do
    # store hostnames
    hostnames[${hostname_num}]=${param}
    hostname_num=`expr ${hostname_num} + 1`
done
if [ ${node_num} -ne ${hostname_num} ]; then
    echo "hostname_num is not equal to node_num."
    exit
fi
### end check ####

host_latency_log_file="host_latency_log.csv"
target_latency_log_file="target_latency_log.csv"
perf_output_log="perf_output.log"
perf_rep_output_log="perf_rep_output.log"

ssh_arg="-o StrictHostKeyChecking=no"
# dir of remote machine is Absolute Path
workspace_dir="/opt/Workspace"
spdk_version="24.05.x"
spdk_dir="${workspace_dir}/spdk-${spdk_version}"
# dir of local machine is Relative Path
run_output_dir="run_output"

# xxx_latency_log.csv
# xxx_output.log
function get_outputs() {
    local hostname=$1
    local src_file_name=$2
    local dest_file_name=$3
    # scp [args...] <username>@<hostname>:<remote_path> <local_path>
    scp ${ssh_arg} ${cloudlab_username}@${hostname}:${workspace_dir}/output/${src_file_name} ./setup/${run_output_dir}/${dest_file_name}
}

function get_run_output_fn() {
    # 1. get latency_logs
    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[${curr_node}]}
        # host
        if [ ${curr_node} -eq 0 ]; then
            get_outputs ${hostname} ${host_latency_log_file} ${host_latency_log_file}
        # target
        else
            get_outputs ${hostname} ${target_latency_log_file} "target${curr_node}_latency_log.csv"
        fi

        curr_node=`expr ${curr_node} + 1`
    done
    # 2. get host perf output log
    curr_node=0
    local host=${hostnames[${curr_node}]}
    get_outputs ${host} ${perf_output_log} ${perf_output_log}
    get_outputs ${host} ${perf_rep_output_log} ${perf_rep_output_log}
}

### run
get_run_output_fn