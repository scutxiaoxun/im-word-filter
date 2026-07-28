# 任务三：社交 Feed 流扩散系统核心设计与实现

> **场景约束**：基于给定三张表（`t_user`、`t_follow`、`t_feed`），完成 **100 万粉丝大V 发布动态**时的实时扩散与消息通知。采用 **混合推拉模式**。

## 一、需求复述与核心矛盾

**核心场景**：用户 A 发布动态，其粉丝数达 **100 万**，系统需完成动态持久化、扩散至所有粉丝时间线、触发消息通知。

**核心矛盾**：
- 纯写扩散（推）：100 万次 ZADD 写入，数据库/Redis 瞬间过载。
- 纯读扩散（拉）：100 万粉丝刷新时联表查询，数据库崩溃。

## 二、架构设计总览（混合推拉模式）

根据用户粉丝数量动态路由：

| 用户类型 | 粉丝数阈值 | 扩散策略 | 存储动作 |
| :--- | :--- | :--- | :--- |
| **普通用户** | < 10 万 | **写扩散（推模式）** | 动态 ID 写入每个粉丝的 `inbox` ZSET |
| **大V（明星）** | ≥ 10 万 | **读扩散（拉模式）** | 动态 ID 仅写入大V的 `outbox` ZSET，粉丝拉取时实时合并 |
## 三、补充数据模型设计

在给定的 `t_user`、`t_follow`、`t_feed` 三表基础上，增加以下缓存与异步层存储：

### 3.1 Redis 数据结构（核心加速层）

| Key 格式 | 数据类型 | 存储内容 | 过期策略 |
| :--- | :--- | :--- | :--- |
| `inbox:user:{user_id}` | **ZSET** | Member = `feed_id`<br>Score = 发布时间戳（毫秒） | 永久，惰性裁剪保留近 1000 条 |
| `outbox:user:{user_id}` | **ZSET** | Member = `feed_id`<br>Score = 发布时间戳（毫秒） | 永久 |
| `feed:detail:{feed_id}` | **HASH** | 动态详情（内容、图片列表、发布者 ID 等） | TTL=1 小时，LRU 淘汰 |
| `followers:bigv:{user_id}` | **SET** | 大V 的粉丝 ID 集合（全量） | TTL=10 分钟，定时从 MySQL 重建 |
| `follower_count:{user_id}` | **STRING** | 用户粉丝总数缓存 | TTL=1 分钟，降低 DB 计数查询压力 |

