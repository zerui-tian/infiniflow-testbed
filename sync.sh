#!/bin/bash

set -euo pipefail

SRC_DIR="/home/fnil-fdu/infiniflow-testbed"
DST_DIR="~/infiniflow-testbed"

DST_HOSTS=(
  # "B03_tmp2_server2"
  "B03_tmp3_server1"
  "B06-4_server4_ns3_server"
  "B06-3_server5"
)

for dst_host in "${DST_HOSTS[@]}"; do
  echo "Syncing to ${dst_host}:${DST_DIR} ..."
  rsync -az \
    --exclude-from="${SRC_DIR}/.gitignore" \
    "${SRC_DIR}/" \
    "${dst_host}:${DST_DIR}/"
done