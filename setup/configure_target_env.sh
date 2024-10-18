#!/usr/bin/env bash
# Author: CS0522
# Configure Target node env
# !!! Run with root !!!
# Steps:
# 1. Start nvmf_tgt;
# 2. create transport;
# 3. attach nvme controller;
# 4. create subsystem;
# 5. add namespace.

function configure_target_env_fn() {
    bdf=`./scripts/setup.sh status 2>&1 | grep NVMe | tail -n 1 | awk '{print $2}'`
	echo "bdf=${bdf}"
}