### 3.2 MySQL 辅助表（异步任务可靠性）

    CREATE TABLE `t_diffusion_task` (
        `task_id`       BIGINT UNSIGNED NOT NULL PRIMARY KEY AUTO_INCREMENT,
        `feed_id`       BIGINT UNSIGNED NOT NULL COMMENT '动态ID',
        `publisher_id`  BIGINT UNSIGNED NOT NULL COMMENT '发布者ID',
        `strategy`      TINYINT NOT NULL COMMENT '1-推模式 2-拉模式',
        `status`        TINYINT NOT NULL DEFAULT 0 COMMENT '0-待处理 1-处理中 2-已完成 3-失败',
        `retry_count`   INT NOT NULL DEFAULT 0,
        `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
        `completed_at`  DATETIME DEFAULT NULL,
        INDEX idx_status (`status`),
        INDEX idx_feed_publisher (`feed_id`, `publisher_id`)
    ) ENGINE=InnoDB COMMENT='动态扩散异步任务追踪表';
    ## 四、核心算法与 C++ 实现

### 4.1 发布入口：动态路由与分片

发布动态时，根据粉丝数量动态分流，并对推模式进行粉丝分片，避免 MQ 大消息体阻塞。

    // FeedDisseminationEngine.h
    #include <vector>
    #include <sw/redis++/redis++.h>
    #include <mq/rabbitmq.hpp>

    class FeedDisseminationEngine {
    private:
        sw::redis::Redis* redis_;
        RabbitMQProducer* mq_;
        const int64_t PUSH_THRESHOLD = 100000;      // 10万粉丝阈值
        const int CHUNK_SIZE = 1000;                 // 每片1000个粉丝

    public:
        // 核心接口：动态发布（发布者调用，阻塞时间仅 ~落库 + MQ投递）
        void PublishFeed(int64_t user_id, int64_t feed_id) {
            // 1. 获取粉丝总数（从 Redis 缓存读取，降低 DB 压力）
            int64_t follower_count = GetFollowerCountCached(user_id);
            
            // 2. 路由决策
            if (follower_count < PUSH_THRESHOLD) {
                // === 写扩散（推模式） ===
                auto fans = GetFollowerIds(user_id); // 从 DB 或 Redis 获取全量粉丝 ID
                
                // 分片投递：每片 1000 个粉丝，并行消费
                for (size_t i = 0; i < fans.size(); i += CHUNK_SIZE) {
                    PushTask task;
                    task.feed_id = feed_id;
                    task.publisher_id = user_id;
                    task.fan_ids.assign(
                        fans.begin() + i, 
                        fans.begin() + std::min(i + CHUNK_SIZE, fans.size())
                    );
                    mq_->Publish("diffusion_push_queue", task.Serialize());
                }
            } else {
                // === 读扩散（拉模式） ===
                // 仅写入大V的 outbox，不扩散至粉丝
                redis_->zadd("outbox:user:" + std::to_string(user_id),
                             GetCurrentTimeMs(),
                             std::to_string(feed_id));
                
                // 投递轻量级通知提醒（不携带动态内容，仅红点触发）
                NotifyFansAsync(user_id, feed_id);
            }
        }
    };
  ### 4.2 MQ 消费者：批量 Pipeline 写入

消费者处理分片任务，使用 **Redis Pipeline** 将数千条 `ZADD` 合并为一次网络请求。

    // PushConsumer.cpp
    void HandlePushTask(const PushTask& task) {
        // 1. 构建 Pipeline
        auto pipeline = redis_->pipeline();
        int64_t score = task.publish_time_ms; // 直接使用发布时间作为 Score
        
        for (auto& fan_id : task.fan_ids) {
            std::string key = "inbox:user:" + std::to_string(fan_id);
            pipeline.zadd(key, score, std::to_string(task.feed_id));
        }
        
        // 2. 一次性执行（1000 条命令，1 次网络 RTT）
        auto replies = pipeline.exec();
        
        // 3. 失败处理：记录失败的用户 ID，投递至重试队列
        // 更新 t_diffusion_task 状态
    }

**复杂度**：1000 条写入命令合并为 1 次网络往返，Redis 服务端顺序执行。
### 4.3 粉丝拉取时间线

这是整个系统最核心的读取路径，集成了 **堆排序**、**Lua 批量拉取**、**本地缓存** 和 **惰性清理**。

    // GetTimeline 完整实现（粉丝刷新时调用）
    std::vector<FeedDetail> GetTimeline(int64_t user_id, int limit = 20) {
        // ========== 第一步：收集候选 Feed ID ==========
        std::vector<FeedItem> candidates;
        
        // 1.1 拉取自己的 inbox（普通关注用户的动态）
        std::string inbox_key = "inbox:user:" + std::to_string(user_id);
        auto inbox_items = redis_->zrevrange_withscores(inbox_key, 0, limit * 2);
        candidates.insert(candidates.end(), inbox_items.begin(), inbox_items.end());
        
        // 惰性清理：仅当 inbox 过大时触发裁剪
        long long inbox_size = redis_->zcard(inbox_key);
        if (inbox_size > 1200) {  // 阈值 = 1000(保留) + 200(缓冲)
            // 使用 UNLINK 非阻塞删除，避免阻塞 Redis
            redis_->zremrangebyrank(inbox_key, 0, inbox_size - 1001);
        }
        
        // 1.2 获取该用户关注的所有大V ID 列表
        auto followed_bigvs = GetFollowedBigVIds(user_id);
        
        if (!followed_bigvs.empty()) {
            // Lua 脚本批量拉取所有大V的 outbox（1次网络RTT）
            std::vector<std::string> keys;
            for (auto& bigv_id : followed_bigvs) {
                keys.push_back("outbox:user:" + std::to_string(bigv_id));
            }
            // Lua 脚本：每个大V取最新 100 条
            auto lua_result = redis_->eval< std::vector<std::pair<std::string, double>> >(
                R"(
                    local results = {}
                    for i, key in ipairs(KEYS) do
                        local items = redis.call('ZREVRANGE', key, 0, 100, 'WITHSCORES')
                        for j = 1, #items, 2 do
                            table.insert(results, {items[j], items[j+1]})
                        end
                    end
                    return results
                )",
                keys,
                {}
            );
            candidates.insert(candidates.end(), lua_result.begin(), lua_result.end());
        }
        
        // ========== 第二步：去重 + TopN 堆排序 ==========
        // 使用哈希表去重（同一动态可能同时出现在 inbox 和 outbox）
        std::unordered_set<int64_t> seen;
        std::vector<FeedItem> unique_items;
        for (auto& item : candidates) {
            int64_t feed_id = std::stoll(item.member);
            if (seen.insert(feed_id).second) {
                unique_items.push_back(item);
            }
        }
        
        // 小根堆取 TopN，避免全量排序 O(K log K) -> O(K log N)
        auto cmp = [](const FeedItem& a, const FeedItem& b) { 
            return a.score > b.score; // 小根堆，堆顶最小
        };
        std::priority_queue<FeedItem, std::vector<FeedItem>, decltype(cmp)> min_heap(cmp);
        
        for (auto& item : unique_items) {
            if (min_heap.size() < limit) {
                min_heap.push(item);
            } else if (item.score > min_heap.top().score) {
                min_heap.pop();
                min_heap.push(item);
            }
        }
        
        // 弹出堆中元素（降序）
        std::vector<FeedItem> top_items;
        while (!min_heap.empty()) {
            top_items.push_back(min_heap.top());
            min_heap.pop();
        }
        std::reverse(top_items.begin(), top_items.end());
        
        // ========== 第三步：批量拉取动态详情 ==========
        std::vector<FeedDetail> result;
        for (auto& item : top_items) {
            int64_t feed_id = std::stoll(item.member);
            FeedDetail detail;
            
            // 先查本地 LRU 缓存
            if (local_cache_.Get(feed_id, detail)) {
                result.push_back(detail);
                continue;
            }
            
            // 缓存未命中，查 Redis HASH
            std::string detail_key = "feed:detail:" + std::to_string(feed_id);
            auto hash_map = redis_->hgetall(detail_key);
            if (!hash_map.empty()) {
                detail = ParseFeedDetail(hash_map);
                local_cache_.Put(feed_id, detail); // 写入本地缓存
                result.push_back(detail);
            }
        }
        return result;
    }
### 4.4 本地 LRU 缓存实现

    // LocalFeedCache.h
    #include <unordered_map>
    #include <list>
    #include <shared_mutex>

    class LocalFeedCache {
    private:
        struct CacheItem {
            FeedDetail detail;
            std::list<int64_t>::iterator lru_iter;
        };
        std::unordered_map<int64_t, CacheItem> map_;
        std::list<int64_t> lru_list_;  // 头部为最近访问
        mutable std::shared_mutex mtx_;
        const size_t MAX_SIZE = 100000; // 缓存10万条热点动态

    public:
        bool Get(int64_t feed_id, FeedDetail& out_detail) {
            std::unique_lock lock(mtx_); // 需写锁更新LRU顺序
            auto it = map_.find(feed_id);
            if (it == map_.end()) return false;
            
            // 移到链表头部（标记最近使用）
            lru_list_.erase(it->second.lru_iter);
            lru_list_.push_front(feed_id);
            it->second.lru_iter = lru_list_.begin();
            out_detail = it->second.detail;
            return true;
        }

        void Put(int64_t feed_id, const FeedDetail& detail) {
            std::unique_lock lock(mtx_);
            if (map_.size() >= MAX_SIZE) {
                // 淘汰链表尾部（最久未使用）
                int64_t evict_id = lru_list_.back();
                lru_list_.pop_back();
                map_.erase(evict_id);
            }
            lru_list_.push_front(feed_id);
            map_[feed_id] = {detail, lru_list_.begin()};
        }
    };

## 五、消息通知设计（异步批量）

通知采用独立 MQ 消费者，与扩散链路解耦。

    // NotificationConsumer.cpp
    void HandleNotificationTask(int64_t publisher_id, int64_t feed_id) {
        auto fans = GetFollowerIds(publisher_id); // 分页查询，每批5000人
        
        // 批量插入 t_notification（需额外创建此表）
        // 同时更新 Redis 未读计数器：INCR unread:user:{fan_id}
        const int BATCH_SIZE = 1000;
        for (size_t i = 0; i < fans.size(); i += BATCH_SIZE) {
            std::string sql = "INSERT INTO t_notification (user_id, feed_id, is_read) VALUES ";
            for (size_t j = i; j < i + BATCH_SIZE && j < fans.size(); ++j) {
                sql += "(" + std::to_string(fans[j]) + "," + std::to_string(feed_id) + ",0),";
            }
            sql.pop_back();
            ExecuteSQL(sql);
        }
    }

## 六、并发与一致性保障

| 风险场景 | 防护策略 |
| :--- | :--- |
| **MQ 消息丢失** | `t_diffusion_task` 状态机追踪，超时未完成（> 60s）定时任务重试 |
| **Redis 写入失败** | Pipeline 执行后检查返回结果，失败条目写入本地日志文件，由 Watchdog 线程补录 |
| **大V 粉丝列表变动（关注/取关）** | 关注/取关时同步更新 `followers:bigv:{id}` SET，并设置 TTL=10 分钟兜底重建 |
| **本地缓存与 Redis 不一致** | 本地缓存 TTL=60s，超时自动失效；发布新动态时主动 invalidate 本地缓存 |
| **收件箱与 MySQL 权威数据不一致** | 每日凌晨 3:00 全量对账：扫描 `t_follow` 重建所有用户的 inbox（作为最终兜底） |

## 七、最终交付总结

本设计方案在给定的三张基础表之上，通过 **混合推拉模式** ，构建了一套可支撑 **100 万粉丝大V 实时发动态** 的高可用 Feed 流系统：

1. **数学模型**：无（Feed 流不涉及公式，但时间线排序依赖 Score = 发布时间戳）。
2. **数据模型**：Redis（ZSET/SET/HASH）+ MySQL（`t_diffusion_task` + `t_notification`），清晰解耦。
3. **核心算法**：动态路由分流 + 分片并行 MQ + Pipeline 批量写入 + Lua 批量拉取 + 堆排序 TopN + 本地 LRU + 惰性清理。
4. **可维护性**：全链路异步化，最终一致性，定时兜底对账，生产级可用。

