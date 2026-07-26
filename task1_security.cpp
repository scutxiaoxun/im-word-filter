 // =========================================================================
 // 编译命令：
 //   平衡版（unordered_map）：g++ -std=c++17 -O2 -Wall -pthread task1_security.cpp -o task1.exe
 //   高速版（固定数组）：     g++ -std=c++17 -O2 -Wall -pthread -DUSE_FIXED_ARRAY task1_security.cpp -o task1_fast.exe
 // =========================================================================

#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <regex>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <condition_variable>
#include <unordered_map>
#include <queue>
#include <cstring>

using namespace std;
namespace fs = std::filesystem;

// =========================================================================
// 第一部分：全局配置结构体
// =========================================================================
struct SecurityConfig {
    bool enableLengthLimit = true;
    bool enableFormatCheck = true;
    bool enableAC = true;
    bool enableCombinationRule = true;
    bool enableXSS = true;
    bool enableSQL = true;
    bool enableLooseSQLCheck = true;   // true=宽松（不拦截普通SELECT）, false=严格

    int  maxInputChars = 100;
    int  maxInputBytes = 5000;
    bool strictBinaryReject = true;

    string sensitiveWordsFolder = "sensitive_words";
    string combinationRulesFolder = "combination_rules";
    string regexRulesFile = "regex_rules.txt";
    string logFilePath = "security.log";

    bool enableHotReload = true;
    int  hotReloadIntervalSeconds = 30;

    bool recursiveScan = false;
    string fileExtension = ".txt";
};

// 全局配置实例
SecurityConfig CONFIG;

// =========================================================================
// 第二部分：配置加载器（外部化所有配置项）
// =========================================================================
class ConfigLoader {
private:
    static mutex configMutex;
    static filesystem::file_time_type lastLoadTime;

public:
    // 加载配置文件
    static bool loadConfig(const string& filepath = "config.txt") {
        lock_guard<mutex> lock(configMutex);
        ifstream file(filepath);
        if (!file.is_open()) {
            cerr << "无法打开配置文件: " << filepath << "，使用默认配置。" << endl;
            return false;
        }

        string line;
        int lineNum = 0;
        while (getline(file, line)) {
            lineNum++;
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            string trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            size_t eqPos = trimmed.find('=');
            if (eqPos == string::npos) {
                cerr << " 第 " << lineNum << " 行格式错误（缺少 '='），跳过: " << trimmed << endl;
                continue;
            }
            string key = trimmed.substr(0, eqPos);
            string value = trimmed.substr(eqPos + 1);
            // 去除首尾空格
            key.erase(remove_if(key.begin(), key.end(), ::isspace), key.end());
            value.erase(remove_if(value.begin(), value.end(), ::isspace), value.end());

            // 如果 value 以引号开头和结尾，去掉引号（用于保留字符串中的空格）
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }

            // 逐个字段赋值
            bool found = false;
            found |= setBool(key, value, "enableLengthLimit", CONFIG.enableLengthLimit);
            found |= setBool(key, value, "enableFormatCheck", CONFIG.enableFormatCheck);
            found |= setBool(key, value, "enableAC", CONFIG.enableAC);
            found |= setBool(key, value, "enableCombinationRule", CONFIG.enableCombinationRule);
            found |= setBool(key, value, "enableXSS", CONFIG.enableXSS);
            found |= setBool(key, value, "enableSQL", CONFIG.enableSQL);
            found |= setBool(key, value, "enableLooseSQLCheck", CONFIG.enableLooseSQLCheck);
            found |= setBool(key, value, "strictBinaryReject", CONFIG.strictBinaryReject);
            found |= setBool(key, value, "enableHotReload", CONFIG.enableHotReload);
            found |= setBool(key, value, "recursiveScan", CONFIG.recursiveScan);

            found |= setInt(key, value, "maxInputChars", CONFIG.maxInputChars);
            found |= setInt(key, value, "maxInputBytes", CONFIG.maxInputBytes);
            found |= setInt(key, value, "hotReloadIntervalSeconds", CONFIG.hotReloadIntervalSeconds);

            found |= setString(key, value, "sensitiveWordsFolder", CONFIG.sensitiveWordsFolder);
            found |= setString(key, value, "combinationRulesFolder", CONFIG.combinationRulesFolder);
            found |= setString(key, value, "regexRulesFile", CONFIG.regexRulesFile);
            found |= setString(key, value, "logFilePath", CONFIG.logFilePath);
            found |= setString(key, value, "fileExtension", CONFIG.fileExtension);

            if (!found) {
                cerr << "未知配置项: " << key << "（已忽略）" << endl;
            }
        }
        file.close();

        // 记录加载时间（用于热更新）
        lastLoadTime = filesystem::last_write_time(filepath);
        cout << "配置文件 " << filepath << " 加载成功。" << endl;
        return true;
    }

    // 检查配置文件是否发生变化（用于热更新）
    static bool checkAndReload(const string& filepath = "config.txt") {
        if (!filesystem::exists(filepath)) return false;
        auto currentTime = filesystem::last_write_time(filepath);
        if (currentTime != lastLoadTime) {
            cout << "检测到配置文件变化，重新加载..." << endl;
            return loadConfig(filepath);
        }
        return false;
    }

private:
    static bool setBool(const string& key, const string& value, const string& fieldName, bool& target) {
        if (key != fieldName) return false;
        string lower = value;
        transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "true" || lower == "yes" || lower == "1") {
            target = true;
        }
        else if (lower == "false" || lower == "no" || lower == "0") {
            target = false;
        }
        else {
            cerr << "配置 " << fieldName << " 的值 '" << value << "' 无法解析为 bool，保持原值。" << endl;
        }
        return true;
    }

    static bool setInt(const string& key, const string& value, const string& fieldName, int& target) {
        if (key != fieldName) return false;
        try {
            target = stoi(value);
        }
        catch (...) {
            cerr << "配置 " << fieldName << " 的值 '" << value << "' 无法解析为 int，保持原值。" << endl;
        }
        return true;
    }

    static bool setString(const string& key, const string& value, const string& fieldName, string& target) {
        if (key != fieldName) return false;
        target = value;
        return true;
    }
};

