 IM 敏感词过滤与安全校验引擎

一个文本安全校验系统，专为社交媒体（IM、评论）场景设计。本分支将核心过滤引擎封装为**动态链接库**（Windows `.dll` / Linux `.so`），对外提供 C 语言 API。可被任意支持 C 接口的语言（C++、Python、Java、C# 等）嵌入调用，适用于需要**实时文本安全校验**的各类后端服务、网关、游戏服务器等场景。**

核心特性

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

快速开始

1. 环境要求
- 支持 C++17 的编译器（如 GCC 8+、Clang 6+）
- 操作系统：windows或linux

2. 编译
Windows:
g++ -std=c++17 -O2 -Wall -pthread -DBUILD_DLL -DFILTER_EXPORTS -shared task1_security.cpp filter_sdk.cpp -o filter_sdk.dll -Wl,--out-implib,libfilter_sdk.a

Linux:
g++ -std=c++17 -O2 -Wall -pthread -DBUILD_DLL -fPIC -shared task1_security.cpp filter_sdk.cpp -o libfilter_sdk.so
高速版编译：如需启用 int next[256] 固定数组存储，在编译命令中添加 -DUSE_FIXED_ARRAY（在-shared前添加）


词库与规则

敏感词：`sensitive_words/` 下任意 `.txt`，每行一个词。
组合规则：`combination_rules/` 下任意 `.txt`，支持两种格式：
简单全匹配：`甲+乙+丙`（三个词必须全部出现才触发）
带阈值：`颜色组合|红+黄+蓝|2`（命中任意 2 个即触发）

词库来源：本项目中的敏感词库基于 [konsheng/Sensitive-lexicon](https://github.com/konsheng/Sensitive-lexicon) 开源词库整理，感谢原作者的贡献。如需更新或扩展词库，请参考该项目的使用条款。

运行
当前测试用例不是真实敏感词，sensitive_words和combination_rules是专为当前测试用例设计的敏感词库，sensitive_words_work和combination_rules_work是真实可用的中文词库，改名为sensitive_words和combination_rules后即可使用。

实时交互与热更新体验
程序启动后，自动加载 sensitive_words/ 和 combination_rules/ 文件夹中的词库与规则，并启动后台热更新线程（默认每 30 秒扫描一次变化）。

热更新期间无需停机：您可以一边交互输入，一边修改 config.txt、增删 sensitive_words/ 中的 .txt 文件，或调整 combination_rules/ 中的规则。

下次热更新周期（默认hotReloadIntervalSeconds=30 秒）：系统将静默加载新配置和新词库，后续输入立即生效，而当前正在处理的请求不受影响。

您可以在日志文件 security.log 中查看拦截记录和热更新日志。



目录结构与文件说明
text
├── task1_security.cpp # 主程序源代码（含所有功能）
├── config.txt               # 运行配置文件（开关、阈值、路径）
├── regex_rules.txt      # 外部正则表达式（XSS 和 SQL 规则）
├── testData.txt           # 测试用例
├── README.md         # 本文档
├── sensitive_words/  # 敏感词库文件夹
│   └── *.txt                # 每行一个词，任意文件名
└── combination_rules/    # 组合规则文件夹
    └── *.txt                  # 格式：甲+乙+丙 或 规则名|甲+乙+丙|阈值
性能与复杂度
指标	                              平衡版 (unordered_map)	高速版 (int next[256])
词库插入	                                           O(1) 均摊	                O(1) 直接赋值
构建失败指针	                           O(M)	                O(L × 256)
单次扫描	                                           O(N)，哈希均摊	O(N)，直接内存寻址
空间占用	                                           O(M)（紧凑）               O(L × 256)（约 200 MB / 20万节点）
适用场景	                                            内存敏感环境	内存充裕、追求极致 QPS
其中 N = 输入文本长度（字节），M = 词库总字符数，L = AC 节点总数。

许可证
MIT License
