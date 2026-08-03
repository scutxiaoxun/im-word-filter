// =========================================================================
// filter_sdk.cpp - SDK 导出包装器
// 功能：将核心引擎封装为 C 动态库接口，供外部系统嵌入调用
// =========================================================================

#include "filter_api.h"

// 直接包含核心引擎实现（注意：task1_security.cpp 中的 main 已被 BUILD_DLL 宏屏蔽）
#include "task1_security.cpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <cstdio>

// ---------- 全局状态管理 ----------
static std::atomic<bool> g_sdk_running{ false };   // 引擎是否已初始化
static std::thread g_sdk_hotThread;              // 热更新后台线程
static std::mutex g_sdk_initMutex;               // 保护初始化和销毁的重入

// ---------- 接口实现 ----------

FILTER_API int Filter_Init(const char* config_dir) {
    std::lock_guard<std::mutex> lock(g_sdk_initMutex);

    if (g_sdk_running.load()) {
        return FILTER_ERROR_INTERNAL; // 已初始化，防止重复调用
    }

    try {
        // 1. 切换工作目录（使相对路径配置生效）
        if (config_dir && config_dir[0] != '\0') {
            fs::current_path(config_dir);
        }

        // 2. 加载外部配置（config.txt）
        ConfigLoader::loadConfig("config.txt");

        // 3. 初始化核心校验器（加载词库、组合规则、正则）
        initValidator();

        // 4. 启动热更新后台线程（零停机重载）
        g_sdk_running.store(true);
        g_sdk_hotThread = std::thread([] {
            while (g_sdk_running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(CONFIG.hotReloadIntervalSeconds));
                if (CONFIG.enableHotReload) {
                    ConfigLoader::checkAndReload("config.txt");
                    reloadValidator();
                }
            }
            });

        return FILTER_OK;
    }
    catch (const std::exception&) {
        return FILTER_ERROR_INTERNAL;
    }
}

FILTER_API int Filter_Check(const char* text, char* err_msg, int err_msg_size) {
    // 1. 空文本直接视为安全（与原业务逻辑一致）
    if (!text || text[0] == '\0') {
        return FILTER_OK;
    }

    // 2. 检查引擎是否已初始化
    if (!g_sdk_running.load()) {
        return FILTER_ERROR_NOT_INIT;
    }

    try {
        // 3. 获取当前校验器实例（线程安全）
        auto validator = getValidator();
        if (!validator) {
            return FILTER_ERROR_INTERNAL;
        }

        // 4. 执行检测
        std::string errMsg;
        bool isSafe = validator->validate(std::string(text), errMsg);

        // 5. 如果被拦截且用户提供了缓冲区，写入拦截原因（跨平台安全复制）
        if (!isSafe && err_msg && err_msg_size > 0) {
            // 使用 snprintf 确保不溢出，且自动添加 '\0' 终止符
            snprintf(err_msg, err_msg_size, "%s", errMsg.c_str());
        }

        return isSafe ? FILTER_OK : FILTER_BLOCKED;
    }
    catch (const std::exception&) {
        return FILTER_ERROR_INTERNAL;
    }
}

FILTER_API int Filter_Reload(void) {
    if (!g_sdk_running.load()) {
        return FILTER_ERROR_NOT_INIT;
    }
    try {
        reloadValidator();
        return FILTER_OK;
    }
    catch (...) {
        return FILTER_ERROR_INTERNAL;
    }
}

FILTER_API void Filter_Destroy(void) {
    std::lock_guard<std::mutex> lock(g_sdk_initMutex);

    if (!g_sdk_running.load()) return;

    // 1. 停止热更新线程
    g_sdk_running.store(false);
    if (g_sdk_hotThread.joinable()) {
        g_sdk_hotThread.join();
    }

    // 2. 关闭异步日志（等待日志队列刷盘完成）
    AsyncLogger::instance().shutdown();

    // 3. 释放全局校验器（引用计数归零，自动析构）
    g_validator.reset();
}