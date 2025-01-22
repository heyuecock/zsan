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
    }
    
    log "环境检查通过"
}

# 安装 zsan
install_zsan() {
    pre_install_check
    
    # 第一步：安装 curl（如果需要）
    log "检查并安装依赖..."
    case $OS in
        ubuntu|debian)
            if $IS_ROOT; then
                apt update && apt install -y curl || error_exit "安装 curl 失败"
            else
                sudo apt update && sudo apt install -y curl || error_exit "安装 curl 失败"
            fi
            ;;
        centos|rhel|fedora)
            if $IS_ROOT; then
                yum install -y curl || error_exit "安装 curl 失败"
            else
                sudo yum install -y curl || error_exit "安装 curl 失败"
            fi
            ;;
        arch)
            if $IS_ROOT; then
                pacman -S --noconfirm curl || error_exit "安装 curl 失败"
            else
                sudo pacman -S --noconfirm curl || error_exit "安装 curl 失败"
            fi
            ;;
        *)
            error_exit "不支持的发行版: $OS"
            ;;
    esac

    # 获取服务器配置信息
    read -p "请输入服务器名称: " SERVER_NAME
    SERVER_NAME=${SERVER_NAME:-"未命名服务器"}

    read -p "请输入服务器位置: " SERVER_LOCATION
    SERVER_LOCATION=${SERVER_LOCATION:-"未知位置"}

    read -p "请输入监测间隔（秒，默认 10）: " INTERVAL
    INTERVAL=${INTERVAL:-10}

    read -p "请输入上报地址（例如 https://example.com/status）: " REPORT_URL
    if [ -z "$REPORT_URL" ]; then
        error_exit "上报地址不能为空！"
    fi

    # 校验上报地址
    log "校验上报地址..."
    RESPONSE=$(curl -s "$REPORT_URL" || error_exit "无法连接到上报地址")
    if [[ $RESPONSE != *"kunlun"* ]]; then
        error_exit "上报地址校验失败：返回内容不包含 'kunlun'"
    fi
    log "上报地址校验通过！"

    # 创建必要的目录
    log "创建必要的目录..."
    mkdir -p ~/.zsan || error_exit "创建配置目录失败"
    mkdir -p ~/bin || error_exit "创建二进制目录失败"

    # 拉取 zsan 二进制文件
    log "拉取 zsan 二进制文件..."
    if ! curl -L https://github.com/heyuecock/zsan/releases/download/v0.0.1/zsan_amd64 -o ~/bin/zsan_amd64; then
        error_exit "下载 zsan 二进制文件失败"
    fi
    chmod +x ~/bin/zsan_amd64 || error_exit "设置可执行权限失败"

    # 创建配置文件
    log "创建配置文件..."
    CONFIG_FILE=~/.zsan/config
    cat > "$CONFIG_FILE" <<EOF || error_exit "创建配置文件失败"
SERVER_NAME="$SERVER_NAME"
SERVER_LOCATION="$SERVER_LOCATION"
REPORT_URL="$REPORT_URL"
INTERVAL=$INTERVAL
EOF

    # 设置配置文件权限
    chmod 600 "$CONFIG_FILE" || error_exit "设置配置文件权限失败"

    # 配置 systemd 服务
    log "配置 systemd 服务..."
    SERVICE_NAME="zsan"
    SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME.service"

    # 创建 systemd 服务文件
    SERVICE_CONTENT="[Unit]
Description=zsan System Monitor
After=network.target

[Service]
ExecStart=$HOME/bin/zsan_amd64 -s $INTERVAL -u $REPORT_URL
Environment=\"SERVER_NAME=$SERVER_NAME\"
Environment=\"SERVER_LOCATION=$SERVER_LOCATION\"
Restart=always
User=$USER
Environment=HOME=$HOME

[Install]
WantedBy=multi-user.target"

    if $IS_ROOT; then
        echo "$SERVICE_CONTENT" > "$SERVICE_PATH" || error_exit "创建服务文件失败"
    else
        echo "$SERVICE_CONTENT" | sudo tee "$SERVICE_PATH" > /dev/null || error_exit "创建服务文件失败"
    fi

    # 重载 systemd 配置
    log "重载 systemd 配置..."
    if $IS_ROOT; then
        systemctl daemon-reload || error_exit "重载 systemd 配置失败"
    else
        sudo systemctl daemon-reload || error_exit "重载 systemd 配置失败"
    fi

    # 启动并启用服务
    log "启动服务..."
    if $IS_ROOT; then
        systemctl start $SERVICE_NAME || error_exit "启动服务失败"
        systemctl enable $SERVICE_NAME || error_exit "启用服务失败"
    else
        sudo systemctl start $SERVICE_NAME || error_exit "启动服务失败"
        sudo systemctl enable $SERVICE_NAME || error_exit "启用服务失败"
    fi

    log "zsan 已成功安装并启动！"
    log "使用以下命令查看状态："
    log "sudo systemctl status $SERVICE_NAME"
}

# 卸载 zsan
uninstall_zsan() {
    log "开始卸载 zsan..."
    SERVICE_NAME="zsan"
    SERVICE_PATH="/etc/systemd/system/$SERVICE_NAME.service"

    # 停止并禁用服务
    if systemctl is-active --quiet $SERVICE_NAME; then
        log "停止 zsan 服务..."
        if $IS_ROOT; then
            systemctl stop $SERVICE_NAME || error_exit "停止服务失败"
        else
            sudo systemctl stop $SERVICE_NAME || error_exit "停止服务失败"
        fi
    fi

    if systemctl is-enabled --quiet $SERVICE_NAME; then
        log "禁用 zsan 服务..."
        if $IS_ROOT; then
            systemctl disable $SERVICE_NAME || error_exit "禁用服务失败"
        else
            sudo systemctl disable $SERVICE_NAME || error_exit "禁用服务失败"
        fi
    fi

    # 删除服务文件
    if [ -f "$SERVICE_PATH" ]; then
        log "删除 systemd 服务文件..."
        if $IS_ROOT; then
            rm -f "$SERVICE_PATH" || error_exit "删除服务文件失败"
            systemctl daemon-reload
        else
            sudo rm -f "$SERVICE_PATH" || error_exit "删除服务文件失败"
            sudo systemctl daemon-reload
        fi
    fi

    # 删除二进制文件和配置
    log "删除 zsan 文件..."
    rm -f "$HOME/bin/zsan_amd64"
    rm -rf "$HOME/.zsan"

    log "zsan 已成功卸载！"
}

# 主菜单
main() {
    echo "请选择操作："
    echo "1. 安装 zsan"
    echo "2. 卸载 zsan"
    read -p "请输入选项（1 或 2）: " CHOICE

    case $CHOICE in
        1)
            install_zsan
            ;;
        2)
            uninstall_zsan
            ;;
        *)
            error_exit "无效选项！"
            ;;
    esac
}

# 执行主菜单
main
