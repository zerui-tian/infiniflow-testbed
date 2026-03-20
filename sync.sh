#!/bin/bash

set -euo pipefail

SRC_DIR="/home/fnil-fdu/infiniflow-testbed"
DST_HOST="user@172.22.4.182"
DST_DIR="/home/user/infiniflow-testbed"

rsync -az \
  --exclude-from="${SRC_DIR}/.gitignore" \
  "${SRC_DIR}/" \
  "${DST_HOST}:${DST_DIR}/"