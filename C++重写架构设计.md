# C++17 + TDLib 重写 Telegram 视频上传器 - 架构设计

## 概述

使用 C++17 + TDLib (Telegram Database Library) + libavformat (FFmpeg) 重写 Python 版上传器。功能完全一致：多账号并行上传视频到 Telegram 频道，包含 caption（模特名 hashtag + 日期 hashtag + 视频信息代码块）、缩略图作为视频封面、文件夹锁、上传后文件处理。

TDLib 内部自动处理文件分块上传、限流重试、session 持久化，Python 版里 ~300 行的手写分块上传代码在 C++ 版里变成一次 `sendMessage` 调用。

## 编译依赖

- TDLib 编译安装（头文件 + 静态库）
- FFmpeg 开发库（libavformat, libavcodec, libavutil）
- nlohmann/json（header-only，用于解析 JSON 配置）
- C++17 编译器（g++ 或 clang++）

## 项目文件结构

```
tg_uploader/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                   # CLI 参数解析，分发到子命令
│   ├── config.h / config.cpp      # AccountConfig + SharedSettings
│   ├── td_client.h / td_client.cpp    # TDLib 封装（认证、发送、查找频道）
│   ├── video_info.h / video_info.cpp  # libavformat 封装（分辨率、时长）
│   ├── caption.h / caption.cpp    # Caption + hashtag 生成
│   ├── folder_lock.h / folder_lock.cpp  # PID 文件夹锁
│   ├── uploader.h / uploader.cpp  # 目录扫描 + 上传编排
│   └── utils.h / utils.cpp       # format_file_size, is_video_file 等
```

---

## 各文件详细设计

### 1. `main.cpp` - 入口 + CLI

```cpp
// CLI 用法:
//   ./tg_uploader upload --account wang [--dir PATH] [--recursive]
//   ./tg_uploader login --account wang
//   ./tg_uploader accounts

int main(int argc, char* argv[]) {
    // 手写解析 argc/argv，不需要额外库
    // 分发到: cmd_upload(), cmd_login(), cmd_accounts()
}
```

三个子命令:
- `upload --account NAME [--dir PATH] [--recursive] [--no-recursive]`
- `login --account NAME`
- `accounts`

### 2. `config.h / config.cpp` - 配置管理

```cpp
struct AccountConfig {
    std::string name;        // "wang"
    std::string phone;       // "+855315376950"
    int api_id;              // 23128848
    std::string api_hash;    // "71233ff48dc5..."
    std::string channel_id;  // "ChaturbateRecord"
    std::string session_dir; // "tdlib_wang" (TDLib 数据库目录)
};

struct SharedSettings {
    bool delete_after_upload = false;
    bool mark_uploaded_files = true;
    std::string uploaded_suffix = ".uploaded";
    std::vector<std::string> exempt_folders;
    std::string default_upload_dir;
};

AccountConfig load_account(const std::string& name);
std::vector<AccountConfig> list_accounts();
SharedSettings load_settings();
```

复用现有配置文件: `.account_configs/accounts.json` + `{phone}.json` + `.env`

**关键区别**: TDLib 每个账号需要独立的 **database directory**（不是 .session 文件）。每个账号的数据库放在 `tdlib_{name}/` 目录下。

### 3. `td_client.h / td_client.cpp` - TDLib 封装（核心）

最核心的文件，封装 TDLib 的异步 API 为同步阻塞接口。

```cpp
class TdClient {
public:
    TdClient(const AccountConfig& config);
    ~TdClient();

    // 阻塞式接口（内部循环 receive 直到完成）
    bool login();              // 处理认证状态机
    bool is_authorized();
    int64_t find_channel(const std::string& channel_id);

    // 发送消息
    int64_t send_video(int64_t chat_id, const std::string& video_path,
                       const std::string& caption_text,
                       const std::string& thumb_path,
                       int duration, int width, int height);

    int64_t send_photo(int64_t chat_id, const std::string& photo_path,
                       const std::string& caption_text);

    void close();

private:
    std::unique_ptr<td::ClientManager> client_manager_;
    int32_t client_id_;
    AccountConfig config_;
    bool authorized_ = false;

    // 发送请求并同步等待响应
    td::td_api::object_ptr<td::td_api::Object> send_sync(
        td::td_api::object_ptr<td::td_api::Function> request);

    // 处理 authorization 状态更新
    void handle_auth_state(
        td::td_api::object_ptr<td::td_api::AuthorizationState> state);
};
```

#### 事件循环设计

采用**同步阻塞模式**。每次 `send_sync()` 内部循环调用 `client_manager_->receive(timeout)` 直到拿到对应 request_id 的响应。简单可靠，不需要异步回调。

#### 认证状态机

