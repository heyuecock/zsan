#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <netdb.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <stdarg.h>
#include <errno.h>
#include <ifaddrs.h>
#include <arpa/inet.h>

// 首先定义所有结构体
typedef struct {
    char name[64];                 // 服务器名称
    char system[128];              // 系统信息
    char location[64];             // 地理位置
    long uptime;                   // 系统运行时间
    double cpu_percent;            // CPU使用率
    unsigned long net_tx;          // 网络发送字节数
    unsigned long net_rx;          // 网络接收字节数
    unsigned long disks_total_kb;  // 磁盘总空间
    unsigned long disks_avail_kb;  // 磁盘可用空间
    int cpu_num_cores;             // CPU核心数
    double mem_total;              // 内存总量
    double mem_free;               // 空闲内存
    double mem_used;               // 已用内存
    double swap_total;             // 交换分区总量
    double swap_free;              // 交换分区可用
    int process_count;             // 进程数
    int connection_count;          // 连接数
    char machine_id[33];           // 机器ID
    char ip_address[INET6_ADDRSTRLEN]; // 本机IP地址
} SystemInfo;

// 全局变量声明
char g_server_name[64] = "未命名";
char g_server_location[64] = "未知";

// 函数声明 - 确保返回类型与定义匹配
int get_connection_count(void);
char *metrics_to_post_data(const SystemInfo *info);
int send_post_request(const char *url, const char *data);
void get_system_info(char *buffer, size_t size);
int get_machine_id(char *buffer, size_t buffer_size);  // 修改为返回 int
void get_total_traffic(unsigned long *net_tx, unsigned long *net_rx);
void get_disk_usage(unsigned long *disks_total_kb, unsigned long *disks_avail_kb);
void get_swap_info(double *swap_total, double *swap_free);
int get_process_count(void);
void collect_metrics(SystemInfo *info);

typedef struct {
    long cpu_delay;                // CPU 延迟（微秒）
    long disk_delay;               // 磁盘延迟（微秒）
    unsigned long net_tx;          // 默认路由接口的发送流量（字节）
    unsigned long net_rx;          // 默认路由接口的接收流量（字节）
    unsigned long disks_total_kb;  // 磁盘总容量（KB）
    unsigned long disks_avail_kb;  // 磁盘可用容量（KB）
    int cpu_num_cores;             // CPU 核心数
    char machine_id[33];           // 固定长度的字符数组，存储机器 ID
} SystemMetrics;


typedef struct {
    long uptime;                   // 系统运行时间（秒）
    double load_1min;              // 1 分钟负载
    double load_5min;              // 5 分钟负载
    double load_15min;             // 15 分钟负载
    int task_total;                // 总任务数
    int task_running;              // 正在运行的任务数
    double cpu_us;                 // 用户空间占用 CPU 统计值
    double cpu_sy;                 // 内核空间占用 CPU 统计值
    double cpu_ni;                 // 用户进程空间内改变过优先级的进程占用 CPU 统计值
    double cpu_id;                 // 空闲 CPU 统计值
    double cpu_wa;                 // 等待 I/O 的 CPU 统计值
    double cpu_hi;                 // 硬件中断占用 CPU 统计值
    double cpu_st;                 // 虚拟机偷取的 CPU 统计值
    double mem_total;              // 总内存大小
    double mem_free;               // 空闲内存大小
    double mem_used;               // 已用内存大小
    double mem_buff_cache;         // 缓存和缓冲区内存大小
    int tcp_connections;           // TCP 连接数
    int udp_connections;           // UDP 连接数
} ProcResult;

// 从 /proc/uptime 读取系统运行时间
void read_uptime(ProcResult *result) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) {
        perror("Failed to open /proc/uptime");
        exit(EXIT_FAILURE);
    }
    double uptime;
    fscanf(fp, "%lf", &uptime);
    fclose(fp);
    result->uptime = (long)uptime;
}

// 从 /proc/loadavg 读取负载信息和任务信息
void read_loadavg_and_tasks(ProcResult *result) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) {
        perror("Failed to open /proc/loadavg");
        exit(EXIT_FAILURE);
    }
    char line[256];
    fgets(line, sizeof(line), fp);
    fclose(fp);
    sscanf(line, "%lf %lf %lf %d/%d",
           &result->load_1min, &result->load_5min, &result->load_15min,
           &result->task_running, &result->task_total);
}

