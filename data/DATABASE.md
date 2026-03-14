# Scanner Database 文档

数据库路径: `/root/pythonTguploader/data/scanner.db`

引擎: SQLite 3 (WAL 模式)

---

## 表结构

### 1. `videos` — 主表

存储从 Telegram 频道扫描到的所有视频记录。

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `message_id` | INTEGER | — | **主键**。TDLib 内部消息 ID，用于唯一标识消息 |
| `url_id` | INTEGER NOT NULL | — | Telegram URL 中的消息编号，计算方式: `message_id >> 20` |
| `file_name` | TEXT NOT NULL | — | 视频文件名，如 `mollyflwers_Chaturbate_20260219-004133.mp4` |
| `model_name` | TEXT NOT NULL | — | 从文件名解析的模特名，如 `mollyflwers` |
| `platform` | TEXT | `''` | 平台名称: `Chaturbate` / `StripChat` / `OnlyFans` / `ManyVids` / `Cam4` / `Streamate` / `LiveJasmin`，未识别则为空 |
| `record_date` | TEXT | `''` | 录制日期，格式 `YYYY-MM-DD`，从文件名解析 |
| `record_time` | TEXT | `''` | 录制时间，格式 `HH:MM:SS`，从文件名解析 |
| `duration` | INTEGER | `0` | 视频时长（秒） |
| `width` | INTEGER | `0` | 视频宽度（像素） |
| `height` | INTEGER | `0` | 视频高度（像素） |
| `file_size` | INTEGER | `0` | 文件大小（字节） |
| `thumb_path` | TEXT | `''` | 缩略图相对路径，如 `thumbs/168967.jpg`，空表示无缩略图 |
| `caption_text` | TEXT | `''` | Telegram 消息的 caption 原文 |
| `uploaded_at` | INTEGER | `0` | 消息发送时间（Unix 时间戳） |

#### 索引

| 索引名 | 字段 | 说明 |
|--------|------|------|
| `idx_model` | `model_name COLLATE NOCASE` | 模特名不区分大小写索引，加速按名搜索 |
| `idx_date` | `record_date` | 日期索引，加速日期范围查询 |
| `idx_platform` | `platform` | 平台索引，加速平台筛选 |

#### 触发器

| 触发器 | 事件 | 说明 |
|--------|------|------|
| `videos_ai` | `AFTER INSERT ON videos` | 插入视频时自动同步到 FTS5 全文搜索表 |

---

### 2. `scan_checkpoint` — 扫描进度

单行表，记录频道扫描的断点信息，用于 `--resume` 续扫。

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `id` | INTEGER | — | **主键**，恒为 `1`（`CHECK(id = 1)`） |
| `oldest_message_id` | INTEGER | `0` | 最后扫描到的最旧消息 ID，续扫从此处继续 |
| `total_scanned` | INTEGER | `0` | 累计已扫描的消息数量 |
| `scan_complete` | INTEGER | `0` | 是否已完成全量扫描: `0`=未完成, `1`=已完成 |

---

### 3. `videos_fts` — 全文搜索 (FTS5)

SQLite FTS5 虚拟表，用于网页端模特名前缀搜索。

| 字段 | 来源 |
|------|------|
| `model_name` | 来自 `videos.model_name` |
| `file_name` | 来自 `videos.file_name` |
| `rowid` | 对应 `videos.message_id` |

配置: `content=videos, content_rowid=message_id`（内容存储在主表，FTS 只维护索引）

#### FTS5 自动维护的内部表（勿直接操作）

| 表名 | 说明 |
|------|------|
| `videos_fts_config` | FTS5 配置 |
| `videos_fts_data` | FTS5 倒排索引数据 |
| `videos_fts_docsize` | 文档大小信息 |
| `videos_fts_idx` | 分段索引 |

---

## 文件名解析规则

文件名格式: `{model}_{platform}_{YYYYMMDD}-{HHMMSS}.mp4`

示例:
```
mollyflwers_Chaturbate_20260219-004133.mp4
→ model_name: mollyflwers
→ platform:   Chaturbate
→ record_date: 2026-02-19
→ record_time: 00:41:33
```

支持的平台关键词: `Chaturbate`, `StripChat`, `OnlyFans`, `ManyVids`, `Cam4`, `Streamate`, `LiveJasmin`

---

## 缩略图

存储路径: `/root/pythonTguploader/data/thumbs/{url_id}.jpg`

优先级:
1. 视频消息之后上传的同名 JPG 照片（高清，通常 1920x1080）
2. 视频自带的封面缩略图（低清，通常 320x180）

判断方式: 文件 >= 30KB 为高清照片，< 30KB 为视频封面

---

## Telegram 链接

频道为私有频道，链接格式:

```
https://t.me/c/1810923743/{url_id}
```

其中 `url_id = message_id >> 20`

---

## 数据写入来源

| 来源 | 说明 |
|------|------|
| `ctg scan` | 扫描频道历史消息，批量写入 |
| `ctg upload` | 上传完成后自动写入单条记录 |
| `ctg fix-thumbs` | 只更新 `thumb_path` 字段 |

---

## 常用查询

```sql
-- 总记录数
SELECT COUNT(*) FROM videos;

-- 按模特统计 top 20
SELECT model_name, COUNT(*) as count
FROM videos GROUP BY model_name ORDER BY count DESC LIMIT 20;

-- 按平台统计
SELECT platform, COUNT(*) as count
FROM videos GROUP BY platform ORDER BY count DESC;

-- 搜索某个模特的所有视频
SELECT file_name, record_date, record_time, duration, file_size
FROM videos WHERE model_name = 'mollyflwers' ORDER BY record_date DESC;

-- 模糊搜索模特名
SELECT DISTINCT model_name FROM videos WHERE model_name LIKE 'molly%';

-- FTS5 前缀搜索（网页端使用的方式）
SELECT * FROM videos WHERE message_id IN
  (SELECT rowid FROM videos_fts WHERE videos_fts MATCH 'molly*');

-- 缩略图缺失统计
SELECT COUNT(*) as total,
       SUM(CASE WHEN thumb_path = '' THEN 1 ELSE 0 END) as missing
FROM videos;

-- 日期范围查询
SELECT * FROM videos
WHERE record_date >= '2026-02-01' AND record_date <= '2026-02-28'
ORDER BY record_date DESC;

-- 查看扫描进度
SELECT * FROM scan_checkpoint;
```
