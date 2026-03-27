#!/bin/bash
#
# DPDK 环境初始化脚本
# 
# 功能:
#   1. 配置 hugepages
#   2. 挂载 hugepages 文件系统
#   3. 加载必要的内核模块
#   4. 绑定网卡到 DPDK 驱动
#
# 使用方法:
#   sudo ./setup_dpdk_env.sh [hugepage_size] [num_pages]
#
# 参数:
#   hugepage_size  - hugepage 大小 (2M 或 1G)，默认 2M
#   num_pages      - hugepage 数量，默认 1024 (对于2M) 或 4 (对于1G)
#
# 示例:
#   sudo ./setup_dpdk_env.sh         # 使用默认配置
#   sudo ./setup_dpdk_env.sh 2M 2048 # 配置 2048 个 2MB hugepages
#   sudo ./setup_dpdk_env.sh 1G 8    # 配置 8 个 1GB hugepages
#

set -e  # 遇到错误立即退出

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印函数
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查是否以 root 权限运行
check_root() {
    if [ "$EUID" -ne 0 ]; then 
        print_error "请使用 sudo 运行此脚本"
        exit 1
    fi
}

# 检查 root 权限
check_root

# 解析参数
HUGEPAGE_SIZE=${1:-"2M"}
if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    NUM_PAGES=${2:-1024}  # 2GB
elif [ "$HUGEPAGE_SIZE" = "1G" ]; then
    NUM_PAGES=${2:-4}     # 4GB
else
    print_error "不支持的 hugepage 大小: $HUGEPAGE_SIZE (仅支持 2M 或 1G)"
    exit 1
fi

# 打印配置信息
echo "========================================"
echo "DPDK 环境初始化脚本"
echo "========================================"
print_info "Hugepage 大小: $HUGEPAGE_SIZE"
print_info "Hugepage 数量: $NUM_PAGES"
if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    TOTAL_MEM=$((NUM_PAGES * 2))
    print_info "总内存: ${TOTAL_MEM}MB"
else
    TOTAL_MEM=$NUM_PAGES
    print_info "总内存: ${TOTAL_MEM}GB"
fi
echo "========================================"
echo

# 步骤 1: 加载必要的内核模块
print_info "步骤 1: 加载内核模块..."


# 加载 vfio-pci 模块
if ! lsmod | grep -q vfio_pci; then
    print_info "加载 vfio-pci 模块..."
    modprobe vfio-pci || print_warning "无法加载 vfio-pci 模块"
else
    print_success "vfio-pci 模块已加载"
fi


echo

# 步骤 2: 配置 hugepages
print_info "步骤 2: 配置 hugepages..."

if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    # 配置 2MB hugepages (在所有 NUMA 节点上)
    print_info "配置 2MB hugepages..."
    
    # 获取 NUMA 节点数量
    NUM_NUMA_NODES=$(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l)
    if [ $NUM_NUMA_NODES -eq 0 ]; then
        NUM_NUMA_NODES=1
    fi
    
    print_info "检测到 $NUM_NUMA_NODES 个 NUMA 节点"
    
    # 计算每个 NUMA 节点的页数
    PAGES_PER_NODE=$((NUM_PAGES / NUM_NUMA_NODES))
    
    # 在每个 NUMA 节点上配置 hugepages
    for node in $(seq 0 $((NUM_NUMA_NODES - 1))); do
        HUGEPAGE_FILE="/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB/nr_hugepages"
        if [ -f "$HUGEPAGE_FILE" ]; then
            echo $PAGES_PER_NODE > $HUGEPAGE_FILE
            ACTUAL_PAGES=$(cat $HUGEPAGE_FILE)
            print_success "NUMA 节点 $node: 配置了 $ACTUAL_PAGES 个 2MB hugepages"
        fi
    done
    
    # 全局配置
    echo $NUM_PAGES > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    
elif [ "$HUGEPAGE_SIZE" = "1G" ]; then
    # 配置 1GB hugepages
    print_info "配置 1GB hugepages..."
    
    # 注意: 1GB hugepages 必须在启动时通过内核参数预留
    # 这里尝试动态配置，但可能失败
    if [ -d "/sys/kernel/mm/hugepages/hugepages-1048576kB" ]; then
        echo $NUM_PAGES > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
        ACTUAL_PAGES=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages)
        
        if [ $ACTUAL_PAGES -lt $NUM_PAGES ]; then
            print_warning "只配置了 $ACTUAL_PAGES 个 1GB hugepages (请求 $NUM_PAGES 个)"
            print_warning "1GB hugepages 可能需要在启动时通过内核参数预留"
            print_warning "请在 GRUB 配置中添加: default_hugepagesz=1G hugepagesz=1G hugepages=$NUM_PAGES"
        else
            print_success "配置了 $ACTUAL_PAGES 个 1GB hugepages"
        fi
    else
        print_error "系统不支持 1GB hugepages"
        print_warning "请在 GRUB 配置中添加: default_hugepagesz=1G hugepagesz=1G hugepages=$NUM_PAGES"
        exit 1
    fi