```
waitTdlibParameters → 设置 api_id/api_hash/database_directory
waitPhoneNumber     → 设置手机号
waitCode            → 从 stdin 读取验证码
waitPassword        → 从 stdin 读取 2FA 密码
ready               → authorized = true
```

#### find_channel

- 如果 channel_id 以 `@` 开头: 调用 `searchPublicChat(username)`
- 否则: `searchPublicChat(channel_id)` 或遍历 `getChats()` 匹配 title

#### send_video 核心逻辑

```cpp
auto video = td_api::make_object<td_api::inputMessageVideo>();
video->video_ = td_api::make_object<td_api::inputFileLocal>(video_path);
video->duration_ = duration;
video->width_ = width;
video->height_ = height;
video->supports_streaming_ = true;

// 缩略图作为封面
if (!thumb_path.empty()) {
    video->thumbnail_ = td_api::make_object<td_api::inputThumbnail>(
        td_api::make_object<td_api::inputFileLocal>(thumb_path), 320, 320
    );
}

// caption: 用 parseMarkdown 自动处理代码块
auto parse_result = send_sync(td_api::make_object<td_api::parseMarkdown>(
    td_api::make_object<td_api::formattedText>(caption_markdown, {})
));
video->caption_ = td_api::move_object_as<td_api::formattedText>(parse_result);

auto msg = td_api::make_object<td_api::sendMessage>();
msg->chat_id_ = chat_id;
msg->input_message_content_ = std::move(video);
send_sync(std::move(msg));
```

**TDLib 自动处理**: 文件分块上传、限流重试、session 持久化全部由 TDLib 内部完成。不需要手写任何上传逻辑。

### 4. `video_info.h / video_info.cpp` - libavformat 获取视频信息

```cpp
struct VideoInfo {
    int width = 0;
    int height = 0;
    double duration = 0.0;  // 秒
};

VideoInfo get_video_info(const std::string& path);
```

实现:
```cpp
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

VideoInfo get_video_info(const std::string& path) {
    VideoInfo info;
    AVFormatContext* fmt_ctx = nullptr;

    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0)
        return info;

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return info;
    }

    // 查找视频流
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            auto* par = fmt_ctx->streams[i]->codecpar;
            info.width = par->width;
            info.height = par->height;

            // 处理 SAR（像素宽高比）
            if (par->sample_aspect_ratio.num > 0 &&
                par->sample_aspect_ratio.den > 0) {
                double sar = (double)par->sample_aspect_ratio.num /
                             par->sample_aspect_ratio.den;
                if (sar != 1.0)
                    info.width = (int)(info.width * sar);
            }

            // 时长
            if (fmt_ctx->streams[i]->duration > 0) {
                auto tb = fmt_ctx->streams[i]->time_base;
                info.duration = (double)fmt_ctx->streams[i]->duration *
                                tb.num / tb.den;
            }
            break;
        }
    }

    // 备选: 从 format 级别获取时长
    if (info.duration <= 0 && fmt_ctx->duration > 0)
        info.duration = (double)fmt_ctx->duration / AV_TIME_BASE;

    avformat_close_input(&fmt_ctx);
    return info;
}
```

### 5. `caption.h / caption.cpp` - Caption 生成

```cpp
// 从文件名提取 #model_name #dateYYYYMMDD
std::string extract_hashtags(const std::string& file_path);

// 生成视频 caption（markdown 格式，带代码块）
std::string make_video_caption(const std::string& file_path,
                               const VideoInfo& info);

// 生成图片 caption
std::string make_image_caption(const std::string& file_path);
```

#### `extract_hashtags` 逻辑（与 Python 版完全一致）

1. 按 `_` 分割文件名
2. 查找平台关键词（Chaturbate, StripChat, OnlyFans, ManyVids, Cam4, Streamate, LiveJasmin）
3. 关键词之前的部分 = model_name
4. 关键词之后匹配 `\d{4}[-]?\d{2}[-]?\d{2}` = date
5. 返回 `"#model_name #dateYYYYMMDD"`

#### `make_video_caption` 输出格式

```
```
filename.mp4
Resolution: 1920x1080
Duration: 01:23:45
Size: 1.23 GB
```
#model_name #date20260213
```

### 6. `folder_lock.h / folder_lock.cpp` - 文件夹锁

```cpp
bool create_folder_lock(const std::string& folder,
                        const std::string& account_name);
bool is_folder_locked(const std::string& folder);
void remove_folder_lock(const std::string& folder);
```

与 Python 版完全一致:
- 写 `.uploading.lock` JSON 文件，包含 pid / account / timestamp
- 检查时验证 pid 是否存活（`kill(pid, 0)`）
- 6 小时超时自动清理僵死锁

### 7. `uploader.h / uploader.cpp` - 上传编排

