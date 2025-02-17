#!/usr/bin/env bash
# Author: CS0522
# Setup all nodes, 
# including Host node and Target nodes.
# !!! Run with root !!!
# !!! Run on local machine !!!
# All scripts are supposed to be executed under spdk root dir.
# Steps: 
### SSH connect to each node: 
# 0. Install and configure git and make directories;
# 1. Clone SPDK vs.24.05.x and reinit a local git repo;
# 2. Clone latency_test code to get setup scripts;
# 3. Setup RDMA;
# 4. Setup SPDK vs.24.05.x with latency_test;
# 5. If is Host, configure host device.
# 6. If is Target, configure this target device;
# 7. Get the node's local ip.

# Assuming that there are (1 + 3) nodes.

# pwd: spdk_dir

set -eu

# skip verify the original spdk and its unittest
skip_verify_original_spdk_and_unit_test=1

# OS is supported? 
os=$(uname -s)
if [[ $os != Linux && $os != FreeBSD && $os != Darwin ]]; then
	echo "not supported platform ($os), exit"
	exit 1
fi

# help function
function usage() {
    echo "Author: CS0522"
    echo "Setup all nodes," 
    echo "including Host node and Target nodes."
    echo "!!! Run with root !!!"
    echo "!!! Run on local machine !!!"
    echo "All scripts are supposed to be executed under spdk root dir."
    echo ""
    echo "You should input as:  <sh_name=setup_all_nodes.sh> <is_mlnx> <is_100g> <cloudlab_username> <node_num> [hostnames...]"
    echo "sh_name:              shell script name"
    echo "is_mlnx:              NIC supports RDMA or not?"
    echo "is_100g:              100 Gbps or normal?"
    echo "cloudlab_username:    CS0522, or smthing like this"
    echo "node_num:             default 1 + 3 = 4, each Target has one NS"
    echo "                      if not 4, it means certain Target has more than one NS"
    echo "hostnames:            \"ms0805.utah.cloudlab.us\" or smthing like this"
    echo "                      the first one must be the host"
    echo "help --help -h:       show script usage"
    exit
}