// 静态成员初始化
mutex ConfigLoader::configMutex;
filesystem::file_time_type ConfigLoader::lastLoadTime = filesystem::file_time_type::min();

/*
 ============================================================================
 异步日志器与基础工具函数
 说明：
   - 提供异步非阻塞日志、格式检测、时间戳生成和 JSON 日志写入。
 ============================================================================
 */

 // =========================================================================
 // 异步日志器
 // =========================================================================
class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    // 停止日志线程（程序退出时调用）
    void shutdown() {
        {
            lock_guard<mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }

    // 异步写入日志（非阻塞）
    void log(const string& msg) {
        {
            lock_guard<mutex> lock(m_mutex);
            if (m_stop) return;
            if (m_queue.size() > 10000) { 
                if (!m_queue.empty()) m_queue.pop();
            }
            m_queue.push(msg);
        }
        m_cv.notify_one();
    }

    // 禁止拷贝和赋值
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

private:
    AsyncLogger() : m_stop(false), m_thread(&AsyncLogger::worker, this) {}

    ~AsyncLogger() {
        shutdown();
    }

    void worker() {
        while (true) {
            vector<string> batch;  // 批量写入，提高效率
            {
                unique_lock<mutex> lock(m_mutex);
                // 等待条件：有数据或需要停止
                m_cv.wait(lock, [this] { return !m_queue.empty() || m_stop; });

                // 取出所有待写入的日志（最多一次取1000条，避免内存占用过大）
                size_t count = 0;
                while (!m_queue.empty() && count < 1000) {
                    batch.push_back(std::move(m_queue.front()));
                    m_queue.pop();
                    count++;
                }
            }

            // 批量写入文件（如果没有数据且停止，则退出循环）
            if (!batch.empty()) {
                writeBatch(batch);
            }

            if (m_stop && m_queue.empty()) {
                break; // 停止信号且队列为空，退出线程
            }
        }
    }

    // 批量写入文件
    void writeBatch(const vector<string>& batch) {
        if (batch.empty()) return;
        ofstream logFile(CONFIG.logFilePath, ios::app);
        if (!logFile.is_open()) return;
        for (const string& msg : batch) {
            logFile << msg;
        }
        logFile.flush();
    }

    queue<string> m_queue;
    mutex m_mutex;
    condition_variable m_cv;
    bool m_stop;
    thread m_thread;
};

// =========================================================================
// 工具函数（格式检测、时间、日志，通用）
// =========================================================================

/*
 * 格式检测：只接受纯文本，拒绝包含图片标签、Base64 图片、二进制控制字符
 * 返回 true 表示格式合法（纯文本），false 表示包含非文本内容
 */
bool isPlainText(const string& input) {
    static regex imgTagRegex("<\\s*(img|image)\\s+", regex::icase | regex::optimize);
    if (regex_search(input, imgTagRegex)) return false;
    static regex dataImageRegex("data:\\s*image\\/", regex::icase | regex::optimize);
    if (regex_search(input, dataImageRegex)) return false;
    if (CONFIG.strictBinaryReject) {
        for (unsigned char c : input) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
        }
    }
    return true;
}

/*
 * 获取当前时间字符串（ISO 8601 格式）
 */
string getCurrentTimeISO() {
    auto now = chrono::system_clock::now();
    auto in_time_t = chrono::system_clock::to_time_t(now);
    struct tm bt;
#ifdef _WIN32
    localtime_s(&bt, &in_time_t);
#else
    localtime_r(&in_time_t, &bt);
#endif
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &bt);
    return string(buffer);
}

/*
 * 结构化日志写入（JSON 格式，异步）
 * 参数：
 *   level   - 日志级别（如 "BLOCK", "ALLOW"）
 *   reason  - 拦截原因（如 "AC_DETECTED", "XSS"）
 *   input   - 原始输入文本（自动转义引号和反斜杠）
 *   rule    - 命中的具体规则名称（可选）
 */
void writeLog(const string& level, const string& reason, const string& input, const string& rule = "") {
    if (CONFIG.logFilePath.empty()) return;
    string inputEscaped = input;
    size_t pos = 0;
    while ((pos = inputEscaped.find('"', pos)) != string::npos) {
        inputEscaped.replace(pos, 1, "\\\"");
        pos += 2;
    }
    while ((pos = inputEscaped.find('\\', pos)) != string::npos) {
        inputEscaped.replace(pos, 1, "\\\\");
        pos += 2;
    }
    string json = "{\"time\":\"" + getCurrentTimeISO() + "\","
        + "\"level\":\"" + level + "\","
        + "\"reason\":\"" + reason + "\","
        + "\"input\":\"" + inputEscaped + "\","
        + "\"rule\":\"" + rule + "\"}\n";
    AsyncLogger::instance().log(json);
}

/*
 ============================================================================
 扫描结果结构体与 AC 自动机类声明（宏控制点）
 说明：
   - 本模块包含 AC 自动机类的前置声明和节点结构体定义。
   - 使用 #ifdef USE_FIXED_ARRAY 控制子节点存储方式：
       * 定义宏时：使用 int next[256]（快速版，空间换时间）
       * 未定义宏时：使用 unordered_map<char, int> next（平衡版，内存优化）
   - 其余成员（fail、output、comboRuleIds）与宏无关。
   - 本模块仅声明类和方法，实现在下一模块中提供。
 ============================================================================
 */

 // =========================================================================
 // 扫描结果结构体
 // =========================================================================
struct ScanResult {
    vector<string> words;   // 命中的敏感词列表
    size_t charCount;       // 输入文本的 UTF-8 字符数（在扫描时一并统计）
};

// =========================================================================
// AC 自动机类
// =========================================================================
class AhoCorasick {
private:
    // ---------- 节点结构体（宏控制点） ----------
    struct Node {
        // 子节点存储：根据宏选择不同数据结构
#ifdef USE_FIXED_ARRAY
        int next[256];          // 固定数组，下标为字符的 unsigned char 值
#else
        unordered_map<char, int> next;  // 哈希表，仅存储存在的边
#endif