// 从 /proc/stat 读取 CPU 信息
void read_cpu_info(ProcResult *result) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("Failed to open /proc/stat");
        exit(EXIT_FAILURE);
    }
    char line[256];
    fgets(line, sizeof(line), fp); // 读取第一行（总 CPU 信息）
    fclose(fp);
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    
    result->cpu_us = user;
    result->cpu_sy = system;
    result->cpu_ni = nice;
    result->cpu_id = idle;
    result->cpu_wa = iowait;
    result->cpu_hi = irq;
    result->cpu_st = steal;
}

// 从 /proc/meminfo 读取内存信息
void read_mem_info(ProcResult *result) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        perror("Failed to open /proc/meminfo");
        exit(EXIT_FAILURE);
    }
    char line[256];
    unsigned long long mem_total = 0, mem_free = 0, mem_buffers = 0, mem_cached = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %llu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %llu kB", &mem_free) == 1) continue;
        if (sscanf(line, "Buffers: %llu kB", &mem_buffers) == 1) continue;
        if (sscanf(line, "Cached: %llu kB", &mem_cached) == 1) continue;
    }
    fclose(fp);
    result->mem_total = mem_total / 1024.0; // 转换为 MiB
    result->mem_free = mem_free / 1024.0;
    result->mem_used = (mem_total - mem_free) / 1024.0;
    result->mem_buff_cache = (mem_buffers + mem_cached) / 1024.0;
}

// 从 /proc/net/tcp 和 /proc/net/udp 读取 TCP/UDP 连接数
void read_network_info(ProcResult *result) {
    FILE *fp;
    char line[256];
    result->tcp_connections = 0;
    fp = fopen("/proc/net/tcp", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, ":") != NULL) result->tcp_connections++;
        }
        fclose(fp);
    } else {
        perror("Failed to open /proc/net/tcp");
    }
    result->udp_connections = 0;
    fp = fopen("/proc/net/udp", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, ":") != NULL) result->udp_connections++;
        }
        fclose(fp);
    } else {
        perror("Failed to open /proc/net/udp");
    }
}

// 获取 Linux 服务器的 machine-id
int get_machine_id(char *buffer, size_t buffer_size) {
    char *paths[] = {"/etc/machine-id", "/var/lib/dbus/machine-id", NULL};
    for (int i = 0; paths[i] != NULL; i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp) {
            if (fgets(buffer, buffer_size, fp)) {
                fclose(fp);
                char *newline = strchr(buffer, '\n');
                if (newline) *newline = '\0';
                // 验证machine-id格式
                if (strlen(buffer) == 32) {
                    return 0; // 成功读取
                }
            }
            fclose(fp);
        }
    }
    
    // 如果无法获取machine-id,生成一个随机ID
    srand(time(NULL));
    for (int i = 0; i < 32; i++) {
        char c = "0123456789abcdef"[rand() % 16];
        buffer[i] = c;
    }
    buffer[32] = '\0';
    return 1; // 表示使用了随机生成的ID
}

// 修改 get_total_traffic 函数
void get_total_traffic(unsigned long *net_tx, unsigned long *net_rx) {
    FILE *fp;
    char line[256];
    *net_tx = 0;
    *net_rx = 0;
    fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        perror("Failed to open /proc/net/dev");
        return;
    }
    fgets(line, sizeof(line), fp); // 跳过表头
    fgets(line, sizeof(line), fp);
    
    while (fgets(line, sizeof(line), fp)) {
        char iface[32] = {0};
        unsigned long rx = 0, tx = 0;
        char *colon = strchr(line, ':');
        if (colon) {
            // 分别处理接口名和数据
            size_t name_len = colon - line;
            if (name_len > sizeof(iface) - 1) {
                name_len = sizeof(iface) - 1;
            }
            strncpy(iface, line, name_len);
            iface[name_len] = '\0';
            
            // 去除接口名称前后的空白字符
            char *start = iface;
            char *end = iface + strlen(iface) - 1;
            while (*start == ' ' || *start == '\t') start++;
            while (end > start && (*end == ' ' || *end == '\t')) *end-- = '\0';
            
            // 解析流量数据
            if (sscanf(colon + 1, " %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                      &rx, &tx) == 2) {
                if (strncmp(start, "lo", 2) != 0 && 
                    strncmp(start, "br", 2) != 0 &&
                    strncmp(start, "docker", 6) != 0 && 
                    strncmp(start, "veth", 4) != 0 &&
                    strncmp(start, "virbr", 5) != 0) {
                    *net_rx += rx;
                    *net_tx += tx;
                }
            }
        }
    }
    fclose(fp);
}

