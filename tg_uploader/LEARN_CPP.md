# 从零读懂 tguploader C++ 源码

> 面向有一点 C++11 基础的读者。本讲义完全基于本项目代码展开，每个概念都能在源码中找到对应位置。建议一边读讲义，一边对照源文件阅读。

---

## 目录

1. [项目文件结构](#1-项目文件结构)
2. [头文件与 cpp 文件的分工](#2-头文件与-cpp-文件的分工)
3. [#pragma once 和 include 守卫](#3-pragma-once-和-include-守卫)
4. [struct：数据的容器](#4-struct数据的容器)
5. [class：数据 + 行为的封装](#5-class数据--行为的封装)
6. [static constexpr：编译期常量](#6-static-constexpr编译期常量)
7. [std::string 常用操作](#7-stdstring-常用操作)
8. [std::vector：动态数组](#8-stdvector动态数组)
9. [std::filesystem：文件系统操作](#9-stdfilesystem文件系统操作)
10. [Lambda 函数](#10-lambda-函数)
11. [多线程：std::thread](#11-多线程stdthread)
12. [线程安全：std::mutex 与 lock_guard](#12-线程安全stdmutex-与-lock_guard)
13. [原子操作：std::atomic](#13-原子操作stdatomic)
14. [RAII 惯用法](#14-raii-惯用法)
15. [try / catch 异常处理](#15-try--catch-异常处理)
16. [类型转换：static_cast / reinterpret_cast](#16-类型转换static_cast--reinterpret_cast)
17. [std::chrono：计时](#17-stdchrono计时)
18. [信号处理：std::signal](#18-信号处理stdsignal)
19. [C++17 结构化绑定](#19-c17-结构化绑定)
20. [从 main() 看整体流程](#20-从-main-看整体流程)

---

## 1. 项目文件结构

```
tg_uploader/src/
├── main.cpp          ← 程序入口，命令解析
├── config.h/.cpp     ← 读取配置文件（账号、设置）
├── utils.h/.cpp      ← 通用工具函数（格式化、文件判断等）
├── video_info.h/.cpp ← 用 ffprobe 获取视频时长/分辨率
├── caption.h/.cpp    ← 生成 Telegram 消息文字
├── bot_client.h/.cpp ← 用 libcurl 调用 Bot API 上传
├── bot_uploader.h/.cpp ← 多 bot 并行上传调度
├── uploader.h/.cpp   ← TDLib 账号上传调度
├── td_client.h/.cpp  ← 封装 TDLib（Telegram 官方 C++ 库）
├── scanner.h/.cpp    ← 扫描频道历史，写入 SQLite 数据库
├── folder_lock.h/.cpp← 文件夹锁，防止多进程同时处理同一目录
└── CMakeLists.txt    ← 构建配置（告诉编译器怎么编译）
```

**读代码的顺序建议**：`utils.cpp` → `config.cpp` → `bot_client.cpp` → `bot_uploader.cpp` → `main.cpp`

---

## 2. 头文件与 cpp 文件的分工

C++ 把"声明"和"实现"分开放：

| 文件类型 | 放什么 | 作用 |
|----------|--------|------|
| `.h` 头文件 | 函数签名、类定义、struct 定义 | 告诉其他文件"这里有什么" |
| `.cpp` 源文件 | 函数的具体实现 | 实际干活的代码 |

**例子 —— `utils.h`（声明）：**
```cpp
// 只写函数名和参数类型，不写实现
std::string format_file_size(int64_t bytes);
std::string fmt_elapsed(double secs);
bool is_video_file(const std::string& path);
```

**例子 —— `utils.cpp`（实现）：**
```cpp
// 真正的代码在这里
std::string format_file_size(int64_t bytes) {
    char buf[64];
    if (bytes >= 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    }
    // ...
    return buf;
}
```

**为什么要这样分？**
`bot_uploader.cpp` 需要调用 `format_file_size()`，只需要 `#include "utils.h"` 就知道这个函数存在，不需要知道它怎么实现的。编译时再把两个文件链接在一起。

---

## 3. #pragma once 和 include 守卫

每个头文件第一行都是：
```cpp
#pragma once
```

**作用**：防止同一个头文件被 include 两次（否则同一个 struct 定义出现两次，编译报错）。

这是现代写法。老写法是这样的（等价）：
```cpp
#ifndef UTILS_H
#define UTILS_H
// ... 头文件内容 ...
#endif
```

---

## 4. struct：数据的容器

`struct` 是把相关数据打包在一起的方式，类似 Python 的 dataclass。

**项目中的例子 —— `config.h`：**
```cpp
struct AccountConfig {
    std::string name;        // 账号名，如 "ajx"
    std::string phone;       // 手机号，如 "+1302..."
    int api_id = 0;          // Telegram API ID
    std::string api_hash;    // Telegram API Hash
    std::string channel_id;  // 频道名
    std::string session_dir; // TDLib 数据库目录
};
```

**使用方式：**
```cpp
AccountConfig config = load_account("ajx");
std::cout << config.name;    // 访问字段用 .
std::cout << config.phone;
```

**带默认值：** `int api_id = 0;` 表示如果不赋值，默认是 0。

**另一个例子 —— `bot_uploader.h`：**
```cpp
struct BotEntry {
    std::string name;   // bot 名称，如 "PhoUpload_bot"
    std::string token;  // bot token
};

struct BotConfig {
    std::string api_url;            // "http://127.0.0.1:8081"
    std::string channel_id;         // "@mychannel"
    std::vector<BotEntry> bots;     // 所有 bot 的列表
};
```

---

## 5. class：数据 + 行为的封装

`class` 和 `struct` 很像，区别是 class 默认成员是 private（外部不能直接访问），struct 默认是 public。

**项目中的例子 —— `bot_uploader.h`：**
```cpp
class BotUploader {
public:
    // public: 外部可以调用
    BotUploader(const BotConfig& config, const SharedSettings& settings);
    void upload_directory(const std::string& dir_path, bool recursive);

private:
    // private: 只有类内部方法能访问
    BotConfig config_;                      // 存配置
    SharedSettings settings_;
    std::mutex log_mutex_;                  // 线程锁
    std::atomic<int> flood_wait_hits_{0};  // 原子计数器

    bool upload_with_bot(int bot_idx, const std::string& video_path);
    std::vector<std::string> scan_videos(const std::string& dir, bool recursive);
};
```

**构造函数**（Constructor）—— 创建对象时自动调用：
```cpp
// 声明（.h 文件）
BotUploader(const BotConfig& config, const SharedSettings& settings);

// 实现（.cpp 文件）
BotUploader::BotUploader(const BotConfig& config, const SharedSettings& settings)
    : config_(config), settings_(settings),           // 初始化列表
      bot_flood_until_(config.bots.size(), 0) {}      // 用 : 初始化成员变量
```

`: config_(config)` 这个语法叫**成员初始化列表**，比在函数体里赋值更高效。

**使用方式：**
```cpp
BotUploader uploader(bot_config, settings);   // 创建对象
uploader.upload_directory(upload_dir, true);  // 调用方法
```

**命名惯例**：本项目中私有成员变量名尾部加下划线，如 `config_`、`settings_`，这是常见的 C++ 约定，一眼就能区分成员变量和局部变量。

---

## 6. static constexpr：编译期常量

```cpp
// bot_uploader.cpp
static constexpr int64_t BOT_FILE_SIZE_LIMIT = 4000LL * 512 * 1024;

// main.cpp
static constexpr int64_t BIG_THRESHOLD    = 4000LL * 512 * 1024; // ~1.953 GiB
static constexpr int64_t MIN_REMAINDER_SIZE = 10LL * 1024 * 1024; // 10 MB
```

- **`constexpr`**：这个值在编译时就计算好，运行时直接用结果，速度最快
- **`static`**（在函数外用）：这个变量只在当前文件可见，不会与其他文件的同名变量冲突
- **`int64_t`**：64 位整数，避免 32 位 int 在大数字（如 2GB）时溢出
- **`4000LL`**：`LL` 后缀表示 long long 字面量，防止乘法中途溢出

对比 Python：
```python
BOT_FILE_SIZE_LIMIT = 4000 * 512 * 1024  # Python 没有这些问题，整数自动大数
```

---

## 7. std::string 常用操作

项目中大量用到字符串处理：

```cpp
// utils.cpp — 判断是不是视频文件
bool is_video_file(const std::string& path) {
    auto dot = path.rfind('.');       // rfind: 从右边找，返回位置
    if (dot == std::string::npos)     // npos: 表示"没找到"
        return false;
    std::string ext = path.substr(dot); // substr(pos): 从 pos 开始截取到末尾
    // transform: 把 ext 里每个字符转小写
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".mp4" || ext == ".mkv" /* ... */;
}
```

```cpp
// bot_uploader.cpp — 字符串拼接
std::string pfx = std::string(color) + "[" + bot.name + "]\033[0m ";
//                  ^^^^^^^^^^^^^^^ const char* 转 string，然后用 + 拼接
```

```cpp
// main.cpp — 字符串转整数
std::string conc_str = get_arg(argc, argv, "--concurrent");
int concurrent = std::stoi(conc_str);  // string to int，失败会抛异常
```

**`const std::string&` 为什么要加 `const` 和 `&`？**
- `&` 是引用，避免复制整个字符串（字符串可能很长）
- `const` 表示函数不会修改它

---

## 8. std::vector：动态数组

相当于 Python 的 list，但只能放同一种类型：

```cpp
// bot_uploader.cpp — 扫描出来的视频路径列表
std::vector<std::string> videos;        // 声明
videos.push_back(path);                 // 添加元素
int total_files = videos.size();        // 长度

// main.cpp — bot 列表
std::vector<BotEntry> bots;
for (const auto& bot : bots) {          // 遍历
    std::cout << bot.name << "\n";
}
```

```cpp
// 初始化时指定大小和默认值
std::vector<std::time_t> bot_flood_until_(config.bots.size(), 0);
//                                        ^^^^ 元素数量          ^^^^ 默认值（全0）

// 初始化时直接赋值
std::vector<int> bot_uploads(n_bots, 0);
```

---

## 9. std::filesystem：文件系统操作

C++17 标准库，本项目大量使用（`#include <filesystem>`）：

```cpp
namespace fs = std::filesystem;  // 别名，省得每次写 std::filesystem::
```

**路径操作：**
```cpp
fs::path p("/mnt/storage/video.mp4");
p.filename()           // "video.mp4"
p.stem()               // "video"（无扩展名）
p.extension()          // ".mp4"
p.parent_path()        // "/mnt/storage"

// 路径拼接用 /
fs::path thumb = p.parent_path() / (p.stem().string() + ".jpg");
// 结果: "/mnt/storage/video.jpg"
```

**文件/目录判断：**
```cpp
fs::exists(path)           // 是否存在
fs::is_directory(path)     // 是不是目录
fs::is_regular_file(path)  // 是不是普通文件
fs::file_size(path)        // 文件大小（字节）
```

**目录遍历（这是本项目扫描视频的核心）：**
```cpp
// bot_uploader.cpp — scan_videos()
for (auto& entry : fs::recursive_directory_iterator(
         dir, fs::directory_options::skip_permission_denied)) {
    if (entry.is_regular_file()) {
        std::string path = entry.path().string();
        if (is_video_file(path))
            videos.push_back(path);
    }
}
```

**文件操作：**
```cpp
fs::rename(src, dst);   // 重命名/移动
fs::remove(path);       // 删除文件
fs::create_directories(path);  // 递归创建目录（像 mkdir -p）
```

**创建空文件（.big 标记文件）：**
```cpp
std::ofstream(video_path + ".big").close();
// 创建一个 xxx.mp4.big 的空文件，用作"已处理"标记
```

---

## 10. Lambda 函数

Lambda 是一种"匿名函数"，可以直接写在代码里，不需要单独命名。

**基本语法：**
```cpp
[捕获列表](参数) { 函数体 }
```

**项目中的例子 1 —— 简单 lambda 替代重复代码：**
```cpp
// bot_client.cpp — send_video()
// 与其写多次重复的代码，定义一个 lambda
auto add_str = [&](const char* name, const std::string& val) {
    curl_mimepart* part = curl_mime_addpart(form);
    curl_mime_name(part, name);
    curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
};

// 然后多次调用
add_str("chat_id", chat_id);
add_str("caption", caption);
add_str("parse_mode", "HTML");
```

`[&]` 表示**捕获所有外部变量的引用**，lambda 内部可以使用 `form`（外部定义的变量）。

**项目中的例子 2 —— 带捕获的线程 lambda：**
```cpp
// bot_uploader.cpp — 多线程上传
for (int i = 0; i < n_bots; i++) {
    threads.emplace_back([&, i]() {    // [&, i]: 引用捕获其他变量，值捕获 i
        while (true) {
            std::string vp;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (work_queue.empty()) break;
                vp = work_queue.front();
                work_queue.pop();
            }
            if (upload_with_bot(i, vp)) {  // i 是每个线程自己的 bot 序号
                par_success++;
            } else {
                par_skipped++;
            }
        }
    });
}
```

**为什么 `i` 要值捕获 `[&, i]` 而不是引用捕获 `[&]`？**
如果用 `[&]`，所有线程共享同一个 `i`，等循环结束 `i` 已经变成最大值，每个线程拿到的都是错误的 bot 序号。值捕获 `i` 会在创建 lambda 时复制一份 `i` 的当前值，每个线程拿到自己的序号。

---

## 11. 多线程：std::thread

本项目用多线程实现"多个 bot 同时上传不同文件"。

```cpp
// bot_uploader.cpp
#include <thread>

std::vector<std::thread> threads;

for (int i = 0; i < n_bots; i++) {
    // emplace_back: 直接在 vector 里构造 thread 对象（比 push_back 更高效）
    threads.emplace_back([&, i]() {
        // 这里的代码在新线程里运行
        while (true) {
            // ... 从队列取文件，上传 ...
        }
    });
}

// 等待所有线程结束（主线程在这里阻塞）
for (auto& t : threads) {
    t.join();
}
```

**线程的问题**：多个线程同时读写同一个变量，会出现数据竞争（race condition）。比如两个线程同时做 `count++`，可能结果不是 +2 而是 +1。解决方法见下一节。

---

## 12. 线程安全：std::mutex 与 lock_guard

**mutex**（互斥锁）保证同一时刻只有一个线程能执行某段代码。

```cpp
// bot_uploader.h
std::mutex log_mutex_;  // 成员变量，控制日志输出

// bot_uploader.cpp — 多线程同时打印日志时用锁保护
{
    std::lock_guard<std::mutex> lk(log_mutex_);  // 加锁
    std::cout << "[INFO] " << pfx << "Success: " << fname << "\n";
}  // 离开作用域，lock_guard 析构，自动解锁
```

**为什么用 `lock_guard` 而不是手动 `lock()` / `unlock()`？**

手动方式的问题：
```cpp
log_mutex_.lock();
std::cout << "...";    // 如果这里抛了异常
log_mutex_.unlock();   // 这行永远不会执行！→ 死锁
```

`lock_guard` 利用 RAII（见第 14 节）：构造时加锁，析构时自动解锁，无论是正常退出还是异常都能解锁。

**工作队列的保护：**
```cpp
std::queue<std::string> work_queue;
std::mutex queue_mutex;

// 每个 bot 线程都这样取任务
{
    std::lock_guard<std::mutex> lk(queue_mutex);  // 加锁
    if (work_queue.empty()) break;
    vp = work_queue.front();
    work_queue.pop();
}  // 自动解锁，其他线程才能进来取
```

---

## 13. 原子操作：std::atomic

某些简单操作（如计数器加减）用 mutex 太重，用 `std::atomic` 更轻量：

```cpp
// bot_uploader.h
std::atomic<int> flood_wait_hits_{0};      // 初始值 0
std::atomic<int> flood_wait_total_secs_{0};

// 类外面（并行块里）
std::atomic<int> par_success{0}, par_skipped{0};
```

```cpp
// 多个线程安全地修改，无需加锁
par_success++;           // 原子加法
par_skipped++;
flood_wait_hits_++;
flood_wait_total_secs_ += resp.retry_after;

// 读取当前值
std::cout << par_success.load() << "/" << total_files;
```

**`atomic` vs `mutex` 的选择**：
- 简单的 +1、-1、读取 → 用 `atomic`
- 复杂操作（读-改-写多个变量、输出多行日志）→ 用 `mutex`

---

## 14. RAII 惯用法

RAII = **Resource Acquisition Is Initialization**（资源获取即初始化）。

核心思想：**把资源（锁、文件、内存）绑定到对象的生命周期**。对象创建时获取资源，对象销毁（离开作用域）时自动释放。

本项目里随处可见 RAII：

**锁的 RAII：**
```cpp
{
    std::lock_guard<std::mutex> lk(log_mutex_);  // 构造: 加锁
    std::cout << "...";
}  // 析构: 自动解锁，即使中途异常也能解锁
```

**文件句柄的 RAII（utils.cpp）：**
```cpp
FILE* f = std::fopen(path.c_str(), "rb");
if (!f) return {};

// ... 处理文件 ...

std::fclose(f);  // 手动关闭（这里没用 RAII，是 C 风格代码）
```

对比 C++ RAII 风格：
```cpp
std::ifstream f(path);  // 构造时打开
// ... 处理 ...
// 离开作用域自动关闭，不需要手动 fclose
```

**理解为什么大量使用 `{}` 大括号块**：
```cpp
// 这对大括号创建了一个新的作用域
{
    std::lock_guard<std::mutex> lk(log_mutex_);
    std::cout << "打印日志\n";
}
// lk 在这里析构 → 解锁
// 后面代码不再持有锁，其他线程可以进入
std::this_thread::sleep_for(std::chrono::seconds(2));  // 耗时操作不占锁
```

---

## 15. try / catch 异常处理

```cpp
// main.cpp — 加载 bot 配置
BotConfig bot_config;
try {
    bot_config = load_bot_config();
} catch (const std::exception& e) {
    std::cerr << "Error loading bot config: " << e.what() << "\n";
    return;
}
```

**忽略异常（不关心失败）：**
```cpp
// 删除临时文件，失败了也无所谓
try { fs::remove(thumb_320); } catch (...) {}
//                                    ^^^ 捕获所有类型的异常，直接忽略
```

**字符串转数字时的异常：**
```cpp
// main.cpp
try {
    range = std::stoi(range_str);   // 如果 range_str 不是数字，抛 std::invalid_argument
} catch (...) {
    std::cerr << "Error: invalid --range value: " << range_str << "\n";
    return 1;
}
```

**主动抛出异常：**
```cpp
// config.cpp 风格（假设代码）
if (cfg.bots.empty()) {
    throw std::runtime_error("No bots configured in bots.json");
}
```

---

## 16. 类型转换：static_cast / reinterpret_cast

C++ 的类型转换比 C 风格的 `(int)x` 更安全、意图更清晰。

**`static_cast`** —— 正常的数值类型转换：
```cpp
// bot_uploader.cpp
int64_t file_size = st.st_size;   // st_size 是 off_t 类型

// 传给需要 int 的函数参数
static_cast<int>(info.duration)

// 整数转浮点（防止整数除法精度丢失）
double bitrate = static_cast<double>(file_size) / info.duration;

// 计算百分比
int pct = static_cast<int>(ulnow * 100 / ultotal);
```

**`reinterpret_cast`** —— 底层位模式重新解释（危险，只在必要时用）：
```cpp
// bot_client.cpp — libcurl 返回 void*，需要转成 CURL*
CURL* curl = reinterpret_cast<CURL*>(curl_);
//           ^^^^^^^^^^^^^^^^^^^^^^^^ 把 void* "重解释"为 CURL*
```

---

## 17. std::chrono：计时

本项目用 chrono 测量上传耗时：

```cpp
#include <chrono>

// 记录开始时间
auto send_start = std::chrono::steady_clock::now();

// ... 上传文件 ...

// 计算经过时间（秒，浮点）
double send_secs = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - send_start).count();

std::cout << "耗时: " << fmt_elapsed(send_secs) << "\n";
```

**线程休眠：**
```cpp
// 等待 2 秒
std::this_thread::sleep_for(std::chrono::seconds(2));

// 等待 retry_after 秒（变量）
std::this_thread::sleep_for(std::chrono::seconds(resp.retry_after + 1));
```

---

## 18. 信号处理：std::signal

程序运行中按 Ctrl+C 会发送 SIGINT 信号，默认行为是直接终止进程。
本项目注册了自定义处理函数，在退出前清理文件夹锁：

```cpp
// main.cpp
std::string g_locked_folder;  // 全局变量，记录当前锁定的文件夹

static void signal_handler(int sig) {
    if (!g_locked_folder.empty()) {
        remove_folder_lock(g_locked_folder);  // 清理锁文件
        g_locked_folder.clear();
    }
    // 恢复默认处理，再次发送信号，让系统正常退出
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, char* argv[]) {
    // 注册信号处理函数
    std::signal(SIGINT, signal_handler);   // Ctrl+C
    std::signal(SIGTERM, signal_handler);  // kill 命令
    // ...
}
```

---

## 19. C++17 结构化绑定

相当于 Python 的解包（unpacking），让代码更简洁：

```cpp
// bot_uploader.cpp — 解析 JSON，遍历 key-value 对
for (auto& [name, token_val] : data["bots"].items()) {
//         ^^^^^^^^^^^^^^^^^^^  结构化绑定，自动解包 pair
    BotEntry e;
    e.name  = name;
    e.token = token_val.get<std::string>();
    cfg.bots.push_back(std::move(e));
}
```

Python 等价：
```python
for name, token_val in data["bots"].items():
    ...
```

```cpp
// main.cpp — 遍历按文件夹分组的视频
std::map<std::string, std::vector<std::string>> folder_groups;
for (auto& [folder_path, folder_videos] : folder_groups) {
    // folder_path 是 key，folder_videos 是 value
}
```

---

## 20. 从 main() 看整体流程

```cpp
int main(int argc, char* argv[]) {
    // 1. 检查参数
    if (argc < 2) { print_usage(); return 1; }

    // 2. 注册信号处理
    std::signal(SIGINT, signal_handler);

    // 3. 确定配置文件目录
    const char* env_base = std::getenv("CTG_BASE_DIR");
    if (env_base) {
        set_base_dir(std::string(env_base));
    } else {
        // 检查可执行文件所在目录是否有 .account_configs
        std::string exe_path = fs::canonical(fs::path(argv[0])).parent_path().string();
        if (fs::is_directory(exe_path + "/.account_configs")) {
            set_base_dir(exe_path);
        } else {
            set_base_dir(fs::current_path().string());  // 用当前目录
        }
    }

    // 4. 根据子命令分发
    std::string command = argv[1];

    if (command == "bot-upload") {
        cmd_bot_upload(dir, recursive);
        // ↓
        // BotUploader uploader(bot_config, settings);
        // uploader.upload_directory(upload_dir, recursive);
        //   ↓
        //   scan_videos() — 扫描目录，找 .mp4 等文件
        //   启动 N 个线程，每个线程对应一个 bot
        //   每个线程循环: 从队列取文件 → upload_with_bot() → 记录结果
        //     ↓
        //     BotClient::send_video() — 用 libcurl 发 multipart 请求
        //     等待响应 → 成功/失败/FLOOD_WAIT
    }
}
```

**代码从外向内的调用链：**
```
main()
  └── cmd_bot_upload()
        └── BotUploader::upload_directory()
              ├── scan_videos()          # 找文件
              ├── std::thread × N        # N 个 bot 线程
              │     └── upload_with_bot()
              │           └── BotClient::send_video()
              │                 └── libcurl HTTP POST → telegram-bot-api
              └── 汇总结果，打印统计
```

---

## 推荐学习路径

按这个顺序阅读源码，难度逐渐递增：

1. **`utils.h` + `utils.cpp`** —— 只有简单函数，没有类和线程，练习阅读 C++ 代码
2. **`config.h` + `config.cpp`** —— struct、文件读取、JSON 解析
3. **`bot_client.h` + `bot_client.cpp`** —— class、libcurl 使用
4. **`bot_uploader.h` + `bot_uploader.cpp`** —— class、多线程、mutex、atomic
5. **`main.cpp`** —— 命令行解析、整体流程、信号处理

## 进一步学习资源

- [cppreference.com](https://cppreference.com) —— C++ 标准库最权威的文档，查任何函数都用它
- [learncpp.com](https://learncpp.com) —— 系统学习 C++ 的免费教程（英文）
- C++ Primer（中文版）—— 适合系统学习的书籍