        int fail;                   // 失败指针
        vector<int> output;         // 敏感词索引列表
        vector<int> comboRuleIds;   // 组合规则 ID 列表

        // 构造函数
        Node() : fail(0) {
#ifdef USE_FIXED_ARRAY
            memset(next, -1, sizeof(next)); // 初始化为 -1（表示不存在）
#endif
            // unordered_map 默认构造为空
        }
    };

    vector<Node> trie;              // Trie 树所有节点
    vector<string> words;           // 敏感词词库（用于输出）
    bool isBuilt;                   // 是否已构建失败指针
    unordered_map<int, int> ruleHitCount; // 当前扫描的规则命中计数（通用，与存储方式无关）

public:
    AhoCorasick();

    // 插入敏感词
    void insertWord(const string& word);

    // 插入组合关键词（关联规则 ID）
    void insertComboKeyword(const string& word, int ruleId);

    // 构建失败指针（同时合并输出和规则 ID）
    void build();

    // 扫描文本，返回 ScanResult（包含命中词和字符数）
    ScanResult scan(const string& text);

    // 获取规则命中计数（只读）
    const unordered_map<int, int>& getRuleHitCount() const;

    // 获取词库大小
    size_t wordCount() const;
};

/*
 ============================================================================
 AC 自动机类的完整实现（宏控制访问逻辑）
 说明：
   - 本模块提供 AhoCorasick 类的所有方法定义。
   - 所有对 trie[node].next 的访问均根据宏 USE_FIXED_ARRAY 适配：
       * 快速版（宏定义）：直接使用 int 下标访问（trie[state].next[uc]）
       * 平衡版（宏未定义）：使用 unordered_map 的 find/insert 操作
 ============================================================================
 */

 // 构造函数
AhoCorasick::AhoCorasick() : isBuilt(false) {
    trie.emplace_back(); // 创建根节点（索引 0）
}

// 插入敏感词
void AhoCorasick::insertWord(const string& word) {
    if (word.empty()) return;
    isBuilt = false;
    int nodeIdx = 0;
    for (char ch : word) {
        unsigned char uc = static_cast<unsigned char>(ch);
#ifdef USE_FIXED_ARRAY
        if (trie[nodeIdx].next[uc] == -1) {
            trie[nodeIdx].next[uc] = static_cast<int>(trie.size());
            trie.emplace_back();
        }
        nodeIdx = trie[nodeIdx].next[uc];
#else
        auto it = trie[nodeIdx].next.find(static_cast<char>(uc));
        if (it == trie[nodeIdx].next.end()) {
            int newIdx = static_cast<int>(trie.size());
            trie[nodeIdx].next[static_cast<char>(uc)] = newIdx;
            trie.emplace_back();
            nodeIdx = newIdx;
        }
        else {
            nodeIdx = it->second;
        }
#endif
    }
    trie[nodeIdx].output.push_back(static_cast<int>(words.size()));
    words.push_back(word);
}

// 插入组合关键词（关联规则 ID）
void AhoCorasick::insertComboKeyword(const string& word, int ruleId) {
    if (word.empty()) return;
    isBuilt = false;
    int nodeIdx = 0;
    for (char ch : word) {
        unsigned char uc = static_cast<unsigned char>(ch);
#ifdef USE_FIXED_ARRAY
        if (trie[nodeIdx].next[uc] == -1) {
            trie[nodeIdx].next[uc] = static_cast<int>(trie.size());
            trie.emplace_back();
        }
        nodeIdx = trie[nodeIdx].next[uc];
#else
        auto it = trie[nodeIdx].next.find(static_cast<char>(uc));
        if (it == trie[nodeIdx].next.end()) {
            int newIdx = static_cast<int>(trie.size());
            trie[nodeIdx].next[static_cast<char>(uc)] = newIdx;
            trie.emplace_back();
            nodeIdx = newIdx;
        }
        else {
            nodeIdx = it->second;
        }
#endif
    }
    auto& ids = trie[nodeIdx].comboRuleIds;
    if (find(ids.begin(), ids.end(), ruleId) == ids.end()) {
        ids.push_back(ruleId);
    }
}

// 构建失败指针（同时合并输出和规则 ID）
void AhoCorasick::build() {
    queue<int> q;
    // 初始化第一层节点（根节点的直接子节点）
#ifdef USE_FIXED_ARRAY
    for (int c = 0; c < 256; ++c) {
        int child = trie[0].next[c];
        if (child != -1) {
            trie[child].fail = 0;
            q.push(child);
        }
    }
#else
    for (auto& pair : trie[0].next) {
        int child = pair.second;
        trie[child].fail = 0;
        q.push(child);
    }
#endif

    while (!q.empty()) {
        int curr = q.front(); q.pop();
        // 遍历当前节点的所有子节点
#ifdef USE_FIXED_ARRAY
        for (int c = 0; c < 256; ++c) {
            int child = trie[curr].next[c];
            if (child == -1) continue;
            // 计算 child 的失败指针
            int failState = trie[curr].fail;
            while (failState != 0 && trie[failState].next[c] == -1) {
                failState = trie[failState].fail;
            }
            if (trie[failState].next[c] != -1) {
                trie[child].fail = trie[failState].next[c];
            }
            else {
                trie[child].fail = 0;
            }
            // 合并输出
            auto& failOutput = trie[trie[child].fail].output;
            trie[child].output.insert(trie[child].output.end(),
                failOutput.begin(), failOutput.end());
            // 合并规则 ID
            auto& failIds = trie[trie[child].fail].comboRuleIds;
            trie[child].comboRuleIds.insert(trie[child].comboRuleIds.end(),
                failIds.begin(), failIds.end());
            // 去重
            auto& ids = trie[child].comboRuleIds;
            sort(ids.begin(), ids.end());
            ids.erase(unique(ids.begin(), ids.end()), ids.end());
            q.push(child);
        }
#else
        for (auto& pair : trie[curr].next) {
            int child = pair.second;
            // 计算 child 的失败指针
            int failState = trie[curr].fail;
            while (failState != 0 && trie[failState].next.find(pair.first) == trie[failState].next.end()) {
                failState = trie[failState].fail;
            }
            auto it = trie[failState].next.find(pair.first);
            if (it != trie[failState].next.end()) {
                trie[child].fail = it->second;
            }
            else {
                trie[child].fail = 0;
            }
            // 合并输出
            auto& failOutput = trie[trie[child].fail].output;
            trie[child].output.insert(trie[child].output.end(),
                failOutput.begin(), failOutput.end());
            // 合并规则 ID
            auto& failIds = trie[trie[child].fail].comboRuleIds;
            trie[child].comboRuleIds.insert(trie[child].comboRuleIds.end(),
                failIds.begin(), failIds.end());
            // 去重
            auto& ids = trie[child].comboRuleIds;
            sort(ids.begin(), ids.end());
            ids.erase(unique(ids.begin(), ids.end()), ids.end());
            q.push(child);
        }
#endif
    }
    isBuilt = true;
}

