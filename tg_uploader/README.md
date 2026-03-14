# tguploader C++ 源码精读讲义

> 语法级别的 C++ 教学，完全结合本项目代码。每个概念都有"Python 对比"帮助理解。
> 建议边读边对照 `src/` 目录下的源文件。

---

## 目录

**基础语法篇**
1. [变量、类型与字面量](#1-变量类型与字面量)
2. [指针与引用](#2-指针与引用)
3. [函数与参数传递](#3-函数与参数传递)
4. [控制流](#4-控制流)
5. [const 关键字的所有用法](#5-const-关键字的所有用法)
6. [static 关键字的所有用法](#6-static-关键字的所有用法)
7. [命名空间 namespace](#7-命名空间-namespace)

**类型系统篇**
8. [struct：聚合数据](#8-struct聚合数据)
9. [class：封装行为](#9-class封装行为)
10. [构造函数与成员初始化列表](#10-构造函数与成员初始化列表)
11. [析构函数与 RAII](#11-析构函数与-raii)
12. [类型转换](#12-类型转换)

**标准库篇**
13. [std::string 完全指南](#13-stdstring-完全指南)
14. [std::vector 完全指南](#14-stdvector-完全指南)
15. [std::map 与 std::queue](#15-stdmap-与-stdqueue)
16. [std::filesystem 完全指南](#16-stdfilesystem-完全指南)
17. [Lambda 表达式完全指南](#17-lambda-表达式完全指南)
18. [多线程完全指南](#18-多线程完全指南)
19. [std::chrono 计时](#19-stdchrono-计时)

**第三方库篇**
20. [nlohmann/json 库](#20-nlohmannjson-库)
21. [libcurl 库](#21-libcurl-库)

**工程篇**
22. [头文件与 cpp 的分工](#22-头文件与-cpp-的分工)
23. [CMake 构建系统](#23-cmake-构建系统)
24. [整体架构梳理](#24-整体架构梳理)

---

# 基础语法篇

---

## 1. 变量、类型与字面量

### 1.1 基本类型

```cpp
// 整数类型
int     a = 42;         // 32 位有符号整数，范围 -2^31 ~ 2^31-1
int64_t b = 1000000LL;  // 64 位有符号整数（来自 <cstdint>），LL 后缀表示 long long 字面量
int32_t c = 100;        // 32 位有符号整数（明确位宽，跨平台更安全）
size_t  d = 0;          // 无符号整数，专门用于表示大小/下标，64位系统上是64位
bool    e = true;       // 布尔值，true/false

// 浮点类型
double  f = 3.14;       // 64 位浮点（双精度），项目中大量用于计算时长/码率
float   g = 3.14f;      // 32 位浮点，f 后缀

// 字符类型
char    h = 'A';        // 单个字符，ASCII 值
uint8_t i = 0xFF;       // 8 位无符号整数，常用于字节操作（如解析 JPEG）
```

**项目中的真实例子（`utils.cpp`）：**
```cpp
std::string format_file_size(int64_t bytes) {
//                            ^^^^^^^ 必须用 64 位，因为文件可能超过 2GB（2^31 bytes）
    if (bytes >= 1024LL * 1024 * 1024) {
//              ^^^^^^ LL 后缀：如果写成 1024 * 1024 * 1024，
//              三个 int 相乘会在 32 位范围内溢出，结果错误！
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
//                    ^^^^^^^^^^^^^^^^^^^ 转成 double 才能做精确除法，
//                    整数除法 bytes / 1073741824 会丢掉小数部分
    }
}
```

**Python 对比：**
```python
# Python 的整数没有溢出，不需要考虑位宽
bytes_val = 2_098_521_235  # 自动大整数
size_gb = bytes_val / (1024 ** 3)  # 自动浮点除法
```

### 1.2 auto：自动类型推断

C++11 引入，让编译器自己推断变量类型：

```cpp
// main.cpp
auto send_start = std::chrono::steady_clock::now();
// send_start 的类型是 std::chrono::time_point<std::chrono::steady_clock>
// 这个类型名太长了，用 auto 省事

auto accounts = list_accounts();
// accounts 的类型是 std::vector<AccountConfig>，函数返回值决定

// bot_uploader.cpp
for (auto& entry : fs::recursive_directory_iterator(dir)) {
//   ^^^^ entry 是 fs::directory_entry，让编译器推断即可
}
```

**注意**：`auto` 不是"任意类型"，类型在编译时就确定了，之后不能改变。

### 1.3 变量的作用域

```cpp
// bot_uploader.cpp — upload_directory()
int n_bots = static_cast<int>(config_.bots.size());

if (n_bots > 1) {
    // 这里开始新的作用域
    std::atomic<int> par_success{0};   // 只在这个 if 块里存在
    std::atomic<int> par_skipped{0};

    // ... 多线程代码 ...

}  // par_success、par_skipped 在这里销毁

// 这里访问 par_success 会编译错误：变量已经不存在
```

---

## 2. 指针与引用

这是 C++ 最核心也最容易混淆的概念。

### 2.1 内存地址与指针

每个变量都在内存中占据一块空间，有一个地址：

```cpp
int x = 42;
int* p = &x;   // & 取地址运算符，p 存储 x 的内存地址
               // int* 意思是"指向 int 的指针"

std::cout << x;   // 42          — x 的值
std::cout << &x;  // 0x7fff...   — x 的内存地址
std::cout << p;   // 0x7fff...   — 和 &x 一样
std::cout << *p;  // 42          — *p 解引用：通过地址找到值
```

**项目中的指针（`bot_client.cpp`）：**
```cpp
// libcurl 用 void* 传递不透明指针
void* curl_;   // 声明时不知道是什么类型，用 void*

// 使用时转换成真正的类型
CURL* curl = reinterpret_cast<CURL*>(curl_);
//    ^^^^^ CURL 是 libcurl 的结构体类型
//                                   ^^^^^ 把 void* 重新解释为 CURL*
```

**指针操作 libcurl：**
```cpp
curl_mime* form = curl_mime_init(curl);
//         ^^^^ form 是指针，指向 libcurl 分配的内存

curl_mimepart* part = curl_mime_addpart(form);
curl_mime_name(part, "chat_id");

curl_mime_free(form);   // 必须手动释放！否则内存泄漏
```

### 2.2 引用：指针的安全替代

引用是变量的别名，比指针更安全（不能为 null，不能重新绑定）：

```cpp
int x = 42;
int& ref = x;   // ref 是 x 的引用（别名）

ref = 100;      // 修改 ref 就是修改 x
std::cout << x; // 100，x 也变了
```

**函数参数中的引用（最重要的用法）：**

```cpp
// 值传递：复制整个字符串，浪费内存和时间
void bad(std::string path) { ... }

// const 引用传递：不复制，只读
void good(const std::string& path) { ... }
//                           ^ 引用，不复制
//         ^^^^^ const，保证不修改

// 非 const 引用传递：不复制，可修改（用于"输出参数"）
bool prepare_video(const std::string& video_path, PreparedVideo& out) {
//                                                               ^ 没有 const，函数会填充 out
    out.caption = "...";  // 修改调用者的变量
    return true;
}
```

**项目真实例子（`uploader.h`）：**
```cpp
class Uploader {
private:
    TdClient& client_;             // 引用成员：不拥有 client，只是引用它
    const AccountConfig& account_; // const 引用：只读访问配置
```

**引用 vs 指针的选择原则：**
- 能用引用就用引用（更安全）
- 需要表示"可能没有值"时用指针（指针可以是 nullptr）
- 与 C 库（如 libcurl）交互时必须用指针

---

## 3. 函数与参数传递

### 3.1 函数定义

```cpp
// 声明（.h 文件）
返回类型 函数名(参数列表);

// 实现（.cpp 文件）
返回类型 函数名(参数列表) {
    函数体
    return 返回值;
}
```

**项目例子（`utils.h` 和 `utils.cpp`）：**
```cpp
// 声明
std::string fmt_elapsed(double secs);

// 实现
std::string fmt_elapsed(double secs) {
    int s = static_cast<int>(secs);
    if (s < 60)   return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m " + std::to_string(s % 60) + "s";
    return std::to_string(s / 3600) + "h "
         + std::to_string((s % 3600) / 60) + "m "
         + std::to_string(s % 60) + "s";
}
```

### 3.2 四种参数传递方式对比

```cpp
// ① 值传递：复制一份，函数内修改不影响外部
void f1(std::string s) {
    s = "modified";  // 只修改副本
}

// ② 引用传递：不复制，函数内修改会影响外部
void f2(std::string& s) {
    s = "modified";  // 修改了调用者的变量！
}

// ③ const 引用传递：不复制，只读（最常用，性能最好）
void f3(const std::string& s) {
    // s = "modified";  // 编译错误：const 引用不能修改
    std::cout << s;     // 只能读
}

// ④ 指针传递：可以为 nullptr，需要解引用
void f4(std::string* s) {
    if (s != nullptr) {
        *s = "modified";
    }
}
```

**项目中的选择规律：**
- `const std::string&` — 传入字符串、路径（99% 的情况）
- `std::string&` — 函数需要往外写结果（如 `PreparedVideo& out`）
- `int`, `bool`, `double` — 小类型直接值传递，复制成本可以忽略

### 3.3 默认参数

```cpp
// utils.h
std::string create_cover_thumb(
    const std::string& orig_jpg,
    const std::string& log_prefix = "");   // 默认空字符串
//                                 ^^^^^^^ 有默认值时可以不传

// 调用方式：
create_cover_thumb(thumb_path);          // log_prefix 用默认值 ""
create_cover_thumb(thumb_path, pfx);     // 显式传入 pfx
```

---

## 4. 控制流

### 4.1 if / else

```cpp
// main.cpp
if (command == "bot-upload") {
    cmd_bot_upload(dir, recursive);
} else if (command == "upload") {
    cmd_upload(account, dir, recursive, concurrent);
} else {
    print_usage();
    return 1;
}
```

**短路求值：**
```cpp
// bot_client.cpp — 解析 JSON
if (j.contains("result") && j["result"].is_object()) {
//  ^^^^^^^^^^^^^^^^^^^^^^^^ 左边为 false 就不执行右边
//  防止 "result" 不存在时访问 j["result"] 崩溃
    resp.message_id = j["result"].value("message_id", int64_t(0));
}
```

### 4.2 for 循环

```cpp
// ① 传统 for
for (int i = 0; i < 3; i++) { ... }

// ② 范围 for（C++11，最常用）
std::vector<std::string> videos = {"a.mp4", "b.mp4"};
for (const auto& v : videos) {
//   ^^^^^^^^^^^ const 引用，不复制，不修改
    std::cout << v << "\n";
}

// ③ 带下标的传统 for
for (size_t i = 0; i < big_files.size(); i++) {
    const std::string& big_path = big_files[i];
    std::cout << "[" << (i + 1) << "/" << big_files.size() << "] " << big_path;
}
```

### 4.3 while + break

```cpp
// bot_uploader.cpp — 等待可用的 bot
while (true) {
    auto now = std::time(nullptr);
    for (int i = 0; i < n; i++) {
        if (now >= bot_flood_until_[idx]) {
            return idx;   // 找到可用 bot，直接返回
        }
    }
    // 所有 bot 都在冷却，等待
    std::this_thread::sleep_for(std::chrono::seconds(wait_secs));
}
```

```cpp
// 线程工作循环
while (true) {
    std::string vp;
    {
        std::lock_guard<std::mutex> lk(queue_mutex);
        if (work_queue.empty()) break;   // 队列空了，退出循环
        vp = work_queue.front();
        work_queue.pop();
    }
    upload_with_bot(i, vp);
}
```

### 4.4 try / catch / throw

```cpp
// 抛出异常
if (cfg.bots.empty()) {
    throw std::runtime_error("No bots configured in bots.json");
//  ^^^^^ 抛出一个异常对象，立刻跳出当前函数
}

// 捕获异常
try {
    bot_config = load_bot_config();   // 这行可能 throw
} catch (const std::exception& e) {
//               ^^^^^^^^^^^^^^ 捕获所有标准异常（runtime_error 是它的子类）
    std::cerr << "Error: " << e.what() << "\n";
//                              ^^^^^^ 获取错误信息字符串
    return;
}

// 捕获所有异常（包括非标准异常），通常用于"不管失败只要不崩"
try { fs::remove(tmp_part1); } catch (...) {}
//                                    ^^^ 省略号：捕获任何类型的异常
```

---

## 5. const 关键字的所有用法

`const` 表示"不可修改"，出现在不同位置意义不同：

```cpp
// ① const 变量：值不可修改
const int MAX_RETRIES = 3;
// MAX_RETRIES = 4;  // 编译错误

// ② const 引用参数：函数不会修改传入的值（最常见）
void foo(const std::string& path);
//       ^^^^^ 承诺不修改 path

// ③ 指向 const 的指针：不能通过指针修改值
const char* msg = "hello";
// *msg = 'H';  // 编译错误

// ④ const 指针：指针本身不能改变（指向固定）
char* const p = &buf[0];
// p = &buf[1];  // 编译错误，但 *p = 'X' 可以

// ⑤ const 成员函数：不会修改对象的成员变量
class BotClient {
public:
    std::string make_url(const std::string& method) const;
//                                                   ^^^^^ 这个函数不修改 BotClient 的状态
};
```

**项目中的真实例子（`bot_client.cpp`）：**
```cpp
std::string BotClient::make_url(const std::string& method) const {
    return api_url_ + "/bot" + token_ + "/" + method;
//         ^^^^^^^^         ^^^^^^^ 只读取成员变量，不修改
}

BotApiResponse BotClient::parse_response(const std::string& body) const {
    // body 是 const 引用：不复制，不修改
    // 函数末尾的 const：parse_response 不修改 BotClient 的状态
}
```

---

## 6. static 关键字的所有用法

`static` 在不同位置含义完全不同：

### 6.1 文件级 static：限制作用域

```cpp
// main.cpp
static void print_usage() { ... }
static void cmd_accounts() { ... }
static bool has_flag(int argc, char* argv[], const std::string& flag) { ... }
```

`static` 函数只在当前 `.cpp` 文件内可见，其他文件即使 `#include` 头文件也看不到它。这是隔离内部实现的方式。

**Python 对比**：类似于模块中以下划线开头的私有函数（`_helper()`），但 C++ 是编译器强制的。

### 6.2 函数内 static：只初始化一次

```cpp
// bot_uploader.cpp
void BotUploader::upload_directory(...) {
    for (size_t i = 0; i < folder_videos.size(); i++) {
        // ...
        if (i + 1 < folder_videos.size()) {
            static std::mt19937 rng(std::random_device{}());
//          ^^^^^^ 只在第一次调用时初始化，之后复用同一个随机数生成器
            // 如果不加 static，每次循环都重新初始化，随机性很差
            std::uniform_int_distribution<int> dist(3, 9);
            int delay = dist(rng);
        }
    }
}
```

### 6.3 static constexpr：文件级编译期常量

```cpp
// bot_uploader.cpp
static constexpr int64_t BOT_FILE_SIZE_LIMIT = 4000LL * 512 * 1024;
//     ^^^^^^^^^^^^^ static: 文件内可见
//            ^^^^^^^^^ constexpr: 编译时计算，运行时等于常量
```

`constexpr` vs `const`：
- `const int n = 42;` — 运行时常量，值可以来自运行时计算
- `constexpr int n = 42;` — 编译时常量，值必须在编译时已知，可以用于数组大小等

---

## 7. 命名空间 namespace

C++ 用命名空间避免名字冲突：

```cpp
// 完整写法（每次都写全名）
std::string s;
std::filesystem::path p;
std::chrono::steady_clock::now();

// 别名（项目中大量使用）
namespace fs = std::filesystem;   // 之后用 fs:: 代替 std::filesystem::
fs::path p("/tmp/file.mp4");
fs::exists(p);

// using 声明（引入单个名字）
using json = nlohmann::json;      // 之后直接写 json 代替 nlohmann::json
json j = json::parse(body);
```

**项目中每个 .cpp 文件几乎都有这两行：**
```cpp
namespace fs = std::filesystem;
using json = nlohmann::json;
```

**为什么不用 `using namespace std;`？**
这会把 std 的所有名字都引入，可能与自己的名字冲突（例如你自己定义了 `sort()` 函数，和 `std::sort` 冲突）。项目中只用精确的别名。

---

# 类型系统篇

---

## 8. struct：聚合数据

### 8.1 基本定义

```cpp
// config.h
struct AccountConfig {
    std::string name;
    std::string phone;
    int api_id = 0;          // = 0 是成员默认值（C++11）
    std::string api_hash;
    std::string channel_id;
    std::string session_dir;
};
```

**创建和使用：**
```cpp
AccountConfig config;        // 默认构造（int 成员为 0，string 为空）
config.name = "ajx";
config.api_id = 34927234;

std::cout << config.name;    // 用 . 访问成员
```

### 8.2 struct vs class 的真正区别

```cpp
struct Foo {
    int x;       // 默认 public（外部可直接访问）
    int y;
};

class Bar {
    int x;       // 默认 private（外部不能直接访问）
    int y;
public:
    int z;       // 显式声明 public
};
```

**项目惯例**：
- 纯数据容器（只有字段，没有行为）→ 用 `struct`：`AccountConfig`, `BotEntry`, `BotConfig`, `PreparedVideo`
- 有方法、有私有状态 → 用 `class`：`BotUploader`, `BotClient`, `TdClient`

---

## 9. class：封装行为

### 9.1 访问控制

```cpp
// bot_uploader.h
class BotUploader {
public:
    // ↓ 外部可以调用
    BotUploader(const BotConfig& config, const SharedSettings& settings);
    void upload_directory(const std::string& dir_path, bool recursive);

private:
    // ↓ 只有 BotUploader 的方法可以访问
    BotConfig config_;
    SharedSettings settings_;
    std::mutex log_mutex_;
    std::atomic<int> flood_wait_hits_{0};

    bool upload_with_bot(int bot_idx, const std::string& video_path);
    std::vector<std::string> scan_videos(const std::string& dir, bool recursive);
};
```

**为什么要 private？**
- `upload_with_bot()` 是内部实现细节，外部不应该直接调用
- `log_mutex_` 是内部线程同步工具，外部不需要知道
- 只暴露 `upload_directory()` 这一个入口，接口简洁

### 9.2 方法定义

在 `.cpp` 文件中实现时，用 `类名::方法名` 的格式：

```cpp
// bot_uploader.cpp

// BotUploader:: 前缀说明这个函数属于 BotUploader 类
bool BotUploader::upload_with_bot(int bot_idx, const std::string& video_path) {
    // 在方法内部，可以直接访问所有成员变量（不需要对象.）
    const auto& bot = config_.bots[bot_idx];
    //                 ^^^^^^^ 直接用成员变量名

    std::lock_guard<std::mutex> lk(log_mutex_);
    //                              ^^^^^^^^^^ 直接用另一个成员变量
}
```

---

## 10. 构造函数与成员初始化列表

### 10.1 构造函数

创建对象时自动调用，用于初始化成员变量：

```cpp
// bot_uploader.h — 声明
BotUploader(const BotConfig& config, const SharedSettings& settings);

// bot_uploader.cpp — 实现
BotUploader::BotUploader(const BotConfig& config, const SharedSettings& settings)
    : config_(config),                              // ← 成员初始化列表
      settings_(settings),
      bot_flood_until_(config.bots.size(), 0) {    // vector<time_t>(size, 初始值)
    // 函数体（通常为空，因为初始化已在列表里完成）
}
```

### 10.2 成员初始化列表 vs 函数体赋值

```cpp
// ❌ 低效写法（先默认构造，再赋值 = 两步操作）
BotUploader::BotUploader(const BotConfig& config, ...) {
    config_ = config;    // 先调用 BotConfig 默认构造，再调用赋值运算符
}

// ✅ 高效写法（直接用参数构造 = 一步操作）
BotUploader::BotUploader(const BotConfig& config, ...)
    : config_(config) {  // 直接用 config 拷贝构造 config_
}
```

**必须用初始化列表的情况：**
1. 成员是引用（引用必须在声明时绑定）
2. 成员是 `const`
3. 成员没有默认构造函数

```cpp
// uploader.h
class Uploader {
    TdClient& client_;             // 引用成员，必须用初始化列表
    const AccountConfig& account_; // const 引用成员，必须用初始化列表
};
```

### 10.3 成员变量的就地初始化（C++11）

```cpp
// bot_uploader.h
class BotUploader {
    int current_bot_idx_ = 0;              // 就地初始化，构造时自动赋 0
    std::atomic<int> flood_wait_hits_{0};  // {} 初始化语法（与 = 0 等价）
    std::atomic<int> flood_wait_total_secs_{0};
};
```

---

## 11. 析构函数与 RAII

### 11.1 析构函数

对象销毁时自动调用（函数返回、离开作用域、delete 时）：

```cpp
// bot_client.cpp
BotClient::~BotClient() {
//         ^ ~ 波浪号表示析构函数
    if (curl_) {
        curl_easy_cleanup(reinterpret_cast<CURL*>(curl_));
        // 自动释放 libcurl 资源，不需要调用者手动清理
    }
}
```

**使用 BotClient 的代码不需要关心清理：**
```cpp
{
    BotClient client(config_.api_url, bot.token);
    auto resp = client.send_video(...);
    // ...
}  // 离开作用域，client 析构，自动调用 curl_easy_cleanup
```

### 11.2 RAII 全面理解

RAII = 对象的**生命周期**管理**资源**的**生命周期**：

| RAII 对象 | 构造时（获取资源） | 析构时（释放资源） |
|-----------|-------------------|-------------------|
| `BotClient` | `curl_easy_init()` | `curl_easy_cleanup()` |
| `std::lock_guard` | `mutex.lock()` | `mutex.unlock()` |
| `std::ifstream` | 打开文件 | 关闭文件 |
| `std::vector` | 分配堆内存 | 释放堆内存 |

**为什么 RAII 很重要？**

```cpp
// ❌ 没有 RAII 的 C 风格代码（容易出错）
void bad() {
    CURL* curl = curl_easy_init();
    if (!curl) return;    // 这里还好

    // ... 100 行代码 ...

    if (error) {
        return;           // 忘记 cleanup！内存泄漏
    }

    curl_easy_cleanup(curl);  // 正常路径才能到这里
}

// ✅ RAII 风格（BotClient 析构函数保证一定清理）
void good() {
    BotClient client(url, token);  // 构造时初始化 curl

    if (error) {
        return;  // 离开作用域，client 析构，自动 cleanup
    }
}  // 自动 cleanup
```

---

## 12. 类型转换

### 12.1 static_cast：安全的数值转换

```cpp
// 整数 → 浮点（防止整数除法精度丢失）
double bitrate = static_cast<double>(file_size) / info.duration;
// 如果不转换：file_size / info.duration 是整数除法，会截断小数

// 浮点 → 整数（截断小数部分）
int total = static_cast<int>(seconds);  // 3.7 → 3

// 有符号 → 无符号（消除编译器警告）
size_t n = static_cast<size_t>(n_bots);

// 计算百分比
int pct = static_cast<int>(ulnow * 100 / ultotal);
```

### 12.2 reinterpret_cast：位模式重解释

```cpp
// bot_client.cpp
// curl_ 声明为 void*（通用指针，不知道指向什么类型）
void* curl_;

// 使用时告诉编译器："我知道这实际上是 CURL*"
CURL* curl = reinterpret_cast<CURL*>(curl_);
curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
```

**为什么用 `void*` 而不直接用 `CURL*`？**
如果头文件里写 `CURL* curl_`，所有 include `bot_client.h` 的文件都需要 `#include <curl/curl.h>`。用 `void*` 可以把 libcurl 的依赖隔离在 `bot_client.cpp` 里，减少编译依赖。

---

# 标准库篇

---

## 13. std::string 完全指南

### 13.1 构造与基本操作

```cpp
std::string s1 = "hello";          // 从字符串字面量构造
std::string s2("world");           // 直接构造
std::string s3(5, 'A');            // 重复字符："AAAAA"
std::string s4 = s1 + " " + s2;   // 拼接：+ 运算符（每次 + 都创建新字符串）
s1 += " world";                    // 追加（比多次 + 更高效）

// 长度
s1.size();     // 字节数（size_t 类型）
s1.empty();    // 是否为空字符串

// 访问字符
char c = s1[0];     // 'h'，不检查越界（越界是未定义行为）
char c2 = s1.at(0); // 'h'，越界抛 std::out_of_range 异常
```

### 13.2 查找与截取

```cpp
// utils.cpp — is_video_file()
std::string path = "/mnt/video.mp4";

// rfind: 从右往左找，返回位置（size_t）
auto dot = path.rfind('.');       // 找最后一个 .
// 如果找不到，返回 std::string::npos（最大 size_t 值，约 18 亿亿）

if (dot == std::string::npos) return false;

// substr(开始位置)        → 截取到末尾
// substr(开始位置, 长度)  → 截取指定长度
std::string ext = path.substr(dot);      // ".mp4"
std::string dir = path.substr(0, dot);   // "/mnt/video"

// find: 从左往右找
size_t pos = path.find("video");         // 5（找到位置）
size_t pos2 = path.find("xyz");          // std::string::npos（没找到）
```

### 13.3 转换

```cpp
// 整数 → 字符串
std::string s = std::to_string(42);         // "42"
std::string s2 = std::to_string(3.14);      // "3.140000"

// 字符串 → 整数（失败会抛异常 std::invalid_argument 或 std::out_of_range）
int n = std::stoi("42");                    // 42
int64_t n2 = std::stoll("1000000000");      // long long
double d = std::stod("3.14");              // 浮点

// std::string → const char*（给 C 函数用）
const char* cstr = s.c_str();
// 注意：cstr 的有效期只到 s 被修改或销毁为止

// const char* → std::string（自动转换）
std::string back = cstr;
```

### 13.4 std::transform 批量转换

```cpp
// utils.cpp — 转小写
std::string ext = path.substr(dot);
std::transform(
    ext.begin(),   // 起始迭代器（指向第一个字符）
    ext.end(),     // 结束迭代器（指向最后一个字符之后）
    ext.begin(),   // 输出到同一位置（原地修改）
    ::tolower      // 对每个字符调用 tolower()
);
// 把 ".MP4" 变成 ".mp4"
```

### 13.5 格式化输出（snprintf）

```cpp
// utils.cpp — format_file_size
char buf[64];   // 栈上的固定大小字符数组
std::snprintf(buf, sizeof(buf), "%.2f GB", 1.95);
// buf 现在包含 "1.95 GB\0"
// sizeof(buf) = 64，传入防止缓冲区溢出（如果格式化结果超过 63 字符，自动截断）
return buf;  // char[] 隐式转换为 std::string
```

---

## 14. std::vector 完全指南

### 14.1 构造

```cpp
std::vector<std::string> v1;                // 空 vector
std::vector<int> v2(5, 0);                  // 5 个 0：[0,0,0,0,0]
std::vector<std::string> v3 = {"a", "b"};   // 初始化列表构造
```

### 14.2 增删改查

```cpp
std::vector<std::string> videos;

// 添加元素
videos.push_back("video1.mp4");     // 复制添加到末尾
videos.emplace_back("video2.mp4");  // 原地构造，避免一次复制（更高效）

// 读取
videos[0];              // 第一个元素，不检查越界
videos.at(0);           // 检查越界，越界抛异常
videos.front();         // 第一个元素
videos.back();          // 最后一个元素

// 大小
videos.size();          // 元素数量（size_t）
videos.empty();         // 是否为空

// 排序（需要 #include <algorithm>）
std::sort(videos.begin(), videos.end());            // 默认字母序
std::sort(videos.begin(), videos.end(), 比较函数);  // 自定义排序
```

### 14.3 遍历

```cpp
// ① 范围 for（推荐）
for (const auto& v : videos) {
    std::cout << v << "\n";
}

// ② 下标（需要用到 i 的场合）
for (size_t i = 0; i < videos.size(); i++) {
    std::cout << i << ": " << videos[i] << "\n";
}
```

### 14.4 项目中的典型用法

```cpp
// scan_videos() 返回文件路径列表
std::vector<std::string> videos;
// 循环里 push_back ...
std::sort(videos.begin(), videos.end());  // 排序保证上传顺序一致

// 放入工作队列
std::queue<std::string> work_queue;
for (const auto& v : videos) work_queue.push(v);

// 每个 bot 的上传计数（各自独立下标，不需要加锁）
std::vector<int> bot_uploads(n_bots, 0);
// 线程 i 只写 bot_uploads[i]，其他线程写不同下标，天然无竞争
```

---

## 15. std::map 与 std::queue

### 15.1 std::map：有序键值对

```cpp
// bot_uploader.cpp — 按文件夹分组视频
std::map<std::string, std::vector<std::string>> folder_groups;
//       ^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^
//       key: 文件夹路径  value: 该文件夹下的视频列表

for (const auto& vp : videos) {
    std::string folder = fs::path(vp).parent_path().string();
    folder_groups[folder].push_back(vp);
    // 如果 folder 这个 key 不存在，[] 运算符自动创建空 vector
}

// 遍历（C++17 结构化绑定）
for (auto& [folder_path, folder_videos] : folder_groups) {
    std::sort(folder_videos.begin(), folder_videos.end());
    // ...
}
```

**map 的特性**：按 key 字母序自动排序，查找时间 O(log n)。

### 15.2 std::queue：先进先出队列

```cpp
// bot_uploader.cpp — 并行上传的工作队列
std::queue<std::string> work_queue;

// 入队（主线程，单次操作）
for (const auto& v : videos) work_queue.push(v);

// 出队（多个 bot 线程竞争取任务，必须加锁）
{
    std::lock_guard<std::mutex> lk(queue_mutex);
    if (work_queue.empty()) break;     // 空了退出
    vp = work_queue.front();           // 查看队首
    work_queue.pop();                  // 取出队首
}
```

**queue 的特性**：只能 `push()`（队尾加）和 `pop()`（队首取），不能随机访问，不能遍历。

---

## 16. std::filesystem 完全指南

`#include <filesystem>`，C++17 标准库，需要编译器支持。

### 16.1 path 类型

```cpp
namespace fs = std::filesystem;

fs::path p("/mnt/storage/alice/video.mp4");

// 分解路径（返回 fs::path，用 .string() 转成 std::string）
p.parent_path().string()   // "/mnt/storage/alice"
p.filename().string()      // "video.mp4"
p.stem().string()          // "video"（无扩展名部分）
p.extension().string()     // ".mp4"

// 路径拼接（/ 运算符）
fs::path thumb = p.parent_path() / (p.stem().string() + ".jpg");
// 结果: fs::path("/mnt/storage/alice/video.jpg")

// 解析符号链接，得到绝对真实路径
fs::path abs = fs::canonical(fs::path(argv[0]));
// 用于 main.cpp 中定位可执行文件所在目录
```

### 16.2 文件查询

```cpp
fs::exists(p)                // 是否存在（文件或目录）
fs::is_directory(p)          // 是否是目录
fs::is_regular_file(p)       // 是否是普通文件（非符号链接、设备文件等）
fs::file_size(p)             // 文件大小（字节），返回 uintmax_t
fs::current_path()           // 当前工作目录
```

### 16.3 目录遍历

```cpp
// 非递归（只看当前目录，不进子目录）
for (auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
        std::string path = entry.path().string();
    }
}

// 递归（遍历所有子目录）
for (auto& entry : fs::recursive_directory_iterator(
         dir,
         fs::directory_options::skip_permission_denied)) {
//                              ^^^^^^^^^^^^^^^^^^^^^^^^ 没权限的目录跳过，不报错
    if (entry.is_regular_file() && entry.path().extension() == ".big") {
        big_files.push_back(entry.path().string());
    }
}
```

### 16.4 文件操作

```cpp
// 创建目录（递归，类似 mkdir -p）
fs::create_directories("/data/thumbs");

// 重命名 / 移动（同分区内是原子操作）
fs::rename(tmp_path, final_path);

// 删除单个文件
fs::remove(path);

// 创建空标记文件
std::ofstream(video_path + ".big").close();
// ofstream 构造时创建/打开文件，.close() 立刻关闭
// 结合 RAII：即使不调用 close()，离开作用域也会自动关闭
```

### 16.5 异常保护

```cpp
// 删除临时文件（失败无所谓）
try { fs::remove(tmp_part1); } catch (...) {}

// 重命名失败则记录错误（不影响程序继续）
try {
    fs::rename(tmp_part1, original_path);
    fs::remove(big_path);
    fixed++;
} catch (const std::exception& e) {
    std::cerr << "Replace failed: " << e.what() << "\n";
    failed++;
}
```

---

## 17. Lambda 表达式完全指南

### 17.1 语法结构

```
[捕获列表](参数列表) -> 返回类型 { 函数体 }
                返回类型通常省略，由编译器推断
```

### 17.2 捕获列表详解

```cpp
int x = 10;
std::string name = "hello";

// ① 不捕获任何外部变量
auto f1 = []() { return 42; };

// ② 值捕获（复制一份，外部变量改变不影响 lambda）
auto f2 = [x]() { return x + 1; };

// ③ 引用捕获（共享外部变量，lambda 内修改会影响外部）
auto f3 = [&x]() { x++; };

// ④ 捕获所有外部变量（引用方式）
auto f4 = [&]() { x++; name = "world"; };

// ⑤ 捕获所有外部变量（值方式）
auto f5 = [=]() { return x + name.size(); };

// ⑥ 混合捕获（本项目多线程的关键写法）
for (int i = 0; i < n_bots; i++) {
    threads.emplace_back([&, i]() {
//                        ^  ^ 值捕获 i（每个线程有独立的 bot 序号）
//                        | 引用捕获其他所有变量（work_queue, queue_mutex 等）
        upload_with_bot(i, vp);  // 这个 i 是 lambda 创建时复制的值
    });
}
```

**为什么多线程必须值捕获 `i`？**
```cpp
// ❌ 错误写法：引用捕获 i
for (int i = 0; i < 3; i++) {
    threads.emplace_back([&]() {
        // 所有线程共享同一个 i 的引用
        // 当线程实际执行时，for 循环早已结束，i == 3
        upload_with_bot(i, vp);  // 3 个线程都用 i=3，全错！
    });
}

// ✅ 正确写法：值捕获 i
for (int i = 0; i < 3; i++) {
    threads.emplace_back([&, i]() {
        // 创建 lambda 时复制当前 i 的值
        // 线程 0: i=0，线程 1: i=1，线程 2: i=2
        upload_with_bot(i, vp);  // 正确
    });
}
```

### 17.3 Lambda 作为局部辅助函数

```cpp
// bot_client.cpp — send_video()
// 定义一个只在这个函数内用的辅助 lambda，避免重复代码
auto add_str = [&](const char* name, const std::string& val) {
    curl_mimepart* part = curl_mime_addpart(form);  // 捕获了外部的 form
    curl_mime_name(part, name);
    curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
};

// 多次调用，代替重复的 4 行代码
add_str("chat_id",          chat_id);
add_str("caption",          caption);
add_str("parse_mode",       "HTML");
add_str("duration",         std::to_string(duration));
add_str("width",            std::to_string(width));
add_str("supports_streaming", "true");
```

---

## 18. 多线程完全指南

### 18.1 std::thread 创建线程

```cpp
#include <thread>

// 创建线程，传入可调用对象（lambda）
std::thread t([]() {
    std::cout << "在新线程里运行\n";
});

// 必须 join 或 detach，否则 thread 析构时程序崩溃
t.join();    // 等待线程结束（主线程阻塞在这里）
// t.detach(); // 让线程在后台独立运行（本项目不用）
```

### 18.2 项目的多线程模型

```cpp
// bot_uploader.cpp — 并行上传
std::vector<std::thread> threads;

for (int i = 0; i < n_bots; i++) {
    threads.emplace_back([&, i]() {
        while (true) {
            // Step 1: 加锁取任务
            std::string vp;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (work_queue.empty()) break;
                vp = work_queue.front();
                work_queue.pop();
            }  // 锁在这里释放

            // Step 2: 上传（耗时操作，不持锁）
            if (upload_with_bot(i, vp)) {
                handle_post_upload(vp);
                par_success++;   // atomic，不需要锁
            } else {
                par_skipped++;
            }

            // Step 3: 检查是否还有任务
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (work_queue.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    });
}

for (auto& t : threads) t.join();  // 等所有线程结束
```

### 18.3 mutex 与 lock_guard 深度理解

```cpp
#include <mutex>

std::mutex log_mutex_;

// ❌ 手动加锁（容易出错）
log_mutex_.lock();
std::cout << "日志信息\n";  // 如果这里抛异常，unlock 永远不会执行 → 死锁
log_mutex_.unlock();

// ✅ lock_guard（RAII，推荐）
{
    std::lock_guard<std::mutex> lk(log_mutex_);  // 构造时 lock()
    std::cout << "日志信息\n";
}  // 析构时自动 unlock()，无论是否发生异常
```

**为什么打印日志也需要锁？**

不加锁时两个线程同时写 `std::cout` 的输出会交错：
```
[INFO] Bo[INFO] Bot2 uploadingt1 uploading   ← 混乱
```
加锁后：
```
[INFO] Bot1 uploading
[INFO] Bot2 uploading                         ← 正常
```

**锁的粒度控制：**
```cpp
// ✅ 锁的范围尽量小——只保护需要保护的代码
{
    std::lock_guard<std::mutex> lk(log_mutex_);
    std::cout << "[INFO] " << pfx << "Success: " << fname << "\n";
}
// 锁释放
// 下面这些耗时操作不占锁，让其他线程能打印日志
record_uploaded_video(...);
std::this_thread::sleep_for(std::chrono::seconds(2));
```

### 18.4 std::atomic 原子操作

```cpp
#include <atomic>

std::atomic<int> par_success{0};  // 初始值 0

// 多线程安全地修改，不需要 mutex
par_success++;                    // 原子递增
par_success.load();               // 原子读取当前值
par_success.store(0);             // 原子写入

// 为什么普通 int++ 在多线程下不安全：
// CPU 执行 n++ 是三步：读 n → 加 1 → 写回 n
// 两个线程同时读到 n=5，各自 +1，各自写回 6 → 最终 n=6，只加了 1 次！
// atomic 把这三步变成一个不可打断的原子操作
```

**atomic vs mutex 的选择：**

| 场景 | 选择 |
|------|------|
| 单个整数的 +1/-1 | `atomic` |
| 读一个值然后基于它做决策 | `mutex`（读-判断-写 需要原子性） |
| 保护多个变量的一致性 | `mutex` |
| 打印多行输出 | `mutex` |
| 向队列 push/pop | `mutex` |

---

## 19. std::chrono 计时

```cpp
#include <chrono>

// 记录开始时间（steady_clock：单调时钟，不受系统时间调整影响）
auto t0 = std::chrono::steady_clock::now();

// ... 执行操作 ...

// 计算时差，转换为秒（浮点）
double secs = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - t0
).count();

std::cout << "耗时: " << fmt_elapsed(secs) << "\n";

// 线程休眠
std::this_thread::sleep_for(std::chrono::seconds(5));        // 整数秒
std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 毫秒
int n = resp.retry_after;
std::this_thread::sleep_for(std::chrono::seconds(n + 1));    // 动态时长
```

---

# 第三方库篇

---

## 20. nlohmann/json 库

[nlohmann/json](https://github.com/nlohmann/json) 是 C++ 最流行的 JSON 库，只需要一个头文件。

### 20.1 包含与别名

```cpp
#include <nlohmann/json.hpp>
using json = nlohmann::json;
```

### 20.2 解析

```cpp
// 从字符串解析
std::string body = R"({"ok":true,"result":{"message_id":12345}})";
json j = json::parse(body);
// 解析失败（非合法 JSON）会抛 json::parse_error

// 从文件流解析（更高效，不需要先读成 string）
std::ifstream f(path);
json data = json::parse(f);
```

### 20.3 读取字段

```cpp
// ① [] 运算符：字段不存在时抛异常
bool ok = j["ok"];                            // bool
int64_t id = j["result"]["message_id"];       // 嵌套访问

// ② value()：字段不存在时返回默认值（更安全，推荐）
bool ok = j.value("ok", false);
int retry = j.value("retry_after", 60);
std::string desc = j.value("description", "unknown error");

// ③ 类型检查（先检查再访问，最安全）
if (j.contains("result") && j["result"].is_object()) {
    resp.message_id = j["result"].value("message_id", int64_t(0));
}

// ④ get<T>()：显式指定目标类型
std::string token = token_val.get<std::string>();
int64_t id = j["message_id"].get<int64_t>();
```

### 20.4 遍历 JSON 对象

```cpp
// bot_uploader.cpp — 遍历 bots 字段
// JSON: {"Bot1": "token1", "Bot2": "token2"}
for (auto& [name, token_val] : data["bots"].items()) {
//          ^^^^  ^^^^^^^^^^ C++17 结构化绑定
    BotEntry e;
    e.name  = name;                           // key
    e.token = token_val.get<std::string>();   // value
    cfg.bots.push_back(std::move(e));
}
```

### 20.5 std::move 是什么？

```cpp
cfg.bots.push_back(std::move(e));
```

- 普通 `push_back(e)`：**复制** `e` 放入 vector，原来的 `e` 还在，有完整数据
- `push_back(std::move(e))`：**移动** `e` 的资源进 vector，`e` 变成空状态（不再有效）

**为什么用 move？** 对于含 `std::string` 成员的 struct，move 只需转移字符串内部的指针，不需要逐字符复制，大字符串时性能差异显著。

---

## 21. libcurl 库

[libcurl](https://curl.se/libcurl/) 是 C 语言写的 HTTP 客户端库，本项目用来上传文件。

### 21.1 初始化与清理（RAII 封装在 BotClient）

```cpp
// bot_client.cpp

// 构造函数：获取资源
BotClient::BotClient(const std::string& api_url, const std::string& token)
    : api_url_(api_url), token_(token), curl_(nullptr) {
    curl_ = curl_easy_init();   // 创建 easy handle（每个上传请求一个）
    if (!curl_) {
        throw std::runtime_error("Failed to initialize libcurl");
    }
}

// 析构函数：释放资源
BotClient::~BotClient() {
    if (curl_) {
        curl_easy_cleanup(reinterpret_cast<CURL*>(curl_));
    }
}
```

### 21.2 发送 multipart/form-data（上传文件的标准方式）

```cpp
// bot_client.cpp — do_post()
CURL* curl = reinterpret_cast<CURL*>(curl_);

// 重置上一次的配置（复用 handle）
curl_easy_reset(curl);

// 设置各项参数（CURLOPT_XXX 是 libcurl 的枚举常量）
curl_easy_setopt(curl, CURLOPT_URL, url.c_str());       // 请求 URL
curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);          // multipart body
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);             // 0 = 不超时
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);     // 连接超时 30 秒
curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb); // 接收响应的回调
curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body); // 传给回调的指针

// 执行请求（阻塞，直到完成或出错）
CURLcode rc = curl_easy_perform(curl);
if (rc != CURLE_OK) {
    // curl_easy_strerror() 把错误码转成人类可读的字符串
    resp.error = std::string("curl error: ") + curl_easy_strerror(rc);
}
```

### 21.3 构建 multipart 表单

```cpp
// bot_client.cpp — send_video()

// 创建表单
curl_mime* form = curl_mime_init(curl);

// 添加文本字段（辅助 lambda 减少重复代码）
auto add_str = [&](const char* name, const std::string& val) {
    curl_mimepart* part = curl_mime_addpart(form);
    curl_mime_name(part, name);
    curl_mime_data(part, val.c_str(), CURL_ZERO_TERMINATED);
};
add_str("chat_id", chat_id);
add_str("caption", caption);
add_str("supports_streaming", "true");

// 添加文件字段（libcurl 自动读取文件内容上传）
curl_mimepart* file_part = curl_mime_addpart(form);
curl_mime_name(file_part, "video");
curl_mime_filedata(file_part, video_path.c_str());  // 只传路径，libcurl 自己读

// 添加缩略图
if (!thumb_path.empty()) {
    curl_mimepart* tp = curl_mime_addpart(form);
    curl_mime_name(tp, "thumbnail");
    curl_mime_filedata(tp, thumb_path.c_str());
}

// 发送并释放
BotApiResponse resp = do_post(make_url("sendVideo"), form, log_prefix_);
curl_mime_free(form);  // 必须手动释放！
```

### 21.4 回调函数

libcurl 用 C 风格回调（函数指针），因为它是 C 库，不支持 lambda：

```cpp
// 接收 HTTP 响应体的回调（每收到一块数据就调用一次）
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    // userdata 是我们传入的 &response_body，转回 string* 后追加数据
    s->append(ptr, size * nmemb);
    return size * nmemb;  // 必须返回处理的字节数，返回其他值会中断请求
}

// 上传进度回调（定期调用，汇报进度）
static int progress_xfer_cb(void* clientp,
                             curl_off_t dltotal, curl_off_t dlnow,  // 下载进度（我们不关心）
                             curl_off_t ultotal, curl_off_t ulnow)  // 上传进度
{
    if (ultotal <= 0 || ulnow <= 0) return 0;
    auto* ctx = static_cast<UploadProgress*>(clientp);
    int pct = static_cast<int>(ulnow * 100 / ultotal);
    int rounded = (pct / 10) * 10;  // 取整到 10%，避免刷屏
    if (rounded > ctx->last_pct) {
        ctx->last_pct = rounded;
        std::cout << "[INFO] " << ctx->prefix
                  << "↑ " << format_file_size(ulnow) << " / "
                  << format_file_size(ultotal)
                  << " (" << rounded << "%)\n";
        std::cout.flush();  // 立即输出，不等缓冲区满
    }
    return 0;  // 非 0 会中断上传
}
```

---

# 工程篇

---

## 22. 头文件与 cpp 的分工

### 22.1 为什么要分开

10 个 `.cpp` 文件都需要用 `BotUploader`：
- 把**声明**（"这里有什么"）放进 `.h` 文件，所有人 `#include` 它
- 把**实现**（"怎么做"）放进 `bot_uploader.cpp`，只编译一次
- 链接时把编译产物合并成一个可执行文件

```
main.cpp          → include bot_uploader.h → 知道 BotUploader 的接口
config.cpp        → include bot_uploader.h → 知道 BotUploader 的接口
bot_uploader.cpp  → 真正实现 BotUploader 的所有方法
```

### 22.2 #pragma once 防止重复包含

```
main.cpp
  #include "bot_uploader.h"     ← 第一次包含
  #include "uploader.h"
    #include "bot_uploader.h"   ← uploader.h 里又包含了一次
```

没有 `#pragma once` 的话，`BotUploader` 的定义出现两次，编译器报"重复定义"错误。
`#pragma once` 让编译器记住"这个文件我已经处理过了，跳过"。

### 22.3 include 路径

```cpp
#include <string>          // 尖括号：系统/第三方库头文件（在系统路径找）
#include "bot_client.h"    // 引号：项目内部头文件（从当前文件所在目录找）
```

---

## 23. CMake 构建系统

`CMakeLists.txt` 是描述"怎么编译这个项目"的配置文件。

### 23.1 主要配置解读

```cmake
cmake_minimum_required(VERSION 3.16)   # 最低 CMake 版本要求
project(tg_uploader)                   # 项目名

set(CMAKE_CXX_STANDARD 17)             # 使用 C++17 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)    # 如果编译器不支持 C++17，报错

# 定义可执行文件：名字 + 所有 .cpp 源文件
add_executable(tg_uploader
    src/main.cpp
    src/bot_client.cpp
    src/bot_uploader.cpp
    src/uploader.cpp
    src/td_client.cpp
    src/scanner.cpp
    src/config.cpp
    src/utils.cpp
    src/caption.cpp
    src/video_info.cpp
    src/folder_lock.cpp
)

# 链接第三方库（PRIVATE = 只有 tg_uploader 需要，不传递给依赖方）
target_link_libraries(tg_uploader
    PRIVATE
    curl         # libcurl：HTTP 上传
    sqlite3      # SQLite：数据库
    Td::TdStatic # TDLib：Telegram 协议库
)
```

### 23.2 编译命令

```bash
# 在 tg_uploader/ 目录下
mkdir build && cd build

# 生成构建系统（读取上级目录的 CMakeLists.txt）
cmake ..

# 并行编译（$(nproc) 是 CPU 核心数，如 8 核就是 make -j8）
make -j$(nproc)

# 生成的可执行文件就在 build/ 目录
./tg_uploader bot-upload --dir /mnt/storage --recursive
```

**为什么要 `mkdir build`？** CMake 会生成大量中间文件（Makefile、.o 对象文件等），放在单独的 `build/` 目录里不污染源码，方便清理（`rm -rf build/`）。

---

## 24. 整体架构梳理

### 24.1 两种上传模式

```
tg_uploader bot-upload         tg_uploader upload --account ajx
       |                                |
  BotUploader                       Uploader
       |                                |
  BotClient (libcurl)              TdClient (TDLib)
       |                                |
  telegram-bot-api（本地）       Telegram 服务器（直连）
       |
  Telegram 服务器
```

### 24.2 并行上传数据流

```
主线程:
  scan_videos()
    → [video1.mp4, video2.mp4, video3.mp4, ...]
    → work_queue（先进先出）

线程 0 (Bot1):        线程 1 (Bot2):        线程 2 (Bot3):
  加锁取 video1         加锁取 video2         加锁取 video3
  解锁                  解锁                  解锁
  上传（4分钟）         上传（4分钟）         上传（4分钟）
  par_success++         par_success++         par_success++
  加锁取 video4         加锁取 video5         加锁取 video6
  ...                   ...                   ...

主线程: join() 等所有线程结束
  → 打印 "Batch complete: 9/9 uploaded"
```

### 24.3 关键数据结构关系

```
BotConfig ──────────────────────────────────────────┐
  ├── api_url: "http://127.0.0.1:8081"              │
  ├── channel_id: "@mychannel"                      │
  └── bots: vector<BotEntry>                        │
              ├── {name:"Bot1", token:"123:ABC"}    │
              └── {name:"Bot2", token:"456:DEF"}    │
                                                    │
BotUploader(持有 BotConfig) ◄───────────────────────┘
  ├── upload_directory()        ← 外部调用入口
  │     ├── scan_videos()       ← 扫描目录，得到文件列表
  │     ├── work_queue          ← 文件路径队列
  │     ├── queue_mutex         ← 保护 work_queue
  │     └── thread × N         ← 每个 bot 一个线程
  │           └── upload_with_bot(i, path)
  │                 ├── 文件大小检查（> 2GiB 则 .big 标记跳过）
  │                 ├── BotClient.send_video()
  │                 │     └── libcurl HTTP multipart POST
  │                 └── 成功则 record_uploaded_video() 写数据库
  ├── log_mutex_                ← 保护 cout 输出
  ├── par_success (atomic)      ← 成功计数
  └── par_skipped (atomic)      ← 跳过计数
```

---

## 推荐阅读顺序

按此顺序阅读源码，难度循序渐进：

| 步骤 | 文件 | 涉及概念 |
|------|------|---------|
| 1 | `utils.h` + `utils.cpp` | 函数、字符串操作、文件 I/O、snprintf |
| 2 | `config.h` + `config.cpp` | struct、JSON 解析、文件读取 |
| 3 | `bot_client.h` + `bot_client.cpp` | class、构造/析构、libcurl、回调函数 |
| 4 | `bot_uploader.h` + `bot_uploader.cpp` | 多线程、mutex、atomic、lambda、工作队列 |
| 5 | `main.cpp` | 命令行解析、信号处理、整体流程 |

## 参考资料

- **[cppreference.com](https://cppreference.com)** — C++ 标准库权威文档，查任何函数都用它
- **[learncpp.com](https://www.learncpp.com)** — 系统学 C++ 的免费网站，从零开始
- **[libcurl 文档](https://curl.se/libcurl/c/)** — libcurl API 参考，尤其是 `curl_easy_setopt` 的选项列表
- **[nlohmann/json 文档](https://json.nlohmann.me)** — JSON 库使用说明
- **C++ Primer（第5版）** — 适合系统学习的书，中文版可以找到