// 计算圆周率并返回时间（微秒）
long calculate_pi(int iterations) {
    double pi = 0.0;
    int sign = 1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++) {
        pi += sign * (4.0 / (2 * i + 1));
        sign *= -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
}

// 获取磁盘延迟（微秒）
long get_disk_delay(int iterations) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "tempfile%d", i);
        int fd = open(filename, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) {
            write(fd, "test", 4);
            close(fd);
            unlink(filename);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
}

// 获取磁盘总容量和可用容量
void get_disk_usage(unsigned long *disks_total_kb, unsigned long *disks_avail_kb) {
    FILE *fp;
    struct mntent *mnt;
    struct statvfs vfs;
    unsigned long total_kb = 0, avail_kb = 0;
    fp = setmntent("/proc/mounts", "r");
    if (!fp) {
        perror("Failed to open /proc/mounts");
        *disks_total_kb = *disks_avail_kb = 0;
        return;
    }
    while ((mnt = getmntent(fp)) != NULL) {
        if (strncmp(mnt->mnt_fsname, "/dev/", 5) == 0 &&
            strncmp(mnt->mnt_fsname, "/dev/loop", 9) != 0 &&
            strncmp(mnt->mnt_fsname, "/dev/ram", 8) != 0 &&
            strncmp(mnt->mnt_fsname, "/dev/dm-", 8) != 0) {
            if (statvfs(mnt->mnt_dir, &vfs) == 0) {
                unsigned long block_size = vfs.f_frsize / 1024;
                total_kb += vfs.f_blocks * block_size;
                avail_kb += vfs.f_bavail * block_size;
            }
        }
    }
    endmntent(fp);
    *disks_total_kb = total_kb;
    *disks_avail_kb = avail_kb;
}

// 获取系统信息
void get_system_info(char *buffer, size_t size) {
    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        char pretty_name[128] = "";
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *value = strchr(line, '=') + 1;
                if (value[0] == '"') value++;
                strcpy(pretty_name, value);
                if (pretty_name[strlen(pretty_name)-1] == '\n') 
                    pretty_name[strlen(pretty_name)-1] = '\0';
                if (pretty_name[strlen(pretty_name)-1] == '"') 
                    pretty_name[strlen(pretty_name)-1] = '\0';
                break;
            }
        }
        fclose(fp);
        snprintf(buffer, size, "%s", pretty_name);
    } else {
        strncpy(buffer, "Unknown", size);
    }
}

// 获取交换分区信息
void get_swap_info(double *swap_total, double *swap_free) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        *swap_total = 0;
        *swap_free = 0;
        return;
    }
    
    char line[256];
    unsigned long long total = 0, free = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "SwapTotal: %llu kB", &total) == 1) continue;
        if (sscanf(line, "SwapFree: %llu kB", &free) == 1) continue;
    }
    fclose(fp);
    
    *swap_total = total / 1024.0; // 转换为 MiB
    *swap_free = free / 1024.0;
}

// 获取进程数
int get_process_count() {
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type == DT_DIR) {
            char *endptr;
            strtol(entry->d_name, &endptr, 10);
            if (*endptr == '\0') count++;
        }
    }
    closedir(dir);
    return count;
}

// 将 get_connection_count 函数的定义移到 collect_metrics 函数之前
int get_connection_count() {
    int count = 0;
    FILE *fp;
    char line[256];
    
    // 统计 TCP 连接
    fp = fopen("/proc/net/tcp", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, ":") != NULL) count++;
        }
        fclose(fp);
    }
    
    // 统计 TCP6 连接
    fp = fopen("/proc/net/tcp6", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, ":") != NULL) count++;
        }
        fclose(fp);
    }
    
    return count;
}