// 扫描文本，返回 ScanResult（包含命中词和字符数）
ScanResult AhoCorasick::scan(const string& text) {
    if (!isBuilt) build();
    ruleHitCount.clear();

    ScanResult result;
    result.charCount = 0;
    int state = 0;

    for (char ch : text) {
        unsigned char uc = static_cast<unsigned char>(ch);
        // 统计 UTF-8 字符数
        if ((uc & 0xC0) != 0x80) {
            result.charCount++;
        }

        // AC 匹配（关键：根据宏选择不同的查找方式）
#ifdef USE_FIXED_ARRAY
        while (state != 0 && trie[state].next[uc] == -1) {
            state = trie[state].fail;
        }
        if (trie[state].next[uc] != -1) {
            state = trie[state].next[uc];
        }
        else {
            state = 0;
        }
#else
        while (state != 0 && trie[state].next.find(static_cast<char>(uc)) == trie[state].next.end()) {
            state = trie[state].fail;
        }
        auto it = trie[state].next.find(static_cast<char>(uc));
        if (it != trie[state].next.end()) {
            state = it->second;
        }
        else {
            state = 0;
        }
#endif

        // 处理命中的敏感词
        if (!trie[state].output.empty()) {
            for (int idx : trie[state].output) {
                result.words.push_back(words[idx]);
            }
        }

        // 处理命中的组合规则
        if (!trie[state].comboRuleIds.empty()) {
            for (int ruleId : trie[state].comboRuleIds) {
                ruleHitCount[ruleId]++;
            }
        }
    }

    sort(result.words.begin(), result.words.end());
    result.words.erase(unique(result.words.begin(), result.words.end()), result.words.end());
    return result;
}

// 获取规则命中计数
const unordered_map<int, int>& AhoCorasick::getRuleHitCount() const {
    return ruleHitCount;
}

// 获取词库大小
size_t AhoCorasick::wordCount() const {
    return words.size();
}

/*
 ============================================================================
 组合规则结构与校验器核心类（通用）
 说明：
   - 本模块提供 CombinationRule 结构体定义和 TextSecurityValidator 类。
   - 所有逻辑与 AC 存储方式无关，仅调用 AhoCorasick 的公开接口。
   - 包含外部正则加载、长度检测、格式检测、AC 扫描、组合规则、XSS、SQL 等完整流程。
 ============================================================================
 */

 // =========================================================================
 // 组合规则结构（通用）
 // =========================================================================
struct CombinationRule {
    vector<string> keywords;   // 关键词列表（如 {"甲", "乙", "丙"}）
    int minMatch;              // 触发阈值（最少同时出现几个关键词）
    string ruleName;           // 规则名称（用于日志）
    int ruleId;                // 规则唯一 ID（与 AC 中的计数关联）
};

// =========================================================================
// 校验器核心类（通用，无宏依赖）
// =========================================================================
class TextSecurityValidator {
private:
    shared_ptr<AhoCorasick> acMachine;       // AC 自动机引擎
    vector<CombinationRule> comboRules;      // 组合规则列表
    regex xssPattern;                        // XSS 正则
    regex sqlPattern;                        // SQL 严格模式正则
    regex sqlLoosePattern;                   // SQL 宽松模式正则
    bool regexLoaded;                        // 正则是否成功加载

    // 辅助：从文件加载正则表达式（支持外部配置）
    bool loadRegexFromFile(const string& filepath) {
        ifstream file(filepath);
        if (!file.is_open()) {
            cerr << "无法打开正则配置文件: " << filepath << "，使用默认规则。" << endl;
            // 使用原始字符串字面量，避免转义问题
            xssPattern.assign(R"((<\s*script|on\w+\s*=|javascript\s*:|<\s*iframe|eval\s*\())",
                regex::icase | regex::optimize);
            sqlPattern.assign(R"((select\s+\S+\s+from|drop\s+table|union\s+select|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b))",
                regex::icase | regex::optimize);
            sqlLoosePattern.assign(R"((union\s+select|drop\s+table|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b|\bxp_cmdshell\b))",
                regex::icase | regex::optimize);
            regexLoaded = true;
            cout << "使用内置默认正则规则。" << endl;
            return true;
        }

        string line;
        string xssPatternStr, sqlPatternStr, sqlLoosePatternStr;
        while (getline(file, line)) {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            string trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == '#') continue;
            size_t eqPos = trimmed.find('=');
            if (eqPos == string::npos) continue;
            string key = trimmed.substr(0, eqPos);
            string value = trimmed.substr(eqPos + 1);
            key.erase(remove_if(key.begin(), key.end(), ::isspace), key.end());
            value.erase(remove_if(value.begin(), value.end(), ::isspace), value.end());
            if (key == "xss_pattern") xssPatternStr = value;
            else if (key == "sql_pattern") sqlPatternStr = value;
            else if (key == "sql_loose_pattern") sqlLoosePatternStr = value;
        }
        file.close();

        if (xssPatternStr.empty()) {
            xssPatternStr = R"((<\s*script|on\w+\s*=|javascript\s*:|<\s*iframe|eval\s*\())";
            cout << "未配置 xss_pattern，使用默认。" << endl;
        }
        if (sqlPatternStr.empty()) {
            sqlPatternStr = R"((select\s+\S+\s+from|drop\s+table|union\s+select|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b))";
            cout << "未配置 sql_pattern，使用默认严格规则。" << endl;
        }
        if (sqlLoosePatternStr.empty()) {
            sqlLoosePatternStr = R"((union\s+select|drop\s+table|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b|\bxp_cmdshell\b))";
            cout << "未配置 sql_loose_pattern，使用默认宽松规则。" << endl;
        }

