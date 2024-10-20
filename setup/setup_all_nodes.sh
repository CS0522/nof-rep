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
# 3. Setup RDMA Soft-RoCE / RDMA RoCE;
# 4. Setup SPDK vs.24.05.x with latency_test;
# 5. If is Host, configure host device.
# 6. If is Target, configure this target device;
# 7. Get the node's local ip.

# Assuming that there are (1 + 3) nodes.

# pwd: spdk_dir

set -eu

# myprint enabled/disabled
myprint=1

# OS is supported? 
os=$(uname -s)
if [[ $os != Linux && $os != FreeBSD ]]; then
	echo "not supported platform ($os), exit"
	exit 1
fi

# help function
function usage() {
    echo "Author: CS0522"
    echo "Setup all nodes," 
    echo "including Host node and Target nodes."
    echo "!!! Run with root !!!"
    echo "All scripts are supposed to be executed under spdk root dir."
    echo ""
    echo "You should input as:  <sh_name=setup_all_nodes.sh> <cloudlab_username> <is_mlnx> <node_num> [hostnames...]"
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
        if [ ${hm_num} -eq 0 ]; then
            echo "(Host)    node ${hm_num}: ${hostnames[${hm_num}]}"
        else
            echo "(Target)  node ${hm_num}: ${hostnames[${hm_num}]}"
        fi
        hm_num=`expr ${hm_num} + 1`
    done
}

### args of setup
# dir of remote machine is Absolute Path
ssh_arg="-o StrictHostKeyChecking=no"
workspace_dir="/opt/Workspace"
spdk_repo="https://github.com/spdk/spdk.git"
spdk_version="24.05.x"
spdk_dir="${workspace_dir}/spdk-${spdk_version}"
setup_dir="${spdk_dir}/setup"
latency_test_repo="https://github.com/CS0522/nof-rep.git"
latency_test_branch="latency_test"
# dir of local machine is Relative Path
setup_output_dir="setup_output"
run_output_dir="run_output"

### functions of each setup step
function install_git() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            mkdir -p ${workspace_dir}/output && cd ${workspace_dir}
            apt update
            apt install -y git
            git config --global init.defaultBranch master 
            git config --global user.name "CS0522"
            git config --global user.email "chenshi020522@outlook.com"
            exit
ENDSSH
}

function clone_spdk() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${workspace_dir}
            git clone --branch v${spdk_version} ${spdk_repo} ./spdk-${spdk_version} && cd ./spdk-${spdk_version}
            git submodule update --init
            rm -rf .git && git init
            exit
ENDSSH
}

function clone_latency_test() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            git remote add origin ${latency_test_repo} && git fetch
            git checkout -f -t origin/${latency_test_branch}
            chmod 777 ./setup/*
            exit
ENDSSH
}

function setup_rdma() {
    local hostname=$1
    # is mlnx: RoCE
    # is not mlnx: Soft RoCE
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/setup_rdma.sh ${is_mlnx}
            exit
ENDSSH
}

function setup_spdk_with_latency_test() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/setup_spdk_with_latency_test.sh
            exit
ENDSSH
}

function configure_target() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/configure_target.sh ${is_100g}
            exit
ENDSSH
}

function configure_host() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/configure_host.sh
            exit
ENDSSH
}

# get certain node's local ip and store in an array
# params: 
#   hostname: the hostname
#   node_index: the index of this node, mapping to hostnames
function get_local_ip() {
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
    local curr_node=0
    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[${curr_node}]}
        
        ### SSH connect to each node: 

        # 0. Install and configure git and make directories;
        install_git ${hostname}
        # 1. Clone SPDK vs.24.05.x and re-init a local git repo;
        clone_spdk ${hostname}
        # 2. Clone latency_test code to get setup scripts;
        clone_latency_test ${hostname}
        # 3. Setup RDMA Soft-RoCE / RDMA RoCE;
        setup_rdma ${hostname}
        # 4. Setup SPDK vs.24.05.x with latency_test;
        setup_spdk_with_latency_test ${hostname}
        # 5. If is Host, configure host device;
        if [ ${curr_node} -eq 0 ]; then
            configure_host ${hostname}
        # 6. If is Target, configure this target device;
        else
            configure_target ${hostname}
        fi
        # 7. get the current node's local ip
        get_local_ip ${hostname} ${curr_node}

        curr_node=`expr ${curr_node} + 1`
    done

    # make output dir (local machine)
    mkdir ${setup_output_dir}
    mkdir ${run_output_dir}
    # write local_ip to nodes_local_ip.txt
    curr_node=0
    while (( ${curr_node}<${node_num} )); do
        echo "${nodes_local_ip[${curr_node}]}" >> ./setup/${setup_output_dir}/nodes_local_ip.txt
        
        curr_node=`expr ${curr_node} + 1`
    done
}


### run
output_hostnames
setup_all_nodes_fn

echo "All nodes are successfully set!"