// 获取本机IP地址
char* get_local_ip() {
    struct ifaddrs *ifaddr, *ifa;
    static char ip[INET6_ADDRSTRLEN];
    int family, s;

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return NULL;
    }

    // 遍历所有网络接口
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        // 只处理IPv4地址
        if (family == AF_INET) {
            // 跳过 lo 和 docker 等接口
            if (strncmp(ifa->ifa_name, "lo", 2) == 0 ||
                strncmp(ifa->ifa_name, "docker", 6) == 0 ||
                strncmp(ifa->ifa_name, "br-", 3) == 0 ||
                strncmp(ifa->ifa_name, "veth", 4) == 0)
                continue;

            s = getnameinfo(ifa->ifa_addr,
                          sizeof(struct sockaddr_in),
                          ip, NI_MAXHOST,
                          NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                continue;
            }
            
            // 找到第一个有效的非本地IP地址
            if (strcmp(ip, "127.0.0.1") != 0) {
                freeifaddrs(ifaddr);
                return ip;
            }
        }
    }

    freeifaddrs(ifaddr);
    return NULL;
}

// 获取所有监控数据
void collect_metrics(SystemInfo *info) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        info->uptime = si.uptime;
    }
    
    get_total_traffic(&info->net_tx, &info->net_rx);
    get_disk_usage(&info->disks_total_kb, &info->disks_avail_kb);
    info->cpu_num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    
    // 计算 CPU 使用率
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
            sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
            
            unsigned long total = user + nice + system + idle + iowait + irq + softirq + steal;
            unsigned long idle_total = idle + iowait;
            
            static unsigned long prev_total = 0;
            static unsigned long prev_idle = 0;
            
            if (prev_total > 0) {
                unsigned long total_diff = total - prev_total;
                unsigned long idle_diff = idle_total - prev_idle;
                info->cpu_percent = ((total_diff - idle_diff) * 100.0) / total_diff;
            } else {
                info->cpu_percent = 0;
            }
            
            prev_total = total;
            prev_idle = idle_total;
        }
        fclose(fp);
    }
    
    // 读取内存信息
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        unsigned long long mem_total = 0, mem_free = 0, mem_available = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemTotal: %llu kB", &mem_total) == 1) continue;
            if (sscanf(line, "MemFree: %llu kB", &mem_free) == 1) continue;
            if (sscanf(line, "MemAvailable: %llu kB", &mem_available) == 1) continue;
        }
        fclose(fp);
        
        info->mem_total = mem_total / 1024.0;  // 转换为 MB
        info->mem_free = mem_free / 1024.0;
        info->mem_used = (mem_total - mem_available) / 1024.0;
    }
    
    get_swap_info(&info->swap_total, &info->swap_free);
    info->process_count = get_process_count();
    info->connection_count = get_connection_count();
    get_system_info(info->system, sizeof(info->system));
    
    // 获取machine-id
    int machine_id_status = get_machine_id(info->machine_id, sizeof(info->machine_id));
    if (machine_id_status != 0) {
        fprintf(stderr, "Warning: Using randomly generated machine-id\n");
    }
    
    // 获取本机IP地址
    char *local_ip = get_local_ip();
    if (local_ip) {
        strncpy(info->ip_address, local_ip, sizeof(info->ip_address) - 1);
        info->ip_address[sizeof(info->ip_address) - 1] = '\0';
    } else {
        strncpy(info->ip_address, "unknown", sizeof(info->ip_address) - 1);
    }
}

// 将 metrics_to_post_data 函数移到 main 函数之前
char *metrics_to_post_data(const SystemInfo *info) {
    char *data = malloc(4096);
    if (!data) {
        fprintf(stderr, "Error: Failed to allocate memory for POST data\n");
        return NULL;
    }
    
    snprintf(data, 4096,
        "machine_id=%s&"
        "name=%s&"
        "system=%s&"
        "location=%s&"
        "ip_address=%s&"
        "uptime=%ld&"
        "cpu_percent=%.2f&"
        "net_tx=%lu&"
        "net_rx=%lu&"
        "disks_total_kb=%lu&"
        "disks_avail_kb=%lu&"
        "cpu_num_cores=%d&"
        "mem_total=%.1f&"
        "mem_free=%.1f&"
        "mem_used=%.1f&"
        "swap_total=%.1f&"
        "swap_free=%.1f&"
        "process_count=%d&"
        "connection_count=%d",
        info->machine_id,
        g_server_name,
        info->system,
        g_server_location,
        info->ip_address,
        info->uptime,
        info->cpu_percent,
        info->net_tx,
        info->net_rx,
        info->disks_total_kb,
        info->disks_avail_kb,
        info->cpu_num_cores,
        info->mem_total,
        info->mem_free,
        info->mem_used,
        info->swap_total,
        info->swap_free,
        info->process_count,
        info->connection_count
    );
    
    return data;
}