        try {
            xssPattern.assign(xssPatternStr, regex::icase | regex::optimize);
            sqlPattern.assign(sqlPatternStr, regex::icase | regex::optimize);
            sqlLoosePattern.assign(sqlLoosePatternStr, regex::icase | regex::optimize);
            regexLoaded = true;
            cout << "正则表达式从 " << filepath << " 加载成功。" << endl;
            cout << "   xss_pattern = " << xssPatternStr << endl;
            cout << "   sql_pattern = " << sqlPatternStr << endl;
            cout << "   sql_loose_pattern = " << sqlLoosePatternStr << endl;
            return true;
        }
        catch (const regex_error& e) {
            cerr << "正则表达式编译失败: " << e.what() << "，使用默认规则。" << endl;
            // 回退到内置默认
            xssPattern.assign(R"((<\s*script|on\w+\s*=|javascript\s*:|<\s*iframe|eval\s*\())",
                regex::icase | regex::optimize);
            sqlPattern.assign(R"((select\s+\S+\s+from|drop\s+table|union\s+select|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b))",
                regex::icase | regex::optimize);
            sqlLoosePattern.assign(R"((union\s+select|drop\s+table|--|'\s+or\s+'1'\s*=\s*'1|;\s*--|\bexec\b|\bxp_cmdshell\b))",
                regex::icase | regex::optimize);
            regexLoaded = true;
            return false;
        }
    }

public:
    // 构造：传入 AC 引擎和组合规则列表
    TextSecurityValidator(shared_ptr<AhoCorasick> ac, const vector<CombinationRule>& rules)
        : acMachine(ac), comboRules(rules), regexLoaded(false) {
        loadRegexFromFile(CONFIG.regexRulesFile);
    }

    // 默认构造（空引擎，用于异常情况）
    TextSecurityValidator() : acMachine(make_shared<AhoCorasick>()), regexLoaded(false) {
        loadRegexFromFile(CONFIG.regexRulesFile);
    }

    // 核心校验函数
    bool validate(const string& input, string& errMsg) {
        errMsg.clear();
        if (input.empty()) return true;

        // 1. 字节长度快速检查
        if (CONFIG.enableLengthLimit) {
            if (input.length() > static_cast<size_t>(CONFIG.maxInputBytes)) {
                errMsg = "Input exceeds max byte limit (" + to_string(CONFIG.maxInputBytes) + " bytes)";
                writeLog("BLOCK", "BYTE_LIMIT", input, "");
                return false;
            }
        }

        // 2. 格式检测
        if (CONFIG.enableFormatCheck) {
            if (!isPlainText(input)) {
                errMsg = "Input contains non-text content (e.g., image tags or binary data)";
                writeLog("BLOCK", "FORMAT_INVALID", input, "");
                return false;
            }
        }

        // 3. AC 扫描（敏感词 + 组合规则计数 + 字符数统计）
        ScanResult scanResult;
        if (CONFIG.enableAC || CONFIG.enableCombinationRule) {
            scanResult = acMachine->scan(input);
        }
        else {
            // 两者都禁用时，仅统计字符数用于长度限制
            scanResult.charCount = 0;
            for (unsigned char c : input) {
                if ((c & 0xC0) != 0x80) scanResult.charCount++;
            }
        }

        // 4. 字符数限制检查
        if (CONFIG.enableLengthLimit) {
            if (scanResult.charCount > static_cast<size_t>(CONFIG.maxInputChars)) {
                errMsg = "Input exceeds max character limit (" + to_string(CONFIG.maxInputChars) + " chars)";
                writeLog("BLOCK", "LENGTH_LIMIT", input, "");
                return false;
            }
        }

        // 5. 敏感词检测
        if (CONFIG.enableAC && !scanResult.words.empty()) {
            string rule = "AC: ";
            for (const string& w : scanResult.words) rule += w + ",";
            errMsg = "Sensitive words detected: " + rule;
            writeLog("BLOCK", "AC_DETECTED", input, rule);
            return false;
        }

        // 6. 组合规则检测
        if (CONFIG.enableCombinationRule) {
            const auto& hitCount = acMachine->getRuleHitCount();
            for (const CombinationRule& rule : comboRules) {
                auto it = hitCount.find(rule.ruleId);
                int matchCount = (it != hitCount.end()) ? it->second : 0;
                if (matchCount >= rule.minMatch) {
                    errMsg = "Combined sensitive pattern triggered: [" + rule.ruleName + "] " +
                        "(matched " + to_string(matchCount) + "/" + to_string(rule.keywords.size()) + " keywords)";
                    writeLog("BLOCK", "COMBINATION", input, rule.ruleName);
                    return false;
                }
            }
        }

        // 7. XSS 检测
        if (CONFIG.enableXSS && regexLoaded) {
            smatch match;
            if (regex_search(input, match, xssPattern)) {
                errMsg = "Potential XSS attack detected: " + match.str();
                writeLog("BLOCK", "XSS", input, match.str());
                return false;
            }
        }

        // 8. SQL 检测（支持宽松/严格模式）
        if (CONFIG.enableSQL && regexLoaded) {
            smatch match;
            bool sqlDetected = false;
            if (CONFIG.enableLooseSQLCheck) {
                sqlDetected = regex_search(input, match, sqlLoosePattern);
            }
            else {
                sqlDetected = regex_search(input, match, sqlPattern);
            }
            if (sqlDetected) {
                errMsg = "Potential SQL Injection detected: " + match.str();
                writeLog("BLOCK", "SQL_INJECTION", input, match.str());
                return false;
            }
        }

        return true;
    }

    // 获取词库词条数
    size_t getWordCount() const {
        return acMachine ? acMachine->wordCount() : 0;
    }

    // 获取组合规则数
    size_t getRuleCount() const {
        return comboRules.size();
    }

    // 热更新：替换 AC 引擎和规则列表
    void reload(shared_ptr<AhoCorasick> newAc, const vector<CombinationRule>& newRules) {
        acMachine = newAc;
        comboRules = newRules;
        loadRegexFromFile(CONFIG.regexRulesFile);
    }
};

