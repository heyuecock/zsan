// 常量定义
const RATE_LIMIT = {
    WINDOW_SIZE: 60, // 60秒窗口
    MAX_REQUESTS: 100 // 每个窗口最大请求数
};

const ERROR_MESSAGES = {
    RATE_LIMIT: '请求过于频繁，请稍后再试',
    INVALID_DATA: '无效的数据格式',
    DB_ERROR: '数据库操作失败',
    NOT_FOUND: '资源未找到',
    SERVER_ERROR: '服务器内部错误'
};

// 在常量定义部分添加数据保留时间配置
const DATA_RETENTION = {
    MAX_RECORDS_PER_CLIENT: 10  // 每个客户端保留的最大记录数
};

// 工具函数
const utils = {
    validateMetrics: (data) => {
        const required = ['machine_id', 'name', 'system', 'uptime'];
        return required.every(field => data.has(field));
    },

    sanitizeString: (str) => {
        if (!str) return '';
        return str.replace(/[<>]/g, '').slice(0, 255);
    },

    formatResponse: (success, data, error = null) => {
        return {
            success,
            timestamp: Date.now(),
            data: data || null,
            error: error || null
        };
    },

    handleError: (error, status = 500) => {
        console.error('Error:', error);
        return new Response(
            JSON.stringify(utils.formatResponse(false, null, error.message || ERROR_MESSAGES.SERVER_ERROR)),
            {
                status,
                headers: {
                    'Content-Type': 'application/json',
                    'Access-Control-Allow-Origin': '*'
                }
            }
        );
    },

    async cleanupOldData(env) {
        try {
            // 删除每个客户端超过10条的旧记录
            await env.DB
                .prepare(`
                    DELETE FROM status 
                    WHERE id NOT IN (
                        SELECT id
                        FROM (
                            SELECT id
                            FROM status
                            WHERE client_id = status.client_id
                            ORDER BY insert_utc_ts DESC
                            LIMIT ?
                        )
                    )
                `)
                .bind(DATA_RETENTION.MAX_RECORDS_PER_CLIENT)
                .run();

            // 删除没有关联状态数据的客户端
            await env.DB
                .prepare(`
                    DELETE FROM client 
                    WHERE id NOT IN (
                        SELECT DISTINCT client_id 
                        FROM status
                    )
                `)
                .run();

            console.log(`Cleaned up old data, keeping ${DATA_RETENTION.MAX_RECORDS_PER_CLIENT} latest records per client`);
        } catch (error) {
            console.error('Error cleaning up old data:', error);
        }
    }
};

// 速率限制中间件
class RateLimiter {
    constructor() {
        this.requests = new Map();
    }

    async checkLimit(request) {
        const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
        const now = Date.now();
        const windowStart = now - (RATE_LIMIT.WINDOW_SIZE * 1000);

        // 清理过期的请求记录
        if (this.requests.has(ip)) {
            this.requests.get(ip).timestamps = this.requests.get(ip).timestamps.filter(
                time => time > windowStart
            );
        }

        // 获取或初始化请求记录
        const record = this.requests.get(ip) || { timestamps: [] };
        
        // 检查是否超过限制
        if (record.timestamps.length >= RATE_LIMIT.MAX_REQUESTS) {
            return false;
        }

        // 记录新的请求
        record.timestamps.push(now);
        this.requests.set(ip, record);
        return true;
    }
}

const rateLimiter = new RateLimiter();

