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

// 获取总的出流量和入流量
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
        char iface[32];
        unsigned long rx, tx;
        if (sscanf(line, "%31[^:]: %lu %*lu %*lu %*lu %*lu %*lu %*lu %*lu %lu",
                   iface, &rx, &tx) == 3) {
            char *end = iface + strlen(iface) - 1;
            while (end >= iface && (*end == ' ' || *end == '\t')) *end-- = '\0';
            if (strncmp(iface, "lo", 2) == 0 || strncmp(iface, "br", 2) == 0 ||
                strncmp(iface, "docker", 6) == 0 || strncmp(iface, "veth", 4) == 0 ||
                strncmp(iface, "virbr", 5) == 0) continue;
            *net_rx += rx;
            *net_tx += tx;
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

// 添加重试机制的 send_post_request 函数
int send_post_request(const char *url, const char *data) {
    const int max_retries = 3;
    const int retry_delay = 5; // seconds
    
    for (int retry = 0; retry < max_retries; retry++) {
        char command[4096];
        snprintf(command, sizeof(command), 
                "curl -X POST -d '%s' '%s' --connect-timeout 10 --max-time 30 -s",
                data, url);
                
        int result = system(command);
        if (result == 0) {
            return 0; // 成功
        }
        
        fprintf(stderr, "Failed to send data (attempt %d/%d)\n", 
                retry + 1, max_retries);
                
        if (retry < max_retries - 1) {
            sleep(retry_delay);
        }
    }
    
    return -1; // 所有重试都失败
}

// 添加日志记录函数
void log_message(const char *level, const char *format, ...) {
    time_t now;
    time(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    va_list args;
    va_start(args, format);
    
    fprintf(stderr, "[%s] [%s] ", timestamp, level);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
}

// main 函数和其他代码保持不变
int main(int argc, char *argv[]) {
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
        strncpy(g_server_name, env_name, sizeof(g_server_name) - 1);
        printf("Server name from env: %s\n", g_server_name);
    } else {
        printf("SERVER_NAME environment variable not found\n");
        // 尝试从配置文件读取
        FILE *fp = fopen("/root/.kunlun/config", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "SERVER_NAME=", 12) == 0) {
                    char *value = line + 12;
                    if (value[0] == '"') value++;
                    value[strlen(value)-1] = '\0';  // 移除换行符
                    if (value[strlen(value)-1] == '"') value[strlen(value)-1] = '\0';
                    strncpy(g_server_name, value, sizeof(g_server_name) - 1);
                    printf("Server name from config: %s\n", g_server_name);
                    break;
                }
            }
            fclose(fp);
        }
    }
    
    if (env_location) {
        strncpy(g_server_location, env_location, sizeof(g_server_location) - 1);
        printf("Server location from env: %s\n", g_server_location);
    } else {
        printf("SERVER_LOCATION environment variable not found\n");
        // 尝试从配置文件读取
        FILE *fp = fopen("/root/.kunlun/config", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strncmp(line, "SERVER_LOCATION=", 15) == 0) {
                    char *value = line + 15;
                    if (value[0] == '"') value++;
                    value[strlen(value)-1] = '\0';  // 移除换行符
                    if (value[strlen(value)-1] == '"') value[strlen(value)-1] = '\0';
                    strncpy(g_server_location, value, sizeof(g_server_location) - 1);
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
    
    log_message("INFO", "Starting zsan client with interval %d seconds", interval);
    
    while (1) {
        SystemInfo info = {0};
        collect_metrics(&info);
        char *post_data = metrics_to_post_data(&info);
        if (!post_data) {
            log_message("ERROR", "Failed to prepare POST data");
            sleep(interval);
            continue;
        }
        
        if (send_post_request(url, post_data) != 0) {
            log_message("ERROR", "Failed to send data to %s", url);
        } else {
            log_message("INFO", "Successfully sent metrics to server");
        }
        
        free(post_data);
        sleep(interval);
    }
    return 0;
}