/*
 ============================================================================
 外部数据加载函数（通用）
 说明：
   - 本模块提供从文件夹加载敏感词库和组合规则的函数。
   - 敏感词直接插入 AC 自动机（调用 insertWord）。
   - 组合规则解析时，同时将每个关键词插入 AC 自动机（调用 insertComboKeyword），并记录规则 ID。
   - 所有路径和扩展名由 CONFIG 统一控制。
   - 本模块不涉及 AC 存储方式的宏控制，仅使用公开接口。
 ============================================================================
 */

 // =========================================================================
 // 外部数据加载函数
 // =========================================================================

 /*
  * 加载敏感词库（仅 AC 词库，不包含组合关键词）
  * 参数：folderPath - 文件夹路径（相对或绝对）
  * 返回值：构建好的 AhoCorasick 对象（共享指针）
  * 格式：文件夹内所有 .txt 文件，每行一个词，空行或首尾空格自动跳过
  */
shared_ptr<AhoCorasick> loadACFromFolder(const string& folderPath) {
    auto ac = make_shared<AhoCorasick>();
    if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
        cerr << "敏感词文件夹不存在: " << folderPath << "，将使用空词库。" << endl;
        return ac;
    }

    fs::directory_iterator it(folderPath);
    int totalWords = 0;
    for (const auto& entry : it) {
        if (!fs::is_regular_file(entry)) continue;
        if (entry.path().extension() != CONFIG.fileExtension) continue;

        string filepath = entry.path().string();
        ifstream file(filepath);
        if (!file.is_open()) {
            cerr << "无法打开文件: " << filepath << "，跳过。" << endl;
            continue;
        }

        string line;
        int count = 0;
        while (getline(file, line)) {
            // 去除首尾空白
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            string trimmed = line.substr(start, end - start + 1);
            if (!trimmed.empty()) {
                ac->insertWord(trimmed);
                count++;
            }
        }
        file.close();
        cout << filepath << " -> " << count << " 个词条" << endl;
        totalWords += count;
    }
    ac->build();  // 构建失败指针（插入完成后调用）
    cout << "[加载] 从 " << folderPath << " 共加载 " << totalWords << " 个敏感词。" << endl;
    return ac;
}

/*
 * 加载组合规则（同时将关键词插入 AC 自动机）
 * 参数：
 *   folderPath - 组合规则文件夹路径
 *   ac         - 指向 AhoCorasick 的共享指针（用于插入组合关键词）
 * 返回值：组合规则列表（包含规则 ID）
 * 文件格式：
 *   每行一条规则，支持 # 注释和空行。
 *   格式1（简单）：关键词1+关键词2+... （默认阈值 = 关键词总数，全匹配）
 *   格式2（高级）：规则名称|关键词1+关键词2+...|阈值
 *       规则名称和阈值可选，如果省略阈值则默认全匹配。
 *   关键词之间用 '+' 分隔，前后可有空格（自动清理）。
 */
vector<CombinationRule> loadCombinationRulesFromFolder(
    const string& folderPath,
    shared_ptr<AhoCorasick> ac
) {
    vector<CombinationRule> allRules;
    if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
        cerr << "组合规则文件夹不存在: " << folderPath << "，将不加载规则。" << endl;
        return allRules;
    }

    fs::directory_iterator it(folderPath);
    int totalRules = 0;
    int ruleId = 0;   // 从 0 开始，每个规则分配唯一 ID

    for (const auto& entry : it) {
        if (!fs::is_regular_file(entry)) continue;
        if (entry.path().extension() != CONFIG.fileExtension) continue;

        string filepath = entry.path().string();
        ifstream file(filepath);
        if (!file.is_open()) {
            cerr << "无法打开文件: " << filepath << "，跳过。" << endl;
            continue;
        }

        string line;
        int fileRuleCount = 0;
        while (getline(file, line)) {
            // 去除首尾空白
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == string::npos) continue;
            size_t end = line.find_last_not_of(" \t\r\n");
            string trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == '#') continue;

            string ruleName;
            vector<string> keywords;
            int minMatch = 0;

            // ---------- 解析规则 ----------
            size_t pipePos1 = trimmed.find('|');
            if (pipePos1 != string::npos) {
                // 格式2/3：包含 '|' 分隔符
                ruleName = trimmed.substr(0, pipePos1);
                // 清理规则名首尾空格
                size_t rs = ruleName.find_first_not_of(" \t");
                if (rs != string::npos) {
                    size_t re = ruleName.find_last_not_of(" \t");
                    ruleName = ruleName.substr(rs, re - rs + 1);
                }
                else {
                    ruleName = "Rule_" + to_string(totalRules + 1);
                }

                string rest = trimmed.substr(pipePos1 + 1);
                size_t pipePos2 = rest.find('|');
                string keywordPart;
                if (pipePos2 != string::npos) {
                    // 有阈值部分
                    keywordPart = rest.substr(0, pipePos2);
                    string minStr = rest.substr(pipePos2 + 1);
                    try { minMatch = stoi(minStr); }
                    catch (...) { minMatch = 0; }
                }
                else {
                    // 无阈值部分
                    keywordPart = rest;
                }

                // 按 '+' 分割关键词
                size_t plus = 0;
                string temp = keywordPart;
                while ((plus = temp.find('+')) != string::npos) {
                    string kw = temp.substr(0, plus);
                    // 清理关键词首尾空格
                    size_t ks = kw.find_first_not_of(" \t");
                    if (ks != string::npos) {
                        size_t ke = kw.find_last_not_of(" \t");
                        keywords.push_back(kw.substr(ks, ke - ks + 1));
                    }
                    temp.erase(0, plus + 1);
                }
                if (!temp.empty()) {
                    size_t ks = temp.find_first_not_of(" \t");
                    if (ks != string::npos) {
                        size_t ke = temp.find_last_not_of(" \t");
                        keywords.push_back(temp.substr(ks, ke - ks + 1));
                    }
                }
            }
            else {
                // 格式1：简单格式 "关键词1+关键词2+..."
                ruleName = "CombinedRule_" + to_string(totalRules + 1);
                string temp = trimmed;
                size_t plus = 0;
                while ((plus = temp.find('+')) != string::npos) {
                    string kw = temp.substr(0, plus);
                    size_t ks = kw.find_first_not_of(" \t");
                    if (ks != string::npos) {
                        size_t ke = kw.find_last_not_of(" \t");
                        keywords.push_back(kw.substr(ks, ke - ks + 1));
                    }
                    temp.erase(0, plus + 1);
                }
                if (!temp.empty()) {
                    size_t ks = temp.find_first_not_of(" \t");
                    if (ks != string::npos) {
                        size_t ke = temp.find_last_not_of(" \t");
                        keywords.push_back(temp.substr(ks, ke - ks + 1));
                    }
                }
            }

            // 校验：如果关键词列表为空，跳过
            if (keywords.empty()) {
                cerr << "规则解析失败（无关键词），跳过: " << trimmed << endl;
                continue;
            }

            // 若未设置阈值或阈值无效，默认为全匹配（关键词总数）
            if (minMatch == 0 || minMatch > static_cast<int>(keywords.size())) {
                minMatch = static_cast<int>(keywords.size());
            }

            // 【关键】将每个关键词插入 AC 自动机，关联当前规则 ID
            for (const string& kw : keywords) {
                ac->insertComboKeyword(kw, ruleId);
            }

            // 保存规则
            allRules.push_back({ keywords, minMatch, ruleName, ruleId });

            // 打印加载信息
            cout << "  规则: " << ruleName << " | 关键词: [";
            for (size_t i = 0; i < keywords.size(); ++i) {
                cout << keywords[i];
                if (i + 1 < keywords.size()) cout << ", ";
            }
            cout << "] | 阈值: " << minMatch << " | ID: " << ruleId << endl;

            totalRules++;
            fileRuleCount++;
            ruleId++;   // 下一个规则分配新 ID
        }
        file.close();
        cout << filepath << " -> " << fileRuleCount << " 条规则" << endl;
    }
    cout << "[加载] 从 " << folderPath << " 共加载 " << totalRules << " 条组合规则。" << endl;
    return allRules;
}
/*
 ============================================================================
 全局管理实例与主函数（通用）
 说明：
   - 本模块提供 g_validator 全局实例、互斥锁、热更新线程和主函数。
   - 所有逻辑与 AC 存储方式无关，仅调用 TextSecurityValidator 的公开接口。
   - 主函数进行测试。
 ============================================================================
 */

 // =========================================================================
 // 全局管理（热更新 + 校验器实例）
 // =========================================================================