# need for help
if [[ $# -lt 5 ]] || [[ $# -gt 8 ]] || [[ "$1" == "help" ]] || [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    usage
    exit
fi

# store params
is_mlnx=$1
is_100g=$2
cloudlab_username=$3
node_num=$4
# hostnames array
declare -A hostnames
# host local ip array, index mapping to hostname
declare -A nodes_local_ip

### check params ###
# check if hostname_num == node_num
hostname_num=0
# arg_pos moves left 4
# means the previous $5 turns into the current $1
shift 4
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

# store ssh_cmds and split them into hostnames
# default the first one is Host, remains are Target
# NO NEED
function split_ssh_cmds_into_hostnames() {
    local hm_num=0
    while (( ${hm_num}<${node_num} )); do
        ssh_cmd_splitted=($(echo ${hostnames[${hm_num}]} | tr "@" "\n"))
        hostnames[${hm_num}]=${ssh_cmd_splitted[1]}
        hm_num=`expr ${hm_num} + 1`
    done
    if [ ${node_num} -ne ${hm_num} ]; then
        echo "hostname_num is not equal to node_num"
        exit
    fi
}

# output hostname of each node
function output_hostnames() {
    echo "hostnames: "
    local hm_num=0
    while (( ${hm_num}<${node_num} )); do
        if [[ ${hm_num} -eq 0 ]] && [[ ${node_num} -ne 1 ]]; then
            echo "(Host)    node ${hm_num}: ${hostnames[${hm_num}]}"
        else
            echo "(Target)  node ${hm_num}: ${hostnames[${hm_num}]}"
        fi
        hm_num=`expr ${hm_num} + 1`
    done
}

### args of setup
ssh_arg="-o StrictHostKeyChecking=no"
spdk_repo="https://github.com/spdk/spdk.git"
spdk_version="24.05.x"
# dir of remote machine is Absolute Path
workspace_dir="/opt/Workspace"
host_spdk_dir="${workspace_dir}/spdk-${spdk_version}-host"
target_spdk_dir="${workspace_dir}/spdk-${spdk_version}-target"
host_setup_dir="${host_spdk_dir}/setup"
target_setup_dir="${target_spdk_dir}/setup"
latency_test_repo="https://github.com/CS0522/nof-rep.git"
latency_test_branch="latency_test"
# dir of local machine is Relative Path
setup_output_dir="setup_output"
run_output_dir="run_output"

# expriment configs
WARM_UP_TIME=300
TEST_TIME=180     
MIN_IOSIZE=4096    # 8K
MAX_IOSIZE=262144  # 256K
IO_QUEUE_SIZE_MIN=1
IO_QUEUE_SIZE_MAX=32
is_reverse=0
is_sequence=1
just_get_log=0
IO_NUM_PER_SECOND_MIN=100
IO_NUM_PER_SECOND_MAX=1000
BATCH_MIN=1
BATCH_MAX=32

### functions of each setup step
function install_git() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            mkdir -p ${workspace_dir}/output && cd ${workspace_dir}
            apt update
            apt install -y git pkg-config
            git config --global init.defaultBranch master 
            git config --global user.name "CS0522"
            git config --global user.email "chenshi020522@outlook.com"
            exit
ENDSSH
}

function clone_spdk() {
    local hostname=$1
    local not_host=$2
    if [[ ${not_host} -eq 0 ]]; then
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${workspace_dir}
                git clone --branch v${spdk_version} ${spdk_repo} ./spdk-${spdk_version}-host && cd ./spdk-${spdk_version}-host
                git submodule update --init
                rm -rf ./.git && git init
                cd ${workspace_dir}
                git clone --branch v${spdk_version} ${spdk_repo} ./spdk-${spdk_version}-target && cd ./spdk-${spdk_version}-target
                git submodule update --init
                rm -rf ./.git && git init
                exit
ENDSSH
    else
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${workspace_dir}
                git clone --branch v${spdk_version} ${spdk_repo} ./spdk-${spdk_version}-target && cd ./spdk-${spdk_version}-target
                git submodule update --init
                rm -rf ./.git && git init
                exit
ENDSSH
    fi
# use my file
#     ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
#         sudo su
#         cd ${workspace_dir}
#         wget https://codeload.github.com/spdk/spdk/zip/refs/heads/v${spdk_version} -O ./spdk-${spdk_version}.zip
#         unzip -d ./ spdk-${spdk_version}.zip
#         cd ./spdk-${spdk_version}
#         git init
#         git remote add origin ${spdk_repo}
#         git fetch
#         git checkout -f -t origin/v${spdk_version}
#         git submodule update --init
#         rm -rf ./.git && git init
#         exit
# ENDSSH
}

function clone_latency_test() {
    local hostname=$1
    local not_host=$2
    if [[ ${not_host} -eq 0 ]]; then
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${host_spdk_dir}
                git remote add origin ${latency_test_repo} && git fetch
                git checkout -f -t origin/${latency_test_branch}
                chmod 777 ${host_setup_dir}/*
                cd ${target_spdk_dir}
                git remote add origin ${latency_test_repo} && git fetch
                git checkout -f -t origin/${latency_test_branch}
                chmod 777 ${target_setup_dir}/*
                exit
ENDSSH
    else
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${target_spdk_dir}
                git remote add origin ${latency_test_repo} && git fetch
                git checkout -f -t origin/${latency_test_branch}
                chmod 777 ${target_setup_dir}/*
                exit
ENDSSH
    fi
}

function setup_rdma() {
    local hostname=$1
    # is mlnx: check RDMA is up?
    # is 100g: 100 Gbps or normal?
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${target_spdk_dir}
            ${target_setup_dir}/setup_rdma.sh ${is_mlnx} ${is_100g}
            exit
ENDSSH
}

function setup_spdk_with_latency_test() {
    local hostname=$1
    local not_host=$2
    if [[ ${not_host} -eq 0 ]]; then
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${host_spdk_dir}
                ${host_setup_dir}/setup_spdk_with_latency_test.sh ${skip_verify_original_spdk_and_unit_test}
                cd ${target_spdk_dir}
                ${target_setup_dir}/setup_spdk_with_latency_test.sh ${skip_verify_original_spdk_and_unit_test}
                exit
ENDSSH
    else
        ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
                sudo su
                cd ${target_spdk_dir}
                ${target_setup_dir}/setup_spdk_with_latency_test.sh ${skip_verify_original_spdk_and_unit_test}
                exit
ENDSSH
    fi
}

function configure_target() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${target_spdk_dir}
            ${target_setup_dir}/spdk_test_all_configure_target.sh
            exit
ENDSSH
}

function configure_host() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${host_spdk_dir}
            ${host_setup_dir}/configure_host.sh
            exit
ENDSSH
}

function configure_log_off(){
    local hostname=$1
    local spdk_dir=$2
    local setup_dir=$3
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/spdk_test_all_configure_log_off.sh
            exit
ENDSSH
}

# get certain node's local ip and store in an array
# params: 
#   hostname: the hostname
#   node_index: the index of this node, mapping to hostnames
function get_node_local_ip() {
    local hostname=$1
    local node_index=$2
    local mtu="mtu 1500"
    # if is 100 gbps, then mtu = 9000
    if [ ${is_100g} -eq 1 ]; then
        mtu="mtu 9000"
    fi
    local res=$(
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            ifconfig | grep -A 1 '${mtu}' | grep 'inet' | awk '{print \$2}'
ENDSSH
    )
    local local_ip=`echo ${res} | awk '{print $NF}'`
    echo "Local ip of ${hostname}: ${local_ip}"
    # store in an array
    nodes_local_ip[${node_index}]=${local_ip}
    echo "Stored into nodes_local_ip[${node_index}]"
}

### setup function
function setup_all_nodes_fn() 
{
    # make output dir on local machine
    mkdir -p ./setup/${setup_output_dir}
    mkdir -p ./setup/${run_output_dir}

    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[${curr_node}]}
        
        ### SSH connect to each node: 

        # 0. Install and configure git and make directories;
        install_git ${hostname}
        # 1. Clone SPDK vs.24.05.x and re-init a local git repo;
        clone_spdk ${hostname} ${curr_node}
        # 2. Clone latency_test code to get setup scripts;
        clone_latency_test ${hostname} ${curr_node}
        # 3. Setup RDMA;
        setup_rdma ${hostname}
        # 4. Setup SPDK vs.24.05.x with latency_test;
        setup_spdk_with_latency_test ${hostname} ${curr_node}

        get_node_local_ip ${hostname} ${curr_node}

        curr_node=`expr ${curr_node} + 1`
    done

    # write local_ip to nodes_local_ip.txt
    # first delete the file if already exists
    sudo rm -rf ./setup/${setup_output_dir}/nodes_local_ip.txt
    curr_node=0
    while (( ${curr_node}<${node_num} )); do
        echo "${nodes_local_ip[${curr_node}]}" >> ./setup/${setup_output_dir}/nodes_local_ip.txt
        curr_node=`expr ${curr_node} + 1`
    done
}

function run_target(){
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${target_spdk_dir}
            ./setup/configure_target_dev.sh ${is_100g}
            exit
ENDSSH
}

function run_target_127(){
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${target_spdk_dir}
            ./setup/configure_target_dev.sh ${is_100g} 1
            exit
ENDSSH
}

function shutdown_target(){
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${target_spdk_dir}
            ./scripts/rpc.py spdk_kill_instance SIGINT
            exit
ENDSSH
}

function configure_all_nodes_without_log(){
    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[${curr_node}]}
        if [ ${curr_node} -eq 0 ]; then
            configure_log_off ${hostname} ${host_spdk_dir} ${host_setup_dir}
            configure_log_off ${hostname} ${target_spdk_dir} ${target_setup_dir}
        else
            configure_log_off ${hostname} ${target_spdk_dir} ${target_setup_dir}
        fi
        curr_node=`expr ${curr_node} + 1`
    done
    just_get_log=1
}

function configure_all_nodes_wtih_log(){
    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[${curr_node}]}
        if [ ${curr_node} -eq 0 ]; then
            configure_host ${hostname}
            configure_target ${hostname}
        else
            configure_target ${hostname}
        fi
        curr_node=`expr ${curr_node} + 1`
    done
    just_get_log=0
}

function warm_up(){
    local io_size=${MAX_IOSIZE}
    local node_num=4
    local host_status=3 # by pcie
    local hostname=${hostnames[0]}
    local targetname1=${hostnames[1]}
    local targetname2=${hostnames[2]}
    local targetname3=${hostnames[3]}
    run_target ${targetname1}
    run_target ${targetname2}
    run_target ${targetname3}
    ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt 256 ${io_size} write ${WARM_UP_TIME} ${host_status} 0
    shutdown_target ${targetname1}
    shutdown_target ${targetname2}
    shutdown_target ${targetname3}
    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} warm_up/write-io_size_${io_size} ${hostname} ${targetname1} ${targetname2} ${targetname3}
}

function test_single_remote_target(){
    local rw_type_list=("randread" "randwrite")
    #local rw_type_list=("randwrite")
    local node_num=2
    local host_status=0
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        for rw_type in "${rw_type_list[@]}"; do
            local io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname=${hostnames[1]}
                run_target ${targetname}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} ${rw_type} ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} single-remote_target/${rw_type}-io_size_${io_size}-io_queue_size_${io_queue_size} ${hostname} ${targetname}
                io_size=`expr ${io_size} \* 2`
            done
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_single_local_target(){
    local rw_type_list=("randread" "randwrite")
    #local rw_type_list=("randwrite")
    local node_num=1
    local host_status=1
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        for rw_type in "${rw_type_list[@]}"; do
            local io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname=${hostnames[0]}
                run_target ${targetname}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} ${rw_type} ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} single-local_target/${rw_type}-io_size_${io_size}-io_queue_size_${io_queue_size} ${hostname}
                io_size=`expr ${io_size} \* 2`
            done
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_single_local_target_127(){
    # local rw_type_list=("randread" "randwrite")
    local rw_type_list=("randwrite")
    local node_num=1
    local host_status=2
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        for rw_type in "${rw_type_list[@]}"; do
            local io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname=${hostnames[0]}
                run_target_127 ${targetname}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} ${rw_type} ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} single-local_target_127/${rw_type}-io_size_${io_size}-io_queue_size_${io_queue_size} ${hostname}
                io_size=`expr ${io_size} \* 2`
            done
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_single_local_pcie(){
    local rw_type_list=("randread" "randwrite")
    #local rw_type_list=("randwrite")
    local node_num=1
    local host_status=3
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        for rw_type in "${rw_type_list[@]}"; do
            local io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} ${rw_type} ${TEST_TIME} ${host_status} 0
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} single-local_pcie/${rw_type}-io_size_${io_size}-io_queue_size_${io_queue_size} ${hostname}
                io_size=`expr ${io_size} \* 2`
            done
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_rep_remote_target(){
    local node_num=4
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[1]}
            local targetname2=${hostnames[2]}
            local targetname3=${hostnames[3]}
            run_target ${targetname1}
            run_target ${targetname2}
            run_target ${targetname3}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            shutdown_target ${targetname3}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-remote_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
            io_size=`expr ${io_size} \* 2`
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_rep_local_target(){
    local node_num=3
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_rep_local_target_127(){
    local node_num=3
    local host_status=2
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target_127/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target_127/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_rep_local_pcie(){
    local node_num=3
    local host_status=3
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_pcie/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_pcie/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_different_remote_target(){
    local node_num=4
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[1]}
            local targetname2=${hostnames[2]}
            local targetname3=${hostnames[3]}
            run_target ${targetname1}
            run_target ${targetname2}
            run_target ${targetname3}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            shutdown_target ${targetname3}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-remote_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
            io_size=`expr ${io_size} \* 2`
        done
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_different_local_target(){
    local node_num=3
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_different_local_target_127(){
    local node_num=3
    local host_status=2
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target_127/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target_127/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_different_local_pcie(){
    local node_num=3
    local host_status=3
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        if [ ${is_sequence} -eq 1 ]; then
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_pcie/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        if [ ${is_reverse} -eq 1 ]; then
            io_size=${MIN_IOSIZE}
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 1
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_pcie/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-reverse ${hostname} ${targetname2} ${targetname3}
                io_size=`expr ${io_size} \* 2`
            done
        fi
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_local_three_pcie(){
    local node_num=3
    local host_status=4
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} 1 ${host_status} ${just_get_log} three_rep-local_pcie/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname}
            io_size=`expr ${io_size} \* 2`
        done
        io_size=${MIN_IOSIZE}
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_two_rep_remote_target(){
    local node_num=3
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[1]}
            local targetname2=${hostnames[2]}
            run_target ${targetname1}
            run_target ${targetname2}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 2
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} two_rep-remote_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname1} ${targetname2}
            io_size=`expr ${io_size} \* 2`
        done
        io_size=${MIN_IOSIZE}
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_two_rep_local_target(){
    local node_num=2
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[0]}
            local targetname2=${hostnames[1]}
            run_target ${targetname1}
            run_target ${targetname2}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 2
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} two_rep-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2}
            io_size=`expr ${io_size} \* 2`
        done
        io_size=${MIN_IOSIZE}
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_two_different_remote_target(){
    local node_num=3
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[1]}
            local targetname2=${hostnames[2]}
            run_target ${targetname1}
            run_target ${targetname2}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} two_different-remote-target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname1} ${targetname2}
            io_size=`expr ${io_size} \* 2`
        done
        io_size=${MIN_IOSIZE}
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_two_different_local_target(){
    local node_num=2
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local io_queue_size=${IO_QUEUE_SIZE_MIN}
    while (( ${io_queue_size}<=${IO_QUEUE_SIZE_MAX} )); do
        while (( ${io_size}<=${MAX_IOSIZE} )); do
            local hostname=${hostnames[0]}
            local targetname1=${hostnames[0]}
            local targetname2=${hostnames[1]}
            run_target ${targetname1}
            run_target ${targetname2}
            ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt ${io_queue_size} ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10
            shutdown_target ${targetname1}
            shutdown_target ${targetname2}
            ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} two_different-local_target/randwrite-io_size_${io_size}-io_queue_size_${io_queue_size}-sequence ${hostname} ${targetname2}
            io_size=`expr ${io_size} \* 2`
        done
        io_size=${MIN_IOSIZE}
        io_queue_size=`expr ${io_queue_size} \* 2`
    done
}

function test_rep_remote_target_iops(){
    local node_num=4
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local batch_size=${BATCH_MIN}
    local io_num_per_second=${IO_NUM_PER_SECOND_MIN}
    while (( ${io_num_per_second}<=${IO_NUM_PER_SECOND_MAX} )); do
        while (( ${batch_size}<=${BATCH_MAX} )); do
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[1]}
                local targetname2=${hostnames[2]}
                local targetname3=${hostnames[3]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt 256 ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 3 ${io_num_per_second} ${batch_size}
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                if [ ${just_get_log} -eq 1 ]; then
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-remote_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-without_log-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
                else
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-remote_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-with_log-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
                fi
                io_size=`expr ${io_size} \* 4`
            done
            io_size=${MIN_IOSIZE}
            batch_size=`expr ${batch_size} \* 2`
        done
        batch_size=${BATCH_MIN}
        io_num_per_second=`expr ${io_num_per_second} \* 2 + 200`
    done
}

function test_rep_local_target_iops(){
    local node_num=3
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local batch_size=${BATCH_MIN}
    local io_num_per_second=${IO_NUM_PER_SECOND_MIN}
    while (( ${io_num_per_second}<=${IO_NUM_PER_SECOND_MAX} )); do
        while (( ${batch_size}<=${BATCH_MAX} )); do
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_rep_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt 256 ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 3 ${io_num_per_second} ${batch_size}
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                if [ ${just_get_log} -eq 1 ]; then
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-without_log-sequence ${hostname} ${targetname2} ${targetname3}
                else
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} rep-local_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-with_log-sequence ${hostname} ${targetname2} ${targetname3}
                fi
                io_size=`expr ${io_size} \* 4`
            done
            io_size=${MIN_IOSIZE}
            batch_size=`expr ${batch_size} \* 2`
        done
        batch_size=${BATCH_MIN}
        io_num_per_second=`expr ${io_num_per_second} \* 2 + 200`
    done
}

function test_different_remote_target_iops(){
    local node_num=4
    local host_status=0
    local io_size=${MIN_IOSIZE}
    local batch_size=${BATCH_MIN}
    local io_num_per_second=${IO_NUM_PER_SECOND_MIN}
    while (( ${io_num_per_second}<=${IO_NUM_PER_SECOND_MAX} )); do
        while (( ${batch_size}<=${BATCH_MAX} )); do
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[1]}
                local targetname2=${hostnames[2]}
                local targetname3=${hostnames[3]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt 256 ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 3 ${io_num_per_second} ${batch_size}
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                if [ ${just_get_log} -eq 1 ]; then
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-remote_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second-${io_num_per_second}-without_log-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
                else
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-remote_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second-${io_num_per_second}-with_log-sequence ${hostname} ${targetname1} ${targetname2} ${targetname3}
                fi
                io_size=`expr ${io_size} \* 4`
            done
            io_size=${MIN_IOSIZE}
            batch_size=`expr ${batch_size} \* 2`
        done
        batch_size=${BATCH_MIN}
        io_num_per_second=`expr ${io_num_per_second} \* 2 + 200`
    done
}

function test_different_local_target_iops(){
    local node_num=3
    local host_status=1
    local io_size=${MIN_IOSIZE}
    local batch_size=${BATCH_MIN}
    local io_num_per_second=${IO_NUM_PER_SECOND_MIN}
    while (( ${io_num_per_second}<=${IO_NUM_PER_SECOND_MAX} )); do
        while (( ${batch_size}<=${BATCH_MAX} )); do
            while (( ${io_size}<=${MAX_IOSIZE} )); do
                local hostname=${hostnames[0]}
                local targetname1=${hostnames[0]}
                local targetname2=${hostnames[1]}
                local targetname3=${hostnames[2]}
                run_target ${targetname1}
                run_target ${targetname2}
                run_target ${targetname3}
                ./setup/spdk_test_all_run_perf_test.sh ${cloudlab_username} ${hostname} perf_batch ${node_num} ./setup/setup_output/nodes_local_ip.txt 256 ${io_size} randwrite ${TEST_TIME} ${host_status} 0 10 3 ${io_num_per_second} ${batch_size}
                shutdown_target ${targetname1}
                shutdown_target ${targetname2}
                shutdown_target ${targetname3}
                if [ ${just_get_log} -eq 1 ]; then
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-without_log-sequence ${hostname} ${targetname2} ${targetname3}
                else
                    ./setup/spdk_test_all_get_run_output.sh ${cloudlab_username} ${node_num} ${host_status} ${just_get_log} different-local_target-iops/randwrite-io_size_${io_size}-batch_size_${batch_size}-io_num_per_second_${io_num_per_second}-with_log-sequence ${hostname} ${targetname2} ${targetname3}
                fi
                io_size=`expr ${io_size} \* 4`
            done
            io_size=${MIN_IOSIZE}
            batch_size=`expr ${batch_size} \* 2`
        done
        batch_size=${BATCH_MIN}
        io_num_per_second=`expr ${io_num_per_second} \* 2 + 200`
    done
}

function shutdown_all_target(){
    local target_num=0
    while (( ${target_num}<${node_num} )); do
        local hostname=${hostnames[${target_num}]}
        shutdown_target ${hostname}
        target_num=`expr ${target_num} + 1`
    done
}

#if [ -d "./setup/${setup_output_dir}" ]; then
#    rm -rf "./setup/${setup_output_dir}"/*
#fi

#if [ -d "./setup/${run_output_dir}" ]; then
#    rm -rf "./setup/${run_output_dir}"/*
#fi


### run
output_hostnames
setup_all_nodes_fn
echo "All nodes are successfully set!"

#configure_all_nodes_without_log

#shutdown_all_target

#configure_all_nodes_wtih_log

# 预热, 使盘进入 GC
#warm_up

# 单盘的性能测试
#test_single_remote_target

#test_single_local_target

#test_single_local_target_127

#test_single_local_pcie

#configure_all_nodes_wtih_log

# 三副本性能测试
#test_rep_remote_target

#test_rep_local_target

# test_rep_local_target_127

#test_rep_local_pcie

# 非三副本的性能测试
#test_different_remote_target

#test_different_local_target

# test_different_local_target_127

#test_different_local_pcie

#test_local_three_pcie

#configure_all_nodes_wtih_log

#test_two_rep_remote_target
#test_two_rep_local_target
#test_two_different_remote_target
#test_two_different_local_target

configure_all_nodes_wtih_log
test_rep_remote_target_iops
test_rep_local_target_iops
test_different_remote_target_iops
test_different_local_target_iops

configure_all_nodes_without_log
test_rep_remote_target_iops
test_rep_local_target_iops
test_different_remote_target_iops
test_different_local_target_iops