#!/usr/bin/env bash
# Author: CS0522
# Setup RDMA RoCE/Soft RoCE
# !!! Run with root !!!
# Steps:
# 1. Install tools;
# 2. Setup RDMA.

function install_tools() {
    # system tools
    apt-get install -y vim net-tools nvme-cli fio
    # rdma user space tools
    apt-get install -y libibverbs1 ibverbs-utils librdmacm1 libibumad3 ibverbs-providers rdma-core
    # rdma test tool
    apt-get install -y perftest
}

# get local IP address
function get_local_ip() {
    # local_ip=`ifconfig -a | grep inet | grep -v 127.0.0.1 | grep -v inet6 | awk '{print $2}' | tr -d "addr:"`
    local_ip=`hostname -I | awk '{print $1}'`
    echo local_ip="${local_ip}"
}

# get device name
function get_net_dev() {
    net_dev=`ifconfig | grep -w BROADCAST | awk '{print $1}' | sed 's/://g'`
    echo net_dev="${net_dev}"
}

# modprobe RXE NIC
function modprobe_rxe() {
    modprobe rdma_rxe
    rdma link add rxe_0 type rxe netdev ${net_dev}
}

# modprobe nvme
function modprobe_nvme() {
    modprobe nvmet
    modprobe nvmet-rdma
    modprobe nvme-rdma
}

# setup RoCE
# function setup_roce() {

# }

# setup Soft RoCE
function setup_soft_roce() {
    modprobe_rxe
    modprobe_nvme
}

# check RDMA status
function check_rdma_status() {
    rdma link
    local state=`rdma link | awk 'print{$ 4}'`
    local physical_state=`rdma link | awk 'print{$ 6}'`
    local rdma_net_dev=`rdma link | awk 'print{$ 8}'`
    # valid
    if [ "ACTIVE" = ${state} ] && [ "LINK_UP" = ${physical_state} ] && [ ${net_dev} = ${rdma_net_dev} ]; then
        echo "Setup RDMA Soft RoCE succeeded. "
    else
        echo "Setup RDMA Soft RoCE failed. "
    fi
}

function usage() {
    echo "Args:                 <sh_name=setup_rdma.sh> <is_mlnx>"
    echo "sh_name:              shell script name"
    echo "is_mlnx:              0: NIC does not support RDMA; 1: NIC supports RDMA"
}

### check args ###
if [ $# -ne 1 ]; then
    usage
    exit
fi

# setup function
is_mlnx=$1
function setup_rdma_fn() {
    # install tools
    install_tools
    # get local ip and net device name
    get_local_ip
    get_net_dev
    # setup Soft RoCE
    if [ ${is_mlnx} -eq 0 ]; then
        setup_soft_roce
    # setup RoCE
    else
        setup_roce
    fi
    # check RDMA env
    check_rdma_status
}

### run
setup_rdma_fn


### for SPDK,
### the following will not be executed
:<<!
# 1. config nvme subsystem
subsys_name="nvme-rdma-test"
echo subsys_name="${subsys_name}"
mkdir /sys/kernel/config/nvmet/subsystems/"${subsys_name}"
cd /sys/kernel/config/nvmet/subsystems/"${subsys_name}"

# 2. allow any host to be connected to this target
echo 1 > attr_allow_any_host

# 3. create a namespace，example: nsid=10
nsid=10
echo nsid="${nsid}"
mkdir namespaces/"${nsid}"
cd namespaces/"${nsid}"

# 4. set the path to the NVMe device
echo -n /dev/nvme0n1> device_path
echo 1 > enable

# 5. create the following directory with an NVMe port
portid=1
echo portid="${portid}"
mkdir /sys/kernel/config/nvmet/ports/"${portid}"
cd /sys/kernel/config/nvmet/ports/"${portid}"

# 6. set ip address to traddr
echo "${local_ip}" > addr_traddr

# 7. set rdma as a transport type，addr_trsvcid is unique.
echo rdma > addr_trtype
echo 4420 > addr_trsvcid

# 8. set ipv4 as the Address family
echo ipv4 > addr_adrfam

# 9. create a soft link
ln -s /sys/kernel/config/nvmet/subsystems/"${subsys_name}" /sys/kernel/config/nvmet/ports/"${portid}"/subsystems/"${subsys_name}"

# 10. Check dmesg to make sure that the NVMe target is listening on the port
dmesg -T | grep "enabling port"

# 11. output info < ip/port>
# ... nvmet_rdma: enabling port 1 (192.168.225.131:4420)

# 12. check the status of nvme
lsblk | grep nvme
# nvme0n1
!