shared_ptr<TextSecurityValidator> g_validator;
mutex g_validatorMutex;

/*
 * 热更新核心函数：重新加载词库和组合规则，并切换校验器
 * 流程：
 *   1. 从文件夹加载新 AC 自动机（敏感词）
 *   2. 从文件夹加载新组合规则（同时将关键词插入 AC）
 *   3. 重建 AC 自动机（构建失败指针）
 *   4. 创建新的 TextSecurityValidator 实例
 *   5. 原子替换 g_validator（写锁保护）
 */
void reloadValidator() {
    lock_guard<mutex> lock(g_validatorMutex);
    cout << "开始热更新..." << endl;
    auto newAc = loadACFromFolder(CONFIG.sensitiveWordsFolder);
    auto newRules = loadCombinationRulesFromFolder(CONFIG.combinationRulesFolder, newAc);
    newAc->build();  // 重新构建（因为组合关键词已插入）
    auto newValidator = make_shared<TextSecurityValidator>(newAc, newRules);
    g_validator = newValidator;
    cout << "热更新完成，新词库词条数: " << newAc->wordCount() << "，新规则数: " << newRules.size() << endl;
}

/*
 * 热更新后台线程
 * 每 CONFIG.hotReloadIntervalSeconds 秒执行一次：
 *   1. 检查配置文件 config.txt 是否变化，若是则重新加载配置
 *   2. 执行 reloadValidator() 重建校验器
 */
void hotReloadThread() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(CONFIG.hotReloadIntervalSeconds));
        if (CONFIG.enableHotReload) {
            // 检测配置文件变化
            if (ConfigLoader::checkAndReload("config.txt")) {
                cout << "配置热更新成功。" << endl;
            }
            // 重建校验器（词库 + 规则）
            reloadValidator();
        }
    }
}

/*
 * 获取当前校验器实例（线程安全）
 * 返回值：指向 TextSecurityValidator 的共享指针
 */
shared_ptr<TextSecurityValidator> getValidator() {
    lock_guard<mutex> lock(g_validatorMutex);
    return g_validator;
}

/*
 * 初始化校验器（在 main 中调用一次）
 * 加载初始词库和规则，并构建 AC 自动机
 */
void initValidator() {
    lock_guard<mutex> lock(g_validatorMutex);
    auto ac = loadACFromFolder(CONFIG.sensitiveWordsFolder);
    auto rules = loadCombinationRulesFromFolder(CONFIG.combinationRulesFolder, ac);
    ac->build();  // 构建失败指针
    g_validator = make_shared<TextSecurityValidator>(ac, rules);
}

// =========================================================================
// 转义替换函数，用于处理 testData.txt 中的特殊字符
// =========================================================================
string unescapeString(const string& s) {
    string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == 'n') {
                result += '\n';
                i++;
            }
            else if (next == 't') {
                result += '\t';
                i++;
            }
            else if (next == 'r') {
                result += '\r';
                i++;
            }
            else if (next == '\\') {
                result += '\\';
                i++;
            }
            else if (next == 'x' && i + 3 < s.size()) {
                // 处理 \xNN 十六进制
                string hex = s.substr(i + 2, 2);
                if (isxdigit(hex[0]) && isxdigit(hex[1])) {
                    char val = static_cast<char>(stoi(hex, nullptr, 16));
                    result += val;
                    i += 3;
                }
                else {
                    result += s[i]; // 原样保留
                }
            }
            else {
                result += s[i];
            }
        }
        else {
            result += s[i];
        }
    }
    return result;
}

