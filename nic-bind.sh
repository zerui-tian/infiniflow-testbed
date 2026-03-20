#!/usr/bin/env bash
set -euo pipefail

# ===== 配置区 =====
PCI_DEVICES=(
  "0000:01:00.0"
  "0000:01:00.1"
)

DPDK_DRIVER="vfio-pci"
KERNEL_DRIVER="ixgbe"

# 自动寻找 dpdk-devbind.py
if command -v dpdk-devbind.py >/dev/null 2>&1; then
    DEVBIND="dpdk-devbind.py"
elif [[ -x /usr/share/dpdk/usertools/dpdk-devbind.py ]]; then
    DEVBIND="/usr/share/dpdk/usertools/dpdk-devbind.py"
else
    echo "错误: 找不到 dpdk-devbind.py"
    exit 1
fi

# ===== 函数区 =====
usage() {
    cat <<EOF
用法: sudo $0 <action>

可选 action:
  status    查看网卡绑定状态
  dpdk      绑定两张网卡到 ${DPDK_DRIVER}
  kernel    绑定两张网卡到 ${KERNEL_DRIVER} 并尝试拉起网卡
  unbind    卸载两张网卡当前驱动
  up        仅将已经回到内核驱动的网卡置为 UP

示例:
  sudo $0 status
  sudo $0 dpdk
  sudo $0 kernel
  sudo $0 unbind
  sudo $0 up
EOF
}

require_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "请用 sudo 或 root 执行"
        exit 1
    fi
}

show_status() {
    echo "==== 当前网卡状态 ===="
    "$DEVBIND" --status
}

bind_dpdk() {
    echo "==== 绑定到 ${DPDK_DRIVER} ===="
    modprobe vfio-pci || true
    "$DEVBIND" -b "$DPDK_DRIVER" "${PCI_DEVICES[@]}"
    echo
    show_status
}

bind_kernel() {
    echo "==== 绑定回 ${KERNEL_DRIVER} ===="
    modprobe "$KERNEL_DRIVER" || true
    "$DEVBIND" -b "$KERNEL_DRIVER" "${PCI_DEVICES[@]}"

    echo "==== 尝试将网卡置为 UP ===="
    for dev in "${PCI_DEVICES[@]}"; do
        net_path="/sys/bus/pci/devices/${dev}/net"
        if [[ -d "$net_path" ]]; then
            for ifname in "$net_path"/*; do
                [[ -e "$ifname" ]] || continue
                iface="$(basename "$ifname")"
                echo "拉起接口: $iface"
                ip link set "$iface" up || true
            done
        else
            echo "设备 $dev 当前没有对应内核网卡名"
        fi
    done

    echo
    show_status
    echo
    ip link show || true
}

unbind_all() {
    echo "==== 卸载当前驱动 ===="
    "$DEVBIND" -u "${PCI_DEVICES[@]}"
    echo
    show_status
}

link_up() {
    echo "==== 将已绑定到内核的接口置为 UP ===="
    for dev in "${PCI_DEVICES[@]}"; do
        net_path="/sys/bus/pci/devices/${dev}/net"
        if [[ -d "$net_path" ]]; then
            for ifname in "$net_path"/*; do
                [[ -e "$ifname" ]] || continue
                iface="$(basename "$ifname")"
                echo "拉起接口: $iface"
                ip link set "$iface" up || true
            done
        else
            echo "设备 $dev 当前没有对应内核网卡名，可能仍绑定在 DPDK 驱动上"
        fi
    done
}

# ===== 主逻辑 =====
ACTION="${1:-}"

case "$ACTION" in
    status)
        show_status
        ;;
    dpdk)
        require_root
        bind_dpdk
        ;;
    kernel)
        require_root
        bind_kernel
        ;;
    unbind)
        require_root
        unbind_all
        ;;
    up)
        require_root
        link_up
        ;;
    *)
        usage
        exit 1
        ;;
esac