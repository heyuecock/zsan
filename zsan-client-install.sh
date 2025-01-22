#!/bin/bash

# 错误处理函数
error_exit() {
    echo "错误: $1" >&2
    exit 1
}

# 日志函数
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

# 检查命令是否存在
check_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error_exit "未找到命令: $1，请先安装"
    fi
}

# 检查是否为 root 用户
if [ "$EUID" -eq 0 ]; then
    IS_ROOT=true
else
    IS_ROOT=false
fi

# 识别发行版
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    error_exit "无法识别发行版！"
fi

# 安装前环境检查
pre_install_check() {
    log "执行安装前检查..."
    
    # 检查必需命令
    check_command curl
    check_command systemctl
    
    # 检查目录权限
    if [ ! -w /etc/systemd/system ]; then
        error_exit "没有 systemd 服务目录的写入权限"
    fi
    
    # 检查内存
    total_mem=$(free -m | awk '/^Mem:/{print $2}')
    if [ "$total_mem" -lt 100 ]; then
        error_exit "内存不足（小于100MB）"
    fi
    
    log "环境检查通过"
}

[其余部分保持不变...]