// 路由处理函数
const routeHandlers = {
    async handlePostStatus(request, env) {
        try {
            // 速率限制检查
            if (!await rateLimiter.checkLimit(request)) {
                return utils.handleError(new Error(ERROR_MESSAGES.RATE_LIMIT), 429);
            }

            const formData = await request.formData();
            
            // 数据验证
            if (!utils.validateMetrics(formData)) {
                return utils.handleError(new Error(ERROR_MESSAGES.INVALID_DATA), 400);
            }

            // 清理和验证数据
            const machineId = utils.sanitizeString(formData.get('machine_id'));
            const name = utils.sanitizeString(formData.get('name')) || '未命名';
            const system = utils.sanitizeString(formData.get('system')) || '';
            const location = utils.sanitizeString(formData.get('location')) || '未知';

            // 数据库操作
            let clientId;
            let { results } = await env.DB
                .prepare('SELECT id FROM client WHERE machine_id = ?')
                .bind(machineId)
                .run();

            if (results && results[0]) {
                clientId = results[0].id;
                await env.DB
                    .prepare('UPDATE client SET name = ? WHERE id = ?')
                    .bind(name, clientId)
                    .run();
            } else {
                const { meta } = await env.DB
                    .prepare('INSERT INTO client (machine_id, name) VALUES (?, ?)')
                    .bind(machineId, name)
                    .run();
                clientId = meta.last_row_id;
            }

            // 插入状态数据
            await env.DB
                .prepare(`
                    INSERT INTO status (
                        client_id, name, system, location, insert_utc_ts,
                        uptime, cpu_percent, net_tx, net_rx, disks_total_kb,
                        disks_avail_kb, cpu_num_cores, mem_total, mem_free,
                        mem_used, swap_total, swap_free, process_count,
                        connection_count
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                `)
                .bind(
                    clientId,
                    name,
                    system,
                    location,
                    Math.floor(Date.now() / 1000),
                    parseInt(formData.get('uptime')) || 0,
                    parseFloat(formData.get('cpu_percent')) || 0,
                    parseInt(formData.get('net_tx')) || 0,
                    parseInt(formData.get('net_rx')) || 0,
                    parseInt(formData.get('disks_total_kb')) || 0,
                    parseInt(formData.get('disks_avail_kb')) || 0,
                    parseInt(formData.get('cpu_num_cores')) || 0,
                    parseFloat(formData.get('mem_total')) || 0,
                    parseFloat(formData.get('mem_free')) || 0,
                    parseFloat(formData.get('mem_used')) || 0,
                    parseFloat(formData.get('swap_total')) || 0,
                    parseFloat(formData.get('swap_free')) || 0,
                    parseInt(formData.get('process_count')) || 0,
                    parseInt(formData.get('connection_count')) || 0
                )
                .run();

            // 每次插入数据后都执行清理
            await utils.cleanupOldData(env);

            return new Response(
                JSON.stringify(utils.formatResponse(true, {
                    client_id: clientId,
                    name: name,
                    location: location
                })),
                {
                    headers: {
                        'Content-Type': 'application/json',
                        'Access-Control-Allow-Origin': '*'
                    }
                }
            );
        } catch (error) {
            return utils.handleError(error);
        }
    },

    async handleGetLatestStatus(request, env) {
        try {
            // 速率限制检查
            if (!await rateLimiter.checkLimit(request)) {
                return utils.handleError(new Error(ERROR_MESSAGES.RATE_LIMIT), 429);
            }

            const { results } = await env.DB
                .prepare(`
                    SELECT 
                        c.machine_id, 
                        c.name, 
                        s.*
                    FROM status s
                    JOIN client c ON s.client_id = c.id
                    WHERE s.id IN (
                        SELECT MAX(id)
                        FROM status
                        GROUP BY client_id
                    )
                `)
                .run();

            return new Response(
                JSON.stringify(utils.formatResponse(true, results)),
                {
                    headers: {
                        'Content-Type': 'application/json',
                        'Access-Control-Allow-Origin': '*',
                        'Cache-Control': 'no-cache'
                    }
                }
            );
        } catch (error) {
            return utils.handleError(error);
        }
    },

    async handleGetIndex(request, env) {
        try {
            const response = await fetch(
                'https://raw.githubusercontent.com/heyuecock/zsan-server-worker/refs/heads/main/index.html'
            );
            
            if (!response.ok) {
                throw new Error('Failed to fetch template');
            }
            
            const html = await response.text();
            
            return new Response(html, {
                headers: { 
                    'Content-Type': 'text/html',
                    'Access-Control-Allow-Origin': '*',
                    'Cache-Control': 'public, max-age=3600'
                },
            });
        } catch (error) {
            return utils.handleError(error);
        }
    },

    async handleGetStatus(request, env) {
        return new Response('kunlun', {
            headers: { 
                'Content-Type': 'text/plain',
                'Cache-Control': 'no-cache'
            },
        });
    },

    async handleOptions(request) {
        return new Response(null, {
            headers: {
                'Access-Control-Allow-Origin': '*',
                'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
                'Access-Control-Allow-Headers': 'Content-Type',
                'Access-Control-Max-Age': '86400'
            }
        });
    }
};

// 主导出
export default {
    async fetch(request, env) {
        try {
            const url = new URL(request.url);
            const path = url.pathname;
            const method = request.method;

            // 处理 OPTIONS 请求
            if (method === 'OPTIONS') {
                return routeHandlers.handleOptions(request);
            }

            // 定义路由映射
            const routes = {
                'POST /status': routeHandlers.handlePostStatus,
                'GET /status/latest': routeHandlers.handleGetLatestStatus,
                'GET /': routeHandlers.handleGetIndex,
                'GET /status': routeHandlers.handleGetStatus,
            };

            // 路由匹配
            const routeKey = `${method} ${path}`;
            const handler = routes[routeKey];

            if (handler) {
                return await handler(request, env);
            }

            return utils.handleError(new Error(ERROR_MESSAGES.NOT_FOUND), 404);
        } catch (error) {
            return utils.handleError(error);
        }
    }
};

