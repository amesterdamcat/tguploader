# tg_uploader 使用指南

## 目录

- [概述](#概述)
- [目录结构](#目录结构)
- [账号配置](#账号配置)
- [全局设置](#全局设置)
- [命令参考](#命令参考)
  - [accounts — 列出账号](#accounts--列出账号)
  - [login — 登录账号](#login--登录账号)
  - [upload — TDLib 上传](#upload--tdlib-上传)
  - [bot-upload — Bot API 上传](#bot-upload--bot-api-上传)
  - [fix-big — 拆分超大文件](#fix-big--拆分超大文件)
  - [scan — 扫描频道](#scan--扫描频道)
  - [fix-thumbs — 修复缩略图](#fix-thumbs--修复缩略图)
- [Bot API 本地服务器配置](#bot-api-本地服务器配置)
- [文件命名规范](#文件命名规范)
- [上传后文件处理](#上传后文件处理)
- [常见问题](#常见问题)

---

## 概述

`tg_uploader` 支持两种上传模式：

| 模式 | 命令 | 适用场景 |
|------|------|----------|
| **TDLib 上传** | `upload` | 用户账号上传，支持并发预上传，文件上限 ~2GB（免费）/ ~4GB（Premium） |
| **Bot API 上传** | `bot-upload` | 多 Bot 轮换上传，自动切换规避 FLOOD_WAIT，解决长时间封号问题 |

---

## 目录结构

```
tg_uploader/          ← 二进制所在目录（也是工作目录）
├── tg_uploader       ← 可执行文件
├── .env              ← 全局设置
├── .account_configs/
│   ├── accounts.json ← 账号映射表（TDLib 用）
│   ├── 861xxxxxxxx.json  ← 各账号详细配置
│   └── bots.json     ← Bot API 配置（bot-upload 用）
├── tdlib_wang/       ← TDLib 会话数据库（login 后自动创建）
└── data/
    ├── scanner.db    ← 扫描记录数据库
    └── thumbs/       ← 缩略图缓存
```

> **提示：** 程序启动时会自动在二进制所在目录寻找 `.account_configs/`；找不到则使用当前工作目录。

---

## 账号配置

### `accounts.json` — 账号映射表

```json
{
  "wang": "861xxxxxxxxx",
  "li":   "861yyyyyyyyy"
}
```

键为账号别名（命令行 `--account` 使用），值为手机号数字部分。

### `<手机号>.json` — 账号详情

```json
{
  "phone":      "+861xxxxxxxxx",
  "api_id":     123456,
  "api_hash":   "abcdef1234567890abcdef1234567890",
  "channel_id": "@my_channel"
}
```

- `api_id` / `api_hash`：从 [my.telegram.org](https://my.telegram.org) 获取
- `channel_id`：支持 `@username`、`-100xxxxxxxxxx` 数字 ID、`me`（收藏夹）

---

## 全局设置

`.env` 文件（支持行注释 `#`，值可加引号）：

```env
# 上传后是否删除源文件（默认 false）
DELETE_AFTER_UPLOAD=false

# 上传后是否给文件加后缀标记（默认 true）
MARK_UPLOADED_FILES=true
UPLOADED_SUFFIX=.uploaded

# 默认上传目录（不传 --dir 时使用）
DEFAULT_UPLOAD_DIR=/data/recordings

# 免于删除的目录（逗号分隔，DELETE_AFTER_UPLOAD=true 时有效）
EXEMPT_FOLDERS=/data/recordings/keep,/data/archive

# Bot API 服务器地址（bot-upload 也可在 bots.json 中指定）
BOT_API_URL=http://127.0.0.1:8081
```

---

## 命令参考

### `accounts` — 列出账号

```bash
./tg_uploader accounts
```

输出所有已配置账号及其 TDLib 数据库状态：

```
Name       Phone              Channel              TDLib DB
--------------------------------------------------------------------
wang       +861xxxxxxxxx      @my_channel          OK
li         +861yyyyyyyyy      @another_channel     NEEDS LOGIN
```

---

### `login` — 登录账号

首次使用或 session 过期时执行：

```bash
./tg_uploader login --account wang
```

交互式输入手机验证码，完成后 TDLib 会话保存到 `tdlib_wang/`，后续无需重复登录。

---

### `upload` — TDLib 上传

通过用户账号上传视频（MTProto 协议）。

```bash
# 上传指定目录（默认递归）
./tg_uploader upload --account wang --dir /data/recordings

# 不递归（只扫描一级目录）
./tg_uploader upload --account wang --dir /data/recordings --no-recursive

# 并发预上传 4 个文件（加速批量上传）
./tg_uploader upload --account wang --dir /data/recordings --concurrent 4
```

**参数说明：**

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--account NAME` / `-a NAME` | 账号别名（必填） | — |
| `--dir PATH` / `-d PATH` | 上传目录 | `.env` 中 `DEFAULT_UPLOAD_DIR` |
| `--concurrent N` / `-c N` | 并发预上传文件数 | `1` |
| `--recursive` / `-r` | 递归子目录 | 开启 |
| `--no-recursive` | 只扫描一级目录 | — |

**文件大小限制：**
- 免费账号：~1.95 GB（4000 块 × 512 KB）
- Premium 账号：~3.91 GB（8000 块 × 512 KB）
- 超过限制的文件自动重命名为 `.big`，需用 `fix-big` 处理

---

### `bot-upload` — Bot API 上传

通过自建 Bot API 本地服务器上传视频，支持多 Bot 轮换以规避 FLOOD_WAIT。

```bash
# 上传指定目录
./tg_uploader bot-upload --dir /data/recordings

# 使用 .env 中的 DEFAULT_UPLOAD_DIR
./tg_uploader bot-upload

# 不递归
./tg_uploader bot-upload --dir /data/recordings --no-recursive
```

**参数说明：**

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--dir PATH` / `-d PATH` | 上传目录 | `.env` 中 `DEFAULT_UPLOAD_DIR` |
| `--recursive` / `-r` | 递归子目录 | 开启 |
| `--no-recursive` | 只扫描一级目录 | — |

**前置条件：** 需要先启动 Bot API 本地服务器，详见 [Bot API 本地服务器配置](#bot-api-本地服务器配置)。

**Bot 轮换逻辑：**
1. 某个 Bot 收到 429 FLOOD_WAIT（例如等待 37 秒）→ 立即切换到下一个 Bot
2. 如果所有 Bot 都在冷却 → 等待冷却时间最短的 Bot 恢复
3. 相比 TDLib 账号封号（1-2 天），Bot FLOOD_WAIT 通常只需 30-60 秒

---

### `fix-big` — 拆分超大文件

将超过 TDLib 大小限制的 `.big` 文件拆分为两段：

```bash
./tg_uploader fix-big --dir /data/recordings
./tg_uploader fix-big --dir /data/recordings --no-recursive
```

**处理流程：**
1. 扫描目录下所有 `*.mp4.big`（或 `*.mkv.big` 等）文件
2. 计算安全时长，用 ffmpeg 剪切 Part1（≤ 限制大小）
3. 提取剩余部分为 `原名_part2.mp4`
4. 为 Part2 生成接触表缩略图（10×9 格，3840px 宽）
5. 用 Part1 替换原文件，删除 `.big`

**依赖：** 系统安装 `ffmpeg`。

---

### `scan` — 扫描频道

扫描 Telegram 频道中已有消息，写入 `data/scanner.db`：

```bash
# 增量扫描（只扫描新消息）
./tg_uploader scan --account wang

# 全量扫描（重新扫描全部历史）
./tg_uploader scan --account wang --full

# 断点续扫
./tg_uploader scan --account wang --resume
```

---

### `fix-thumbs` — 修复缩略图

为频道中缺少缩略图的视频消息补发缩略图：

```bash
./tg_uploader fix-thumbs --account wang
./tg_uploader fix-thumbs --account wang --force   # 强制重发所有缩略图
./tg_uploader fix-thumbs --account wang --range 50  # 处理最近 50 条消息
```

---

## Bot API 本地服务器配置

### 1. 安装 `telegram-bot-api`

```bash
# 从源码编译（推荐）
git clone --recursive https://github.com/tdlib/telegram-bot-api.git
cd telegram-bot-api
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

或使用发行版预编译包（如有）。

### 2. 在 BotFather 创建 Bot

在 Telegram 中与 [@BotFather](https://t.me/BotFather) 对话：

```
/newbot
→ 输入 Bot 名称（显示名）
→ 输入 Bot 用户名（以 _bot 结尾）
→ 获得 Token: 1234567890:ABCDEFxxxxxxxxxxxxxxxxxxxxxxxx
```

**建议创建 2-3 个 Bot** 用于轮换，当一个触发 FLOOD_WAIT 时无缝切换。

将每个 Bot 加入目标频道并设置为管理员（需要"发送消息"权限）。

### 3. 启动本地服务器

```bash
./telegram-bot-api \
  --api-id=YOUR_API_ID \
  --api-hash=YOUR_API_HASH \
  --local \
  --http-port=8081 \
  --dir=/tmp/bot-api-data \
  --log=/var/log/bot-api.log
```

| 参数 | 说明 |
|------|------|
| `--local` | **必须**，启用本地文件路径模式（绕过 50 MB HTTP 限制） |
| `--api-id` / `--api-hash` | 同 TDLib，从 my.telegram.org 获取 |
| `--http-port` | 监听端口（默认 8081） |
| `--dir` | 服务器工作目录 |

> **注意：** `--local` 模式下，文件路径直接传给服务器，服务器从磁盘读取。单文件最大支持 **2 GB**。

### 4. 配置 `bots.json`

```json
{
  "api_url": "http://127.0.0.1:8081",
  "channel_id": "@your_channel",
  "bots": {
    "bot1": "1234567890:AABBCCDDxxxxxxxxxxxxxxxxxxxxxx",
    "bot2": "9876543210:ZZYYXXWWyyyyyyyyyyyyyyyyyyyyyy",
    "bot3": "1122334455:QQWWEERRzzzzzzzzzzzzzzzzzzzzzz"
  }
}
```

保存至 `.account_configs/bots.json`（与 `accounts.json` 同目录）。

### 5. 验证

```bash
# 测试服务器是否正常（替换 TOKEN 和 CHAT_ID）
curl "http://127.0.0.1:8081/botTOKEN/sendMessage" \
  -F "chat_id=@your_channel" \
  -F "text=test"
```

返回 `{"ok":true,...}` 表示正常。

---

## 文件命名规范

缩略图自动匹配：与视频同目录、同名的 `.jpg` 文件。

```
/data/recordings/
├── modelname_Chaturbate_20241201_120000.mp4   ← 视频
└── modelname_Chaturbate_20241201_120000.jpg   ← 自动作为缩略图
```

Caption 自动提取 Hashtag，识别以下平台关键词（不区分大小写）：
`Chaturbate` / `StripChat` / `OnlyFans` / `ManyVids` / `Cam4` / `Streamate` / `LiveJasmin`

命名格式 `模型名_平台_YYYYMMDD_HHMMSS.mp4` 会生成：
```
#模型名 #date20241201
```

---

## 上传后文件处理

由 `.env` 控制，优先级：**删除 > 标记**

| 配置 | 行为 |
|------|------|
| `DELETE_AFTER_UPLOAD=true` | 上传成功后删除文件 |
| `DELETE_AFTER_UPLOAD=true` + `EXEMPT_FOLDERS` | 豁免目录内的文件改为标记 `.uploaded`，不删除 |
| `MARK_UPLOADED_FILES=true`（默认） | 上传成功后重命名为 `原文件名.uploaded` |
| 两者均为 false | 不做任何处理，文件保留原样 |

已标记 `.uploaded` 或存在同名 `.big` 的文件会被跳过，不重复上传。

---

## 常见问题

**Q: 上传时触发 FLOOD_WAIT 怎么办？**

- `upload` 模式：程序自动等待并重试，这是 Telegram 的速率限制，属于正常现象。若封号 1-2 天，建议改用 `bot-upload`。
- `bot-upload` 模式：自动轮换到下一个 Bot，冷却时间通常 30-60 秒，基本无感知。

**Q: 文件大于 2 GB 怎么上传？**

- TDLib 免费账号限制 ~1.95 GB，超出的文件自动标记为 `.big`
- 运行 `fix-big` 将其拆分为两段后再上传
- 或使用 `bot-upload` + 本地服务器（`--local` 模式支持 2 GB）

**Q: `bot-upload` 提示 "Cannot open bots.json"？**

检查 `.account_configs/bots.json` 是否存在，内容格式是否正确（参见上方配置示例）。

**Q: `bot-upload` 提示连接失败？**

确认 `telegram-bot-api` 服务已启动且 `--local` 参数已加上：
```bash
curl http://127.0.0.1:8081/
# 应返回 {"ok":false,"error_code":404,...}
```

**Q: 视频上传成功但没有缩略图？**

检查是否有与视频同名的 `.jpg` 文件放在同一目录下。

**Q: 扫描/修复缩略图时提示 "channel not found"？**

确认 `channel_id` 格式正确，且该账号已加入对应频道/群组。