fi

# 验证 hugepages 配置
if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    TOTAL_HUGEPAGES=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    FREE_HUGEPAGES=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
else
    TOTAL_HUGEPAGES=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages)
    FREE_HUGEPAGES=$(cat /sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages)
fi

print_success "Hugepages 配置完成: 总计 $TOTAL_HUGEPAGES 个，空闲 $FREE_HUGEPAGES 个"
echo

# 步骤 3: 挂载 hugepages 文件系统
print_info "步骤 3: 挂载 hugepages 文件系统..."

HUGEPAGE_MOUNT="/mnt/huge"

if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    PAGESIZE="pagesize=2M"
else
    PAGESIZE="pagesize=1G"
fi

# 创建挂载点
if [ ! -d "$HUGEPAGE_MOUNT" ]; then
    print_info "创建挂载点: $HUGEPAGE_MOUNT"
    mkdir -p $HUGEPAGE_MOUNT
fi

# 检查是否已挂载
if mount | grep -q "$HUGEPAGE_MOUNT"; then
    print_warning "Hugepages 已挂载，先卸载..."
    umount $HUGEPAGE_MOUNT || true
fi

# 挂载 hugepages
print_info "挂载 hugepages 到 $HUGEPAGE_MOUNT..."
mount -t hugetlbfs -o $PAGESIZE nodev $HUGEPAGE_MOUNT

if mount | grep -q "$HUGEPAGE_MOUNT"; then
    print_success "Hugepages 文件系统挂载成功"
else
    print_error "Hugepages 文件系统挂载失败"
    exit 1
fi

echo

# 步骤 4: 配置 VFIO
print_info "步骤 4: 配置 VFIO..."

if [ -d "/sys/module/vfio_pci" ]; then
    # 允许 VFIO 在不安全模式下运行（如果需要的话）
    if [ -f "/sys/module/vfio/parameters/enable_unsafe_noiommu_mode" ]; then
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
        print_success "启用 VFIO unsafe noiommu 模式"
    fi
fi

echo

# 步骤 5: 显示网卡信息
print_info "步骤 5: 显示网卡信息..."

if command -v dpdk-devbind.py &> /dev/null; then
    print_info "当前网卡绑定状态:"
    echo "----------------------------------------"
    dpdk-devbind.py --status-dev net
    echo "----------------------------------------"
    echo
    
    print_info "提示: 使用以下命令绑定网卡到 DPDK:"
    print_info "  sudo dpdk-devbind.py --bind=vfio-pci <PCI_ADDRESS>"
    print_info "  或者运行项目中的 dpdk_dev_init.sh 脚本"
elif command -v dpdk-devbind &> /dev/null; then
    print_info "当前网卡绑定状态:"
    echo "----------------------------------------"
    dpdk-devbind --status-dev net
    echo "----------------------------------------"
    echo
    
    print_info "提示: 使用以下命令绑定网卡到 DPDK:"
    print_info "  sudo dpdk-devbind --bind=vfio-pci <PCI_ADDRESS>"
    print_info "  或者运行项目中的 dpdk_dev_init.sh 脚本"
else
    print_warning "未找到 dpdk-devbind 工具"
    print_info "您可以手动绑定网卡，或运行项目中的 dpdk_dev_init.sh 脚本"
fi

echo

# 步骤 6: 显示汇总信息
print_success "========================================"
print_success "DPDK 环境初始化完成!"
print_success "========================================"
print_info "Hugepages: $TOTAL_HUGEPAGES 个 ($HUGEPAGE_SIZE)，空闲 $FREE_HUGEPAGES 个"
print_info "挂载点: $HUGEPAGE_MOUNT"
print_info "内核模块: $(lsmod | grep -E 'vfio_pci|uio|igb_uio' | awk '{print $1}' | tr '\n' ' ')"
echo
print_info "下一步操作:"
print_info "  1. 如果需要，绑定网卡: sudo ./dpdk_dev_init.sh"
print_info "  2. 运行发送方: ./run_sender.sh"
print_info "  3. 运行接收方: ./run_receiver.sh"
echo

# 步骤 7: 可选的持久化配置提示
print_info "========================================"
print_info "持久化配置 (可选)"
print_info "========================================"
print_info "如果希望重启后自动配置 hugepages，可以："
print_info "  1. 编辑 /etc/sysctl.conf，添加:"
print_info "     vm.nr_hugepages=$NUM_PAGES"
echo
print_info "  2. 或者编辑 /etc/default/grub，添加到 GRUB_CMDLINE_LINUX:"
if [ "$HUGEPAGE_SIZE" = "2M" ]; then
    print_info "     hugepagesz=2M hugepages=$NUM_PAGES"
else
    print_info "     default_hugepagesz=1G hugepagesz=1G hugepages=$NUM_PAGES"
fi
print_info "     然后运行: sudo update-grub"
echo

print_success "脚本执行完成!"
