#!/usr/bin/env bash
# Author: CS0522
# Setup all nodes, 
# including Host node and Target nodes
# !!! Run with root !!!
# Steps: 
### SSH connect to each node: 
# 0. Install and configure git and make directories;
# 1. Clone SPDK vs.24.05.x and reinit a local git repo;
# 2. Clone latency_test code to get setup scripts;
# 3. Setup RDMA Soft-RoCE / RDMA RoCE;
# 4. Setup SPDK vs.24.05.x with latency_test;
# 5. If is Target, configure this target device;
# 6. If is Host, configure host device and do a simple perf test.

# Assuming that there are (1 + 3) nodes.

# Args: <sh_name=setup_all_nodes.sh> <cloudlab_username> <is_mlnx> <node_num> [ssh_cmd...]
# sh_name: bash script name;
# cloudlab_username: CS0522, or smthing like this;
# is_mlnx: NIC supports RDMA or not?
# node_num: default 1 + 3 = 4, each Target has one NS.
#           If not, means certain Target has more than one NS;
# ssh_cmd: "ssh CS0522@<domain_name>" or smthing like this;
#           the number of ssh_cmd may be not the same each time.
#
set -eu

# myprint enabled/disabled
myprint=1

# OS is supported? 
os=$(uname -s)
if [[ $os != Linux && $os != FreeBSD ]]; then
	echo "not supported platform ($os), exit"
	exit 1
fi

function usage() {
    echo "Args:                 <sh_name=setup_all_nodes.sh> <cloudlab_username> <is_mlnx> <node_num> [ssh_cmds...]"
    echo "sh_name:              shell script name"
    echo "cloudlab_username:    CS0522, or smthing like this"
    echo "is_mlnx:              NIC supports RDMA or not?"
    echo "node_num:             default 1 + 3 = 4, each Target has one NS"
    echo "                      if not, means certain Target has more than one NS"
    echo "hostnames:            \"ms0805.utah.cloudlab.us\" or smthing like this"
    echo "                      the number of ssh_cmds may be not the same each time"
}

# store args
cloudlab_username=$1
is_mlnx=$2
node_num=$3
declare -A hostnames

### check args ###
if [ $# -lt 4 ] || [ $# -gt 7 ]; then
    usage
    exit
fi
# check if ssh_cmd_num == node_num
ssh_cmd_num=0
# arg_pos moves left 3
# means the previous $4 turns into the current $1
shift 3
for param in "$@"; do
    # store ssh_cmds in hostnames temporarily
    hostnames[${ssh_cmd_num}]=${param}
    ssh_cmd_num=`expr ${ssh_cmd_num} + 1`
done
if [ ${node_num} -ne ${ssh_cmd_num} ]; then
    echo "ssh_cmd_num is not equal to node_num"
    exit
fi
### end check ####

# store ssh_cmds and split them into hostnames
# default the first one is Host, remains are Target
# NO NEED
function split_ssh_cmds_into_hostnames() {
    local hostname_num=0
    while (( ${hostname_num}<${node_num} )); do
        ssh_cmd_splitted=($(echo ${hostnames[${hostname_num}]} | tr "@" "\n"))
        hostnames[${hostname_num}]=${ssh_cmd_splitted[1]}
        hostname_num=`expr ${hostname_num} + 1`
    done
    if [ ${node_num} -ne ${hostname_num} ]; then
        echo "hostname_num is not equal to node_num"
        exit
    fi
}

# output hostname of each node
function output_hostnames() {
    echo "hostnames: "
    local hostname_num=0
    while (( ${hostname_num}<${node_num} )); do
        if [ ${hostname_num} -eq 0 ]; then
            echo "(Host)    node ${hostname_num}: ${hostnames[${hostname_num}]}"
        else
            echo "(Target)  node ${hostname_num}: ${hostnames[${hostname_num}]}"
        fi
        hostname_num=`expr ${hostname_num} + 1`
    done
}

### args of setup
ssh_arg="-o StrictHostKeyChecking=no"
workspace_dir="/opt/Workspace"
spdk_repo="https://github.com/spdk/spdk.git"
spdk_version="24.05.x"
spdk_dir="${workspace_dir}/spdk-${spdk_version}"
setup_dir="${spdk_dir}/setup"
latency_test_repo="https://github.com/CS0522/nof-rep.git"
latency_test_branch="latency_test"

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

# TODO 需要返回该 Target IP
function configure_target() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/configure_target.sh
            exit
ENDSSH
}

# TODO 需要返回该 Host IP
function configure_host() {
    local hostname=$1
    ssh ${ssh_arg} ${cloudlab_username}@${hostname} << ENDSSH
            sudo su
            cd ${spdk_dir}
            ${setup_dir}/configure_host.sh
            exit
ENDSSH
}

### setup function
function setup_all_nodes_fn() 
{
    local curr_node=0

    while (( ${curr_node}<${node_num} )); do
        local hostname=${hostnames[$curr_node]}
        
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
        # 5. If is Target, configure this target device;
        # configure_target ${hostname}
        # 6. If is Host, configure host device and do a simple perf test.
        # configure_host ${hostname}
        
        curr_node=`expr ${curr_node} + 1`
    done
}


### run
# split_ssh_cmds_into_hostnames
output_hostnames
setup_all_nodes_fn

echo "All nodes are successfully set!"