```cpp
class Uploader {
public:
    Uploader(TdClient& client, const SharedSettings& settings,
             int64_t chat_id);

    void upload_directory(const std::string& dir_path, bool recursive);

private:
    TdClient& client_;
    SharedSettings settings_;
    int64_t chat_id_;

    std::vector<std::string> scan_videos(const std::string& dir,
                                         bool recursive);
    bool upload_single(const std::string& video_path);
    void handle_post_upload(const std::string& path);
    std::string find_thumbnail(const std::string& video_path);
};
```

#### `upload_directory` 流程

1. `scan_videos()` 递归扫描 .mp4/.avi/.mov/.mkv/.webm/.m4v/.flv/.wmv
2. 按文件夹分组
3. 选择未锁定的文件夹（random shuffle）
4. 逐个文件夹上传:
   - `create_folder_lock()`
   - 逐文件: `upload_single()` → `handle_post_upload()`
   - `remove_folder_lock()`

#### `upload_single` 流程

1. 检查文件大小（>2GB 标记为 .big 跳过）
2. `get_video_info()` 获取分辨率/时长
3. `find_thumbnail()` 查找同名 .jpg
4. `make_video_caption()` 生成 caption
5. `client_.send_video()` 发送视频（TDLib 自动处理上传）
6. 如果有缩略图: `client_.send_photo()` 单独发送缩略图作为独立图片

### 8. `utils.h / utils.cpp` - 工具函数

```cpp
std::string format_file_size(int64_t bytes);
bool is_video_file(const std::string& path);
std::string format_duration(double seconds);
```

### 9. `CMakeLists.txt` - 构建配置

```cmake
cmake_minimum_required(VERSION 3.14)
project(tg_uploader CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(Td REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)

# nlohmann/json (自动下载)
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(tg_uploader
    src/main.cpp
    src/config.cpp
    src/td_client.cpp
    src/video_info.cpp
    src/caption.cpp
    src/folder_lock.cpp
    src/uploader.cpp
    src/utils.cpp
)

target_link_libraries(tg_uploader PRIVATE
    Td::TdStatic
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    nlohmann_json::nlohmann_json
)

target_include_directories(tg_uploader PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)
```

---

## TDLib Session 存储

TDLib 使用 **database directory** 而不是 .session 文件。每个账号有独立目录:

```
tg_uploader/
├── tdlib_ajx/       # ajx 的 TDLib 数据库
├── tdlib_aoo/       # aoo 的 TDLib 数据库
├── tdlib_pussy/
├── tdlib_jane/
└── tdlib_wang/
```

首次 `login` 时创建，后续直接复用。

## 与 Python 版的配置复用

| 配置文件 | 复用方式 |
|----------|----------|
| `.account_configs/accounts.json` | 直接读取，同一个文件 |
| `.account_configs/{phone}.json` | 直接读取，同一个文件 |
| `.env` | 直接读取，解析 key=value |
| `session_*.session` | **不复用**（TDLib 用自己的数据库，需要每个账号重新 login 一次） |

## 用法（与 Python 版一致）

```bash
# 编译
mkdir build && cd build && cmake .. && make

# 列出所有账号
./tg_uploader accounts

# 首次登录（每个账号需要做一次）
./tg_uploader login --account wang

# 上传（使用 .env 中的默认目录）
./tg_uploader upload --account wang

# 指定目录上传
./tg_uploader upload --account aoo --dir /mnt/storage/ctbrec/media

# 5 个账号同时后台上传
./tg_uploader upload --account ajx &
./tg_uploader upload --account aoo &
./tg_uploader upload --account pussy &
./tg_uploader upload --account jane &
./tg_uploader upload --account wang &
```

## 代码量估算

| 文件 | 预估行数 |
|------|----------|
| main.cpp | ~80 |
| config.cpp | ~100 |
| td_client.cpp | ~250（最复杂: auth 状态机 + send_sync） |
| video_info.cpp | ~60 |
| caption.cpp | ~80 |
| folder_lock.cpp | ~80 |
| uploader.cpp | ~150 |
| utils.cpp | ~30 |
| 头文件合计 | ~100 |
| CMakeLists.txt | ~30 |
| **总计** | **~960 行** |

## 编译前需要安装的依赖

```bash
# Debian/Ubuntu
apt install -y cmake g++ libssl-dev zlib1g-dev \
    libavformat-dev libavcodec-dev libavutil-dev

# TDLib 编译安装（用户自行完成）
# 参考: https://github.com/tdlib/td#building

# nlohmann/json 由 CMake FetchContent 自动下载
```

## 验证步骤

1. `./tg_uploader accounts` → 验证配置文件加载正确
2. `./tg_uploader login --account wang` → 验证 TDLib 认证流程
3. 用一个小视频文件测试 `upload` → 验证视频上传、caption、缩略图
4. 同时运行两个账号 → 验证文件夹锁互不干扰
5. 检查 Telegram 频道中的视频: caption 格式、hashtag、封面图是否正确