// =========================================================================
// 主函数
// =========================================================================

int main() {
    // 1. 加载外部配置文件
    if (ConfigLoader::loadConfig("config.txt")) {
        cout << "主配置加载成功。" << endl;
    }
    else {
        cout << "主配置加载失败或不存在，使用默认值。" << endl;
    }

    cout << "============================================================" << endl;
    cout << "  任务一：IM 安全过滤系统" << endl;
#ifdef USE_FIXED_ARRAY
    cout << "  版本 fast - 固定数组 AC 子节点 (高速版)" << endl;
#else
    cout << "  版本 balanced - unordered_map AC 子节点 (平衡版)" << endl;
#endif
    cout << "============================================================" << endl << endl;

    // 2. 显示当前配置
    cout << "当前配置：" << endl;
    cout << "  长度限制: " << (CONFIG.enableLengthLimit ? "启用" : "禁用")
        << " (最大 " << CONFIG.maxInputChars << " 字符)" << endl;
    cout << "  格式检测: " << (CONFIG.enableFormatCheck ? "启用" : "禁用") << endl;
    cout << "  AC过滤:   " << (CONFIG.enableAC ? "启用" : "禁用") << endl;
    cout << "  组合规则: " << (CONFIG.enableCombinationRule ? "启用" : "禁用") << endl;
    cout << "  XSS检测:  " << (CONFIG.enableXSS ? "启用" : "禁用") << endl;
    cout << "  SQL检测:  " << (CONFIG.enableSQL ? "启用" : "禁用") << endl;
    cout << "  SQL模式:  " << (CONFIG.enableLooseSQLCheck ? "宽松 (不拦截普通SELECT)" : "严格 (拦截SELECT...FROM)") << endl;
    cout << "  词库文件夹: " << CONFIG.sensitiveWordsFolder << endl;
    cout << "  规则文件夹: " << CONFIG.combinationRulesFolder << endl;
    cout << "  正则文件:   " << CONFIG.regexRulesFile << endl;
    cout << "  日志文件:   " << CONFIG.logFilePath << endl;
    cout << "  热更新:     " << (CONFIG.enableHotReload ? "启用 (间隔 " + to_string(CONFIG.hotReloadIntervalSeconds) + "s)" : "禁用") << endl;
    cout << endl;

    // 3. 初始化校验器
    initValidator();

    // 4. 启动热更新线程
    thread hotThread;
    if (CONFIG.enableHotReload) {
        hotThread = thread(hotReloadThread);
        hotThread.detach();
        cout << "热更新线程已启动。" << endl;
    }

    // ============================================================
    // 从 testData.txt 加载测试用例
    // ============================================================
    struct TestCase {
        string input;
        bool expectedSafe;
        string desc;
    };

    vector<TestCase> tests;
    ifstream testFile("testData.txt");
    if (!testFile.is_open()) {
        cerr << "无法打开 testData.txt，使用默认硬编码测试用例。" << endl;
        // 后备硬编码（完整保留原有的 tests 数组，此处省略，实际可保留）
        // 但为了演示，我们直接退出，提示用户创建文件。
        // 你可以在这里粘贴原有硬编码的 tests 数组作为后备。
        cerr << "请创建 testData.txt 文件并放入测试用例。" << endl;
        return 1;
    }
    else {
        string line;
        while (getline(testFile, line)) {
            // 查找分隔符 "||"
            size_t pos1 = line.find("||");
            if (pos1 == string::npos) {
                cerr << "测试行格式错误，跳过: " << line << endl;
                continue;
            }
            size_t pos2 = line.find("||", pos1 + 2);
            if (pos2 == string::npos) {
                cerr << "测试行格式错误，跳过: " << line << endl;
                continue;
            }
            string input = line.substr(0, pos1);
            string expectedStr = line.substr(pos1 + 2, pos2 - pos1 - 2);
            string desc = line.substr(pos2 + 2);
            bool expected = (expectedStr == "true");
            // 处理转义字符（如 \x01, \n, \t 等）
            input = unescapeString(input);
            tests.push_back({ input, expected, desc });
        }
        testFile.close();
        cout << "从 testData.txt 加载了 " << tests.size() << " 个测试用例。" << endl;
    }

    // 如果文件为空或加载失败，提供后备（可选）
    if (tests.empty()) {
        cerr << "没有加载到任何测试用例，请检查 testData.txt。" << endl;
        return 1;
    }

    // ============================================================
    // 执行测试
    // ============================================================
    cout << "========== 执行测试 ==========" << endl;
    int pass = 0, total = static_cast<int>(tests.size());
    for (const auto& t : tests) {
        string err;
        auto validator = getValidator();
        bool ok = validator->validate(t.input, err);
        bool isPass = (ok == t.expectedSafe);
        if (isPass) pass++;
        cout << "[ " << (isPass ? "PASS" : "FAIL") << " ] " << t.desc << endl;
        cout << "    输入: \"" << t.input.substr(0, 50) << (t.input.size() > 50 ? "..." : "") << "\"" << endl;
        cout << "    结果: " << (ok ? "安全" : "拦截") << (err.empty() ? "" : " | " + err) << endl << endl;
    }

    cout << "通过率: " << pass << "/" << total << endl;
    if (pass == total) cout << "全部通过！" << endl;

    auto validator = getValidator();
    cout << "当前词库词条数: " << validator->getWordCount() << endl;
    cout << "当前组合规则数: " << validator->getRuleCount() << endl;

    cout << "\n热更新已启用，每 " << CONFIG.hotReloadIntervalSeconds << " 秒自动扫描词库文件夹变化。" << endl;
    cout << "日志文件: " << CONFIG.logFilePath << endl;
    cout << "正则配置文件: " << CONFIG.regexRulesFile << endl;

    cout << "\n按 Enter 键退出..." << endl;
    cin.get();

    // 关闭异步日志器，等待所有日志写入完毕
    AsyncLogger::instance().shutdown();

    return 0;
}