// 修改 send_post_request 函数，添加响应解析
int send_post_request(const char *url, const char *data) {
    const int max_retries = 3;
    const int retry_delay = 5; // seconds
    
    for (int retry = 0; retry < max_retries; retry++) {
        char command[4096];
        char response[4096];
        
        // 使用临时文件存储响应
        char temp_file[] = "/tmp/zsan_response_XXXXXX";
        int temp_fd = mkstemp(temp_file);
        if (temp_fd == -1) {
            log_message("ERROR", "Failed to create temporary file: %s", strerror(errno));
            return -1;
        }
        close(temp_fd);
        
        // 修改 curl 命令以保存响应并格式化 JSON
        snprintf(command, sizeof(command), 
                "curl -X POST -d '%s' '%s' --connect-timeout 10 --max-time 30 -s | python3 -m json.tool > %s",
                data, url, temp_file);
                
        int result = system(command);
        
        // 读取响应
        FILE *fp = fopen(temp_file, "r");
        if (fp) {
            size_t bytes_read = fread(response, 1, sizeof(response) - 1, fp);
            response[bytes_read] = '\0';
            fclose(fp);
            
            // 解析响应中的关键信息
            char *success_str = strstr(response, "\"success\":");
            char *error_str = strstr(response, "\"error\":");
            char *data_str = strstr(response, "\"data\":");
            
            if (success_str && strstr(success_str, "true")) {
                if (data_str) {
                    char *client_id_str = strstr(data_str, "\"client_id\":");
                    char *name_str = strstr(data_str, "\"name\":");
                    if (client_id_str && name_str) {
                        int client_id;
                        char name[64];
                        sscanf(client_id_str, "\"client_id\": %d", &client_id);
                        sscanf(name_str, "\"name\": \"%63[^\"]\"", name);
                        log_message("INFO", "Data sent successfully - Client ID: %d, Name: %s", 
                                  client_id, name);
                    } else {
                        log_message("INFO", "Data sent successfully");
                    }
                }
                unlink(temp_file);
                return 0;
            } else if (error_str) {
                char error_msg[256] = {0};
                if (sscanf(error_str, "\"error\": \"%255[^\"]\"", error_msg) == 1) {
                    log_message("ERROR", "Server error: %s", error_msg);
                } else {
                    log_message("ERROR", "Unknown server error");
                }
            }
        }
        unlink(temp_file);
        
        if (retry < max_retries - 1) {
            log_message("INFO", "Retrying in %d seconds (attempt %d/%d)...", 
                       retry_delay, retry + 1, max_retries);
            sleep(retry_delay);
        }
    }
    
    return -1;
}

// 修改 log_message 函数，改进格式化
void log_message(const char *level, const char *format, ...) {
    time_t now;
    time(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    va_list args;
    va_start(args, format);
    
    // 准备完整的日志消息
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    
    // 构建完整的日志行，添加更多上下文信息
    char log_line[2048];
    snprintf(log_line, sizeof(log_line), 
             "[%s] [%s] [PID:%d] [Name:%s] [Location:%s] %s\n",
             timestamp, level, getpid(), 
             g_server_name[0] ? g_server_name : "未命名",
             g_server_location[0] ? g_server_location : "未知",
             message);
    
    // 写入到标准错误（带颜色）
    const char *color = "";
    const char *reset = "\033[0m";
    if (strcmp(level, "ERROR") == 0) {
        color = "\033[31m"; // 红色
    } else if (strcmp(level, "INFO") == 0) {
        color = "\033[32m"; // 绿色
    } else if (strcmp(level, "WARN") == 0) {
        color = "\033[33m"; // 黄色
    }
    fprintf(stderr, "%s%s%s", color, log_line, reset);
    
    // 写入到日志文件（不带颜色）
    const char *log_path = (strcmp(level, "ERROR") == 0) ? 
        "/var/log/zsan/zsan.error.log" : "/var/log/zsan/zsan.log";
    
    FILE *log_file = fopen(log_path, "a");
    if (log_file) {
        fputs(log_line, log_file);
        fflush(log_file);  // 确保立即写入
        fclose(log_file);
    } else {
        fprintf(stderr, "%s无法写入日志文件 %s: %s%s\n", 
                "\033[31m", log_path, strerror(errno), reset);
    }
    
    va_end(args);
}

// 添加安全的字符串复制函数
static void safe_strncpy(char *dest, const char *src, size_t size) {
    if (size > 0) {
        size_t i;
        for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
            dest[i] = src[i];
        }
        dest[i] = '\0';
    }
}

