# 豁免文件夹功能使用指南

## 功能说明

当启用 `DELETE_AFTER_UPLOAD=true` 时，上传完成的文件会被自动删除。但有些重要文件夹的内容你可能希望手动检查后再删除。

**豁免文件夹功能**允许你指定某些文件夹，这些文件夹中的文件即使上传成功也不会被自动删除。

## 配置方法

在 `.env` 文件中配置：

```bash
# 启用上传后自动删除
DELETE_AFTER_UPLOAD=true

# 配置豁免文件夹（多个文件夹用逗号分隔）
EXEMPT_FOLDERS=important,/path/to/archive,VIP_content
```

## 配置示例

### 示例 1: 单个文件夹
```bash
EXEMPT_FOLDERS=important
```
- 会保护 `./important/` 下的所有文件

### 示例 2: 多个文件夹
```bash
EXEMPT_FOLDERS=important,archive,backup
```
- 会保护 `./important/`、`./archive/`、`./backup/` 下的所有文件

### 示例 3: 绝对路径和相对路径混合
```bash
EXEMPT_FOLDERS=/home/user/videos/VIP,important,./archive
```
- 支持绝对路径和相对路径混合使用

### 示例 4: 禁用豁免（所有文件都会被删除）
```bash
EXEMPT_FOLDERS=
```
- 留空表示不豁免任何文件夹

## 工作原理

1. **上传成功后**：程序检查文件是否在豁免文件夹中
2. **如果在豁免文件夹中**：跳过删除，文件保留
3. **如果不在豁免文件夹中**：删除文件
4. **如果DELETE_AFTER_UPLOAD=false**：使用 `.uploaded` 后缀标记（不删除）

## 日志输出

启用豁免文件夹后，你会在日志中看到：

```
✅ 上传成功: video.mp4
🛡️ 文件在豁免文件夹中，不会删除: ./important/video.mp4
   豁免文件夹: important
```

或者：

```
✅ 上传成功: video.mp4
🗑️ 已删除源文件: ./regular/video.mp4
```

## 使用场景

### 场景 1: 批量上传，保护重要内容
```bash
/videos/
  ├── important/      # 重要内容，手动检查后删除
  ├── regular/        # 普通内容，上传后自动删除
  └── backup/         # 备份内容，保留不删除
```

配置：
```bash
DELETE_AFTER_UPLOAD=true
EXEMPT_FOLDERS=important,backup
```

结果：
- `important/` 和 `backup/` 中的文件保留
- `regular/` 中的文件上传后自动删除

### 场景 2: 测试阶段
```bash
DELETE_AFTER_UPLOAD=true
EXEMPT_FOLDERS=test,staging
```
- 测试文件夹的内容不会被删除，方便反复测试

## 安全性保证

1. **只在上传成功后才删除**：只有当文件完全上传到 Telegram 并返回消息 ID 后才会执行删除
2. **异常时不删除**：如果检查豁免文件夹时出错，默认不删除文件，保证文件安全
3. **明确的日志输出**：每次删除或跳过删除都会有明确的日志记录

## 与 MARK_UPLOADED_FILES 的关系

| DELETE_AFTER_UPLOAD | 文件在豁免文件夹 | 行为 |
|-------------------|--------------|------|
| true | 是 | 不删除，不标记 |
| true | 否 | 删除文件 |
| false | - | 添加 `.uploaded` 后缀 |

## 注意事项

1. **路径匹配**：程序会将文件和豁免文件夹都转换为绝对路径进行匹配
2. **子文件夹自动包含**：如果设置 `EXEMPT_FOLDERS=important`，那么 `important/subfolder/` 下的文件也会被保护
3. **大小写敏感**：Linux 系统下路径是大小写敏感的
4. **逗号分隔**：多个文件夹必须用英文逗号分隔，不要有多余空格

## 相关工具

配合 `clean.sh` 脚本使用：
```bash
# 清理所有 .uploaded 标记文件
./clean.sh /path/to/directory

# 强制清理，不提示确认
./clean.sh /path/to/directory --force

# 详细输出模式
./clean.sh /path/to/directory -v
```

这样你可以：
1. 设置 `DELETE_AFTER_UPLOAD=true` 自动删除普通文件
2. 豁免重要文件夹，手动检查后再删除
3. 使用 `clean.sh` 清理 `.uploaded` 标记文件
