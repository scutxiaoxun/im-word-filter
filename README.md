 ### IM 敏感词过滤与安全校验引擎

一个文本安全校验系统，专为社交媒体（IM、评论）场景设计。本分支将核心过滤引擎封装为**动态链接库**（Windows `.dll` / Linux `.so`），对外提供 C 语言 API。可被任意支持 C 接口的语言（C++、Python、Java、C# 等）嵌入调用，适用于需要**实时文本安全校验**的各类后端服务、网关、游戏服务器等场景。**

## 核心特性

基于 AC 自动机（Aho-Corasick）算法，支持万级词库的 `O(N)` 线性扫描。
支持“A+B+C”逻辑检测（单词单独不违规，组合在一起拦截），与 AC 自动机深度融合，不增加额外遍历开销
集成 XSS 正则检测、SQL 注入检测（宽松/严格双模式）、纯文本格式与长度限制。
所有开关与路径均通过 `config.txt` 外部化，支持不停机加载新词库和新配置。
异步非阻塞日志，采用生产者-消费者模型，主流程 IO 零阻塞，支持高并发。
编译时可选 `unordered_map`（平衡版，省内存）或 `int next[256]`（高速，更大内存占用）。

## API 接口说明

对外暴露 4 个核心函数（定义在 `filter_api.h`）：

| 函数 | 说明 |
| :--- | :--- |
| `Filter_Init(config_dir)` | 初始化引擎，`config_dir` 为配置文件所在目录（如 `"."` 或 `"./config"`）。返回 `FILTER_OK` 表示成功。 |
| `Filter_Check(text, err_msg, size)` | 检测文本。返回 `FILTER_OK`（安全）或 `FILTER_BLOCKED`（拦截）。若被拦截，原因写入 `err_msg` 缓冲区。 |
| `Filter_Reload()` | 手动触发热重载（一般无需调用，后台自动完成）。 |
| `Filter_Destroy()` | 销毁引擎，释放所有资源（程序退出前必须调用）。 |

**错误码定义**：
```c
#define FILTER_OK               0   // 安全
#define FILTER_BLOCKED          1   // 被拦截
#define FILTER_ERROR_NOT_INIT  -1   // 引擎未初始化
#define FILTER_ERROR_INVALID   -2   // 参数无效
#define FILTER_ERROR_INTERNAL  -3   // 内部异常
```

## 快速开始

1. 环境要求
- 支持 C++17 的编译器（如 GCC 8+、Clang 6+）
- 操作系统：windows或linux

2. 编译

Windows:
g++ -std=c++17 -O2 -Wall -pthread -DBUILD_DLL -DFILTER_EXPORTS -shared task1_security.cpp filter_sdk.cpp -o filter_sdk.dll -Wl,--out-implib,libfilter_sdk.a

Linux:
g++ -std=c++17 -O2 -Wall -pthread -DBUILD_DLL -fPIC -shared task1_security.cpp filter_sdk.cpp -o libfilter_sdk.so
高速版编译：如需启用 int next[256] 固定数组存储，在编译命令中添加 -DUSE_FIXED_ARRAY（在-shared前添加）

## 词库与规则

敏感词：`sensitive_words/` 下任意 `.txt`，每行一个词。
组合规则：`combination_rules/` 下任意 `.txt`，支持两种格式：
简单全匹配：`甲+乙+丙`（三个词必须全部出现才触发）
带阈值：`颜色组合|红+黄+蓝|2`（命中任意 2 个即触发）

词库来源：本项目中的敏感词库基于 [konsheng/Sensitive-lexicon](https://github.com/konsheng/Sensitive-lexicon) 开源词库整理，感谢原作者的贡献。如需更新或扩展词库，请参考该项目的使用条款。

## 许可证
MIT License