// main 函数和其他代码保持不变
int main(int argc, char *argv[]) {
    // 检查日志文件权限
    FILE *test_log = fopen("/var/log/zsan/zsan.log", "a");
    FILE *test_error = fopen("/var/log/zsan/zsan.error.log", "a");
    
    if (!test_log || !test_error) {
        fprintf(stderr, "无法访问日志文件，请检查权限\n");
        if (test_log) fclose(test_log);
        if (test_error) fclose(test_error);
        return 1;
    }
    
    fclose(test_log);
    fclose(test_error);
    
    int interval = 10;
    char url[256] = "";
    int opt;
    
    // 从环境变量读取服务器名称和位置
    char *env_name = getenv("SERVER_NAME");
    char *env_location = getenv("SERVER_LOCATION");
    
    // 打印所有环境变量用于调试
    extern char **environ;
    printf("Environment variables:\n");
    for (char **env = environ; *env != NULL; env++) {
        printf("%s\n", *env);
    }
    
    // 确保环境变量被正确读取
    if (env_name) {
        safe_strncpy(g_server_name, env_name, sizeof(g_server_name));
        printf("Server name from env: %s\n", g_server_name);
    } else {
        printf("SERVER_NAME environment variable not found\n");
        FILE *fp = fopen("/root/.kunlun/config", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "SERVER_NAME=", 12) == 0) {
                    char *value = line + 12;
                    if (value[0] == '"') value++;
                    size_t len = strlen(value);
                    if (len > 0 && value[len-1] == '\n') value[len-1] = '\0';
                    if (len > 1 && value[len-2] == '"') value[len-2] = '\0';
                    safe_strncpy(g_server_name, value, sizeof(g_server_name));
                    printf("Server name from config: %s\n", g_server_name);
                    break;
                }
            }
            fclose(fp);
        }
    }
    
    if (env_location) {
        safe_strncpy(g_server_location, env_location, sizeof(g_server_location));
        printf("Server location from env: %s\n", g_server_location);
    } else {
        printf("SERVER_LOCATION environment variable not found\n");
        FILE *fp = fopen("/root/.kunlun/config", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "SERVER_LOCATION=", 15) == 0) {
                    char *value = line + 15;
                    if (value[0] == '"') value++;
                    size_t len = strlen(value);
                    if (len > 0 && value[len-1] == '\n') value[len-1] = '\0';
                    if (len > 1 && value[len-2] == '"') value[len-2] = '\0';
                    safe_strncpy(g_server_location, value, sizeof(g_server_location));
                    printf("Server location from config: %s\n", g_server_location);
                    break;
                }
            }
            fclose(fp);
        }
    }
    
    while ((opt = getopt(argc, argv, "s:u:")) != -1) {
        switch (opt) {
            case 's':
                interval = atoi(optarg);
                break;
            case 'u':
                strncpy(url, optarg, sizeof(url) - 1);
                break;
            default:
                fprintf(stderr, "Usage: %s -s <interval> -u <url>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (strlen(url) == 0) {
        fprintf(stderr, "Error: -u <url> is required.\n");
        fprintf(stderr, "Usage: %s -s <interval> -u <url>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    log_message("INFO", "zsan client starting up...");
    log_message("INFO", "Version: 0.0.1");
    
    while (1) {
        SystemInfo info = {0};
        collect_metrics(&info);
        char *post_data = metrics_to_post_data(&info);
        if (!post_data) {
            log_message("ERROR", "Failed to prepare POST data");
            sleep(interval);
            continue;
        }
        
        log_message("INFO", "Sending metrics to %s", url);
        if (send_post_request(url, post_data) != 0) {
            log_message("ERROR", "Failed to send data to %s", url);
        }
        
        free(post_data);
        sleep(interval);
    }
    return 0;
}
