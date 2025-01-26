# Zsan 服务器监控系统

Zsan 是一个轻量级（客户端内存占用 < 1MB）、高效（仅一个 .c 文件）的服务器监控系统，帮助用户实时监控服务器的性能指标，并通过直观的 Web 界面展示数据。系统由 **Zsan Server**（后端）和 **Zsan Client**（客户端）组成，支持跨平台部署，适用于各种 Linux 环境。本仓库是基于 `Cloudflare Worker` + `D1` 的 Zsan Server 实现。

## 目录

- [快速开始](#快速开始)
- [系统架构](#系统架构)
- [功能特性](#功能特性)
- [详细配置](#详细配置)
- [性能优化](#性能优化)
- [故障排除](#故障排除)
- [安全说明](#安全说明)
- [开发指南](#开发指南)
- [更新日志](#更新日志)
- [贡献指南](#贡献指南)

## 快速开始

[演示地址](https://tz.yuehe.workers.dev/)

### 1. 部署 Worker 版 Zsan Server

#### 1.1 准备 Cloudflare D1 数据库
1. 进入 Cloudflare 控制台 -> 存储和数据库 -> D1 SQL 数据库
2. 点击 创建 按钮
3. 输入数据库名称为 zsan
4. 进入顶栏菜单的 控制台
5. 依次执行以下数据库创建命令：

```SQL
CREATE TABLE IF NOT EXISTS client (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    machine_id TEXT NOT NULL UNIQUE,
    name TEXT
);

CREATE TABLE IF NOT EXISTS status (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    client_id INTEGER NOT NULL,
    name TEXT,                   
    system TEXT,                 
    location TEXT,               
    insert_utc_ts INTEGER NOT NULL, 
    uptime INTEGER,              
    cpu_percent REAL,          
    net_tx INTEGER,             
    net_rx INTEGER,            
    disks_total_kb INTEGER,     
    disks_avail_kb INTEGER,    
    cpu_num_cores INTEGER,    
    mem_total REAL,          
    mem_free REAL,             
    mem_used REAL,             
    swap_total REAL,           
    swap_free REAL,             
    process_count INTEGER,      
    connection_count INTEGER,   
    ip_address TEXT,            
    country_code TEXT,          
    FOREIGN KEY (client_id) REFERENCES client(id)
);

CREATE TABLE IF NOT EXISTS latest_status (
    client_id INTEGER PRIMARY KEY,
    status_id INTEGER,
    insert_utc_ts INTEGER,
    ip_address TEXT,
    country_code TEXT,
    FOREIGN KEY (client_id) REFERENCES client(id),
    FOREIGN KEY (status_id) REFERENCES status(id)
);

CREATE TRIGGER IF NOT EXISTS update_latest_status
AFTER INSERT ON status
FOR EACH ROW
BEGIN
    INSERT OR REPLACE INTO latest_status (
        client_id, 
        status_id, 
        insert_utc_ts,
        ip_address,
        country_code
    )
    VALUES (
        NEW.client_id, 
        NEW.id, 
        NEW.insert_utc_ts,
        NEW.ip_address,
        NEW.country_code
    );
END;

CREATE INDEX IF NOT EXISTS idx_client_machine_id ON client(machine_id);
CREATE INDEX IF NOT EXISTS idx_status_client_id ON status(client_id);
CREATE INDEX IF NOT EXISTS idx_status_insert_time ON status(insert_utc_ts);
CREATE INDEX IF NOT EXISTS idx_status_ip_address ON status(ip_address);
CREATE INDEX IF NOT EXISTS idx_status_country_code ON status(country_code);
```

#### 1.2 部署 Worker
1. 进入 Cloudflare 控制台 -> Workers 和 Pages
2. 创建新的 Worker
3. 复制 worker.js 的内容到编辑器
4. 在设置中绑定 D1 数据库
5. 设置变量名称为 `DB`
6. 部署 Worker

### 2. 安装 Zsan Client

在需要监控的服务器上运行：

```bash
curl -L https://github.com/heyuecock/zsan-server-worker/raw/refs/heads/main/zsan-client-install.sh -o zsan-client-install.sh
chmod +x zsan-client-install.sh
./zsan-client-install.sh
```

## 系统架构

### Zsan Server（后端）
- 基于 Cloudflare Worker 提供 RESTful API
- 使用 D1 数据库存储监控数据
- 提供实时数据展示界面
- 支持多服务器监控
- 内置速率限制和安全保护
- 自动识别服务器地理位置并显示国旗

### Zsan Client（客户端）
- 基于 C 语言实现的轻量级客户端
- 支持自动获取服务器IP地址
- 支持自定义监控间隔
- 自动重试机制
- 内置日志记录
- 支持多种 Linux 发行版

## 功能特性

### 基础监控
- CPU 使用率和负载
- 内存使用情况
- 磁盘空间
- 网络流量
- 进程数量
- 系统负载

### 增强功能
- 服务器地理位置显示
- 国旗标识
- IP地址显示
- 实时状态更新
- 深色模式支持
- 响应式设计

## 详细配置

### 数据存储
- 每个客户端保留最新的 10 条记录
- 自动清理旧数据
- 保留所有客户端的最新状态

### 客户端配置
配置文件位置：`~/.zsan/config`
```ini
SERVER_NAME="服务器名称"
SERVER_LOCATION="服务器位置"
REPORT_URL="上报地址"
INTERVAL=10  # 监控间隔（秒）
```

### Worker 配置
- 速率限制：默认每 IP 每分钟 100 请求
- 缓存策略：首页缓存 1 小时，数据接口不缓存
- CORS：允许所有来源访问
- 数据清理：自动保留每个客户端最新的 10 条记录

## 性能优化

### 客户端优化
1. 减少不必要的系统调用
2. 使用缓冲区读取系统信息
3. 避免频繁内存分配
4. 优化网络重试策略

### 服务端优化
1. 使用索引提升查询性能
2. 实现数据自动清理
3. 优化 API 响应格式
4. 使用 CDN 加速静态资源

## 故障排除

### 常见问题
1. 客户端无法连接
   - 检查网络连接
   - 验证上报地址
   - 查看系统日志
   
2. 数据不更新
   - 检查监控进程状态
   - 验证配置文件权限
   - 检查磁盘空间

3. 性能问题
   - 调整监控间隔
   - 检查系统负载
   - 优化数据库查询

## 安全说明

1. 数据安全
   - 所有数据通过 HTTPS 传输
   - 配置文件权限限制为 600
   - 敏感信息加密存储

2. 访问控制
   - 内置速率限制
   - IP 白名单（可选）
   - 数据验证和清理

## 开发指南

### 本地开发
1. 克隆仓库
2. 安装依赖
3. 修改配置
4. 运行测试

### 代码规范
- C 代码遵循 K&R 风格
- JavaScript 使用 ES6+ 特性
- 使用 ESLint 进行代码检查

## 更新日志

### v0.0.2 (2024-03-xx)
- 添加服务器地理位置显示
- 添加国旗标识功能
- 优化 CPU 使用率计算方式
- 添加数据自动清理机制
- 改进前端界面显示
- 优化数据存储结构

### v0.0.1 (2024-03-xx)
- 初始版本发布
- 基本监控功能
- Web 界面支持
- 多服务器支持

## 贡献指南

1. Fork 项目
2. 创建特性分支
3. 提交变更
4. 发起 Pull Request

## 许可证

MIT License

## 致谢

感谢 [hochenggang](https://github.com/hochenggang) 的贡献。
