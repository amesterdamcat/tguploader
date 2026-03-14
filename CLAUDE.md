# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Python Telegram video uploader designed specifically for live stream recording uploads. It provides enhanced parallel uploading, smart rate limiting, and automated file management features.

## Key Architecture

### Core Components

- **EnhancedParallelUploader** (`enhanced_uploader.py:30`) - Main uploader class extending TelegramClient with parallel upload capabilities
- **TelegramConfig** (`enhanced_uploader.py:1184`) - Configuration management and file size limit handling

### Upload Architecture

The system uses a sophisticated batch upload approach:
- Files are split into chunks (max 512KB per chunk)
- Chunks are processed in configurable batches (default 100 chunks per batch)
- Within each batch, multiple chunks uploaded concurrently (configurable 1-8 tasks)
- User-selectable delay between tasks to prevent API flooding
- Automatic retry logic with exponential backoff for different error types

### Batch Upload System

Three-tier configuration system:
- **Batch Size**: How many chunks to process per batch (50/100/200/500)
- **Concurrent Tasks**: How many tasks run simultaneously within each batch (1/2/4/8)
- **Task Delay**: Delay between each task (0-10 seconds, user configurable)

This dramatically improves performance for large files by reducing the number of batches.

## Environment Configuration

Required `.env` file with:
```
TELEGRAM_API_ID=your_api_id
TELEGRAM_API_HASH=your_api_hash
TELEGRAM_PHONE=+your_phone
TELEGRAM_CHANNEL_ID=channel_name_or_id
TELEGRAM_SESSION_NAME=session_name
```

Optional configuration:
- `DELETE_AFTER_UPLOAD=false` - Delete source files after upload
- `MARK_UPLOADED_FILES=true` - Add suffix to uploaded files
- `UPLOADED_SUFFIX=.uploaded` - Suffix for uploaded files
- `PARALLEL_UPLOAD_BLOCKS=8` - Number of concurrent upload chunks

## Common Development Commands

Since this is a Python project without standard build tools:

```bash
# Install dependencies
pip install -r requirements.txt

# Run the main uploader
python enhanced_uploader.py
```

## File Size Limits

The system enforces Telegram's file size limits:
- Free accounts: 2GB (4000 chunks × 512KB)
- Premium accounts: 4GB (8000 chunks × 512KB)
- Automatic detection of user premium status

## Video Processing Features

- Automatic thumbnail detection (looks for same-name .jpg files)
- Video metadata extraction using ffprobe when available
- Caption generation with hashtags extracted from filenames
- Support for various video formats (.mp4, .avi, .mov, .mkv, etc.)

## Error Handling

Comprehensive error handling based on Telegram API documentation:
- 420 FLOOD_WAIT errors with automatic wait and retry
- 400 BAD_REQUEST errors with specific handling for file upload issues
- 500 INTERNAL errors with automatic retry
- Network and connection error recovery
- Simple fixed delay between requests to prevent rate limiting

## Testing Approach

No formal test framework is configured. Test by:
1. Running single file uploads to verify functionality
2. Testing with different file sizes and formats
3. Testing different parallel settings (1, 2, 4, 8 blocks)
4. Verifying premium vs free account limit detection

## Development Principles

### Chinese Version (原版)
- 以瞎猜接口为耻，以认真查询为荣
- 以模糊执行为耻，以寻求确认为荣
- 以臆想业务为耻，以人类确认为荣
- 以创造接口为耻，以复用现有为荣
- 以跳过验证为耻，以主动测试为荣
- 以破坏架构为耻，以遵循规范为荣
- 以假装理解为耻，以诚实无知为荣
- 以盲目修改为耻，以谨慎重构为荣

### English Translation
- Be ashamed of blindly guessing APIs, take pride in thorough investigation
- Be ashamed of vague execution, take pride in seeking confirmation
- Be ashamed of imagining business logic, take pride in human confirmation
- Be ashamed of creating new interfaces, take pride in reusing existing ones
- Be ashamed of skipping validation, take pride in proactive testing
- Be ashamed of breaking architecture, take pride in following standards
- Be ashamed of pretending to understand, take pride in honest ignorance
- Be ashamed of blind modifications, take pride in careful refactoring