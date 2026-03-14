"""
Telegram Video Uploader - parallel upload for live stream recordings.

Usage:
    python enhanced_uploader.py upload --account wang [--dir PATH] [--recursive] [--batch-size 100] [--concurrent 1] [--delay 1.0]
    python enhanced_uploader.py login --account wang
    python enhanced_uploader.py accounts
"""

import argparse
import asyncio
import hashlib
import os
import sys
import time
import logging
import re
import random
import json
import psutil
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime
from typing import Optional, Callable, List, Dict, Any
from collections import defaultdict

from telethon import TelegramClient, utils, helpers
from telethon.errors import RPCError, FloodWaitError, InvalidBufferError
from telethon.tl import types, functions
from telethon.crypto import AES
from telethon.tl.custom import InputSizedFile

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("EnhancedUploader")

ACCOUNT_CONFIGS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.account_configs')


# ============================================================
# Configuration Classes (no shared mutable state)
# ============================================================

@dataclass(frozen=True)
class AccountConfig:
    """Immutable per-account configuration loaded at startup."""
    name: str
    phone: str
    api_id: int
    api_hash: str
    channel_id: str
    session_path: str  # without .session extension

    @classmethod
    def load(cls, account_name: str) -> 'AccountConfig':
        """Load account config by friendly name from .account_configs/"""
        mapping_file = os.path.join(ACCOUNT_CONFIGS_DIR, 'accounts.json')
        if not os.path.exists(mapping_file):
            print(f"Error: {mapping_file} not found. Create it with name->phone mapping.")
            sys.exit(1)

        with open(mapping_file, 'r', encoding='utf-8') as f:
            mapping = json.load(f)

        phone_digits = mapping.get(account_name)
        if not phone_digits:
            available = ', '.join(mapping.keys())
            print(f"Error: account '{account_name}' not found. Available: {available}")
            sys.exit(1)

        config_file = os.path.join(ACCOUNT_CONFIGS_DIR, f'{phone_digits}.json')
        if not os.path.exists(config_file):
            print(f"Error: config file {config_file} not found for account '{account_name}'")
            sys.exit(1)

        with open(config_file, 'r', encoding='utf-8') as f:
            data = json.load(f)

        return cls(
            name=account_name,
            phone=data['phone'],
            api_id=int(data['api_id']),
            api_hash=data['api_hash'],
            channel_id=data.get('channel_id', 'me'),
            session_path=f"session_{phone_digits}",
        )

    @classmethod
    def list_all(cls) -> list:
        """List all configured accounts."""
        mapping_file = os.path.join(ACCOUNT_CONFIGS_DIR, 'accounts.json')
        if not os.path.exists(mapping_file):
            return []

        with open(mapping_file, 'r', encoding='utf-8') as f:
            mapping = json.load(f)

        accounts = []
        for name, phone_digits in mapping.items():
            config_file = os.path.join(ACCOUNT_CONFIGS_DIR, f'{phone_digits}.json')
            if os.path.exists(config_file):
                with open(config_file, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                accounts.append(cls(
                    name=name,
                    phone=data['phone'],
                    api_id=int(data['api_id']),
                    api_hash=data['api_hash'],
                    channel_id=data.get('channel_id', 'me'),
                    session_path=f"session_{phone_digits}",
                ))
            else:
                logger.warning(f"Config file missing for {name}: {config_file}")
        return accounts


@dataclass
class SharedSettings:
    """Shared operational settings loaded from .env (never written to by this script)."""
    delete_after_upload: bool = False
    mark_uploaded_files: bool = True
    uploaded_suffix: str = ".uploaded"
    exempt_folders: list = field(default_factory=list)
    default_upload_dir: str = ""

    @classmethod
    def load(cls) -> 'SharedSettings':
        env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.env')
        if os.path.exists(env_path):
            from dotenv import load_dotenv
            load_dotenv(env_path, override=True)

        exempt_str = os.environ.get('EXEMPT_FOLDERS', '').strip()
        exempt = [f.strip() for f in exempt_str.split(',') if f.strip()] if exempt_str else []

        return cls(
            delete_after_upload=os.environ.get('DELETE_AFTER_UPLOAD', 'false').lower() == 'true',
            mark_uploaded_files=os.environ.get('MARK_UPLOADED_FILES', 'true').lower() == 'true',
            uploaded_suffix=os.environ.get('UPLOADED_SUFFIX', '.uploaded'),
            exempt_folders=exempt,
            default_upload_dir=os.environ.get('DEFAULT_UPLOAD_DIR', ''),
        )


# ============================================================
# Progress Bar
# ============================================================

class ProgressBar:
    def __init__(self, total_size: int, description: str = "uploading"):
        self.total_size = total_size
        self.description = description
        self.start_time = time.time()
        self.last_update = 0
        self.last_bytes = 0
        self.last_line_length = 0

    async def __call__(self, current: int, total: int):
        now = time.time()
        if now - self.last_update < 1.0 and current < total:
            return

        time_diff = now - self.last_update if self.last_update > 0 else 1.0
        bytes_diff = current - self.last_bytes
        self.last_update = now
        self.last_bytes = current

        percentage = (current / total) * 100 if total > 0 else 0
        speed = bytes_diff / time_diff if time_diff > 0 else 0
        speed_mb = speed / 1024 / 1024

        bar_length = 30
        filled_length = int(bar_length * current // total) if total > 0 else 0
        bar = '█' * filled_length + '░' * (bar_length - filled_length)

        progress_text = f'{self.description}: |{bar}| {percentage:.1f}% {speed_mb:.2f} MB/s'

        if self.last_line_length > 0:
            print('\r' + ' ' * self.last_line_length, end='', flush=True)

        print(f'\r{progress_text}', end='', flush=True)
        self.last_line_length = len(progress_text)

        if current >= total:
            print()
            self.last_line_length = 0


# ============================================================
# File Size Constants (from Telegram API docs)
# ============================================================

UPLOAD_MAX_FILEPARTS_DEFAULT = 4000   # free accounts
UPLOAD_MAX_FILEPARTS_PREMIUM = 8000   # premium accounts
PART_SIZE_RECOMMENDED = 512 * 1024    # 512KB
MAX_FILE_SIZE_FREE = UPLOAD_MAX_FILEPARTS_DEFAULT * PART_SIZE_RECOMMENDED     # ~2GB
MAX_FILE_SIZE_PREMIUM = UPLOAD_MAX_FILEPARTS_PREMIUM * PART_SIZE_RECOMMENDED  # ~4GB


def format_file_size(size_bytes: int) -> str:
    if size_bytes >= 1024 * 1024 * 1024:
        return f"{size_bytes / (1024 * 1024 * 1024):.2f} GB"
    elif size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.2f} MB"
    else:
        return f"{size_bytes / 1024:.2f} KB"


# ============================================================
# Enhanced Parallel Uploader
# ============================================================

class EnhancedParallelUploader(TelegramClient):
    """Parallel uploader extending TelegramClient."""

    def __init__(self, account: AccountConfig, settings: SharedSettings,
                 batch_size: int = 100, concurrent_tasks: int = 1,
                 custom_delay: float = None, **kwargs):
        super().__init__(account.session_path, account.api_id, account.api_hash, **kwargs)
        self.account = account
        self.settings = settings
        self.batch_size = batch_size
        self.concurrent_tasks = concurrent_tasks
        self.upload_semaphore = asyncio.Semaphore(self.concurrent_tasks)
        self.channel_entity = None

        if custom_delay is not None:
            self.rate_limit_delay = custom_delay
        else:
            self.rate_limit_delay = self._calculate_upload_delay(concurrent_tasks)

        logger.info(f"Init uploader [{account.name}]: batch={batch_size}, concurrent={concurrent_tasks}, delay={self.rate_limit_delay:.2f}s")

    def _calculate_upload_delay(self, parallel_blocks: int) -> float:
        if parallel_blocks >= 32:
            return 2.5
        elif parallel_blocks >= 16:
            return 2.0
        elif parallel_blocks >= 8:
            return 1.5
        elif parallel_blocks >= 4:
            return 1.0
        elif parallel_blocks >= 2:
            return 0.5
        else:
            return 0.3

    # ---- Folder Lock System ----

    def _create_folder_lock(self, folder_path: str) -> bool:
        try:
            lock_file = os.path.join(folder_path, '.uploading.lock')
            lock_info = {
                'pid': os.getpid(),
                'account': self.account.name,
                'phone': self.account.phone,
                'start_time': datetime.now().isoformat(),
                'folder_path': folder_path,
            }
            with open(lock_file, 'w', encoding='utf-8') as f:
                json.dump(lock_info, f, indent=2, ensure_ascii=False)
            logger.info(f"Locked folder: {folder_path}")
            return True
        except Exception as e:
            logger.error(f"Failed to lock folder: {e}")
            return False

    def _check_folder_lock(self, folder_path: str) -> bool:
        """Return True if folder is locked by another process."""
        try:
            lock_file = os.path.join(folder_path, '.uploading.lock')
            if not os.path.exists(lock_file):
                return False

            with open(lock_file, 'r', encoding='utf-8') as f:
                lock_info = json.load(f)

            pid = lock_info.get('pid')
            if pid:
                try:
                    if psutil.pid_exists(pid) and psutil.Process(pid).is_running():
                        account_name = lock_info.get('account', 'unknown')
                        logger.info(f"Folder locked: {folder_path} (pid:{pid}, account:{account_name})")
                        return True
                    # Process dead, clean stale lock
                    logger.info(f"Cleaning stale lock: {lock_file}")
                    os.remove(lock_file)
                    return False
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    os.remove(lock_file)
                    return False

            # Check time-based expiry (6 hours)
            start_time = lock_info.get('start_time')
            if start_time:
                lock_time = datetime.fromisoformat(start_time)
                if (datetime.now() - lock_time).total_seconds() > 6 * 3600:
                    os.remove(lock_file)
                    return False

            return True
        except Exception as e:
            logger.error(f"Check folder lock failed: {e}")
            return False

    def _remove_folder_lock(self, folder_path: str):
        try:
            lock_file = os.path.join(folder_path, '.uploading.lock')
            if os.path.exists(lock_file):
                os.remove(lock_file)
                logger.info(f"Unlocked folder: {folder_path}")
        except Exception as e:
            logger.error(f"Failed to remove folder lock: {e}")

    def _select_available_folders(self, folder_groups: Dict[str, List[str]]) -> List[str]:
        available_folders = []
        locked_folders = []

        for folder_path in folder_groups.keys():
            if self._check_folder_lock(folder_path):
                locked_folders.append(folder_path)
            else:
                available_folders.append(folder_path)

        if locked_folders:
            logger.info(f"Skipping {len(locked_folders)} locked folders")

        if not available_folders:
            logger.warning("All folders locked")
            return []

        random.shuffle(available_folders)
        logger.info(f"Available folders: {len(available_folders)}")
        return available_folders

    def _is_file_in_exempt_folder(self, file_path: str) -> bool:
        if not self.settings.exempt_folders:
            return False
        file_abs = os.path.abspath(file_path)
        for exempt_folder in self.settings.exempt_folders:
            exempt_abs = os.path.abspath(exempt_folder)
            if file_abs.startswith(exempt_abs + os.sep) or file_abs.startswith(exempt_abs):
                logger.info(f"File in exempt folder, won't delete: {file_path}")
                return True
        return False

    # ---- Channel ----

    async def find_channel(self, channel_id: str):
        try:
            logger.info(f"Finding channel: {channel_id}")
            if channel_id.startswith('@') or channel_id.startswith('-'):
                self.channel_entity = await self.get_entity(channel_id)
            else:
                self.channel_entity = None
                async for dialog in self.iter_dialogs():
                    if hasattr(dialog.entity, 'title') and dialog.entity.title == channel_id:
                        self.channel_entity = dialog.entity
                        break
                if not self.channel_entity:
                    raise Exception(f"Channel not found: {channel_id}")

            logger.info(f"Found channel: {self.channel_entity.title} (ID: {self.channel_entity.id})")
            return True
        except Exception as e:
            logger.error(f"Find channel failed: {e}")
            return False

    # ---- Core Upload Logic ----

    async def _read_file_part(self, file_path: str, part_index: int, part_size: int, file_size: int) -> bytes:
        offset = part_index * part_size
        read_size = min(part_size, file_size - offset)
        with open(file_path, 'rb') as f:
            f.seek(offset)
            return f.read(read_size)

    async def _send_file_part_with_read(self, file_path: str, file_id: int, part_index: int,
                                        part_count: int, part_size: int, file_size: int,
                                        is_big: bool, hash_md5,
                                        progress_callback: Optional[Callable] = None) -> None:
        max_retries = 3
        retry_count = 0

        try:
            while retry_count < max_retries:
                try:
                    if self.rate_limit_delay > 0:
                        await asyncio.sleep(self.rate_limit_delay)

                    start_read = time.time()
                    part = await self._read_file_part(file_path, part_index, part_size, file_size)
                    read_time = time.time() - start_read

                    if len(part) > 512 * 1024:
                        raise ValueError(f"FILE_PART_TOO_BIG: chunk {part_index} size {len(part)} > 512KB")
                    if len(part) == 0 and part_index < part_count - 1:
                        raise ValueError(f"FILE_PART_EMPTY: chunk {part_index} is empty")
                    if part_index < part_count - 1 and len(part) < 1024:
                        raise ValueError(f"FILE_PART_TOO_SMALL: chunk {part_index} size {len(part)} < 1KB")

                    if not is_big and hash_md5:
                        hash_md5.update(part)

                    if is_big:
                        request = functions.upload.SaveBigFilePartRequest(
                            file_id=file_id, file_part=part_index,
                            file_total_parts=part_count, bytes=part
                        )
                    else:
                        request = functions.upload.SaveFilePartRequest(
                            file_id=file_id, file_part=part_index, bytes=part
                        )

                    start_upload = time.time()
                    result = await self(request)
                    upload_time = time.time() - start_upload

                    if not result:
                        raise Exception(f"Upload chunk {part_index} failed: server returned False")

                    progress_text = f'Chunk {part_index + 1}/{part_count} OK (read: {read_time:.2f}s, upload: {upload_time:.2f}s)'
                    print(f'\r{progress_text}', end='', flush=True)
                    if part_index + 1 == part_count:
                        print()

                    if progress_callback:
                        pos = min((part_index + 1) * part_size, file_size)
                        await helpers._maybe_await(progress_callback(pos, file_size))

                    break  # success

                except FloodWaitError as e:
                    wait_time = e.seconds
                    logger.warning(f"FLOOD_WAIT_{wait_time}: chunk {part_index}, waiting {wait_time}s")
                    await asyncio.sleep(wait_time)
                    retry_count += 1

                except RPCError as e:
                    error_code = e.code
                    error_msg = str(e)

                    if error_code == 303:
                        logger.warning(f"FILE_MIGRATE: {error_msg}")
                        retry_count += 1
                        if retry_count < max_retries:
                            await asyncio.sleep(2)
                        else:
                            raise

                    elif error_code == 400:
                        if "FILE_PART_" in error_msg and "_MISSING" in error_msg:
                            logger.warning(f"FILE_PART_MISSING, retrying: {error_msg}")
                            retry_count += 1
                            if retry_count < max_retries:
                                await asyncio.sleep(1)
                            else:
                                raise
                        else:
                            logger.error(f"BAD_REQUEST (400): {error_msg}")
                            raise

                    elif error_code in (401, 403, 404):
                        logger.error(f"Error ({error_code}): {error_msg}")
                        raise

                    elif error_code == 406:
                        logger.warning(f"NOT_ACCEPTABLE (406): {error_msg}")
                        raise

                    elif error_code == 500:
                        logger.warning(f"INTERNAL_ERROR (500), retrying: {error_msg}")
                        retry_count += 1
                        if retry_count < max_retries:
                            await asyncio.sleep(2)
                        else:
                            raise

                    else:
                        logger.warning(f"Unknown error ({error_code}), retrying: {error_msg}")
                        retry_count += 1
                        if retry_count < max_retries:
                            await asyncio.sleep(2)
                        else:
                            raise

                except InvalidBufferError as e:
                    logger.warning(f"Buffer error, retrying chunk {part_index}: {e}")
                    retry_count += 1
                    if retry_count < max_retries:
                        await asyncio.sleep(1)
                    else:
                        raise

                except Exception as e:
                    retry_count += 1
                    if retry_count < max_retries:
                        logger.warning(f"Chunk {part_index} error, retry {retry_count}/{max_retries}: {e}")
                        await asyncio.sleep(1)
                    else:
                        logger.error(f"Chunk {part_index + 1}/{part_count} final failure: {e}")
                        raise
        finally:
            self.upload_semaphore.release()

    async def upload_file_parallel(self, file_path: str, *, part_size_kb: float = None,
                                    file_name: str = None,
                                    progress_callback: Optional[Callable] = None) -> types.TypeInputFile:
        file_size = os.path.getsize(file_path)

        if not part_size_kb:
            part_size_kb = utils.get_appropriated_part_size(file_size)

        if part_size_kb > 512:
            raise ValueError('Part size must be <= 512KB')

        part_size = int(part_size_kb * 1024)

        if part_size % 1024 != 0:
            raise ValueError('Part size must be divisible by 1024')
        if (512 * 1024) % part_size != 0:
            raise ValueError('512KB must be divisible by part size')
        if part_size < 1024:
            raise ValueError('Part size must be >= 1KB')

        file_id = helpers.generate_random_long()
        if not file_name:
            file_name = os.path.basename(file_path) or str(file_id)

        is_big = file_size > 10 * 1024 * 1024
        hash_md5 = hashlib.md5() if not is_big else None

        part_count = (file_size + part_size - 1) // part_size
        max_parts = UPLOAD_MAX_FILEPARTS_PREMIUM

        if part_count > max_parts:
            raise ValueError(f'FILE_PARTS_INVALID: {part_count} parts exceeds limit {max_parts}')

        logger.info(f'File: {file_size:,} bytes, {part_count} chunks, {"big" if is_big else "small"} file mode')
        logger.info(f'Upload delay: {self.rate_limit_delay:.2f}s per chunk')

        start_time = time.time()

        logger.info(f"Batch upload: batch_size={self.batch_size}, concurrent={self.concurrent_tasks}")

        batch_num = 0
        for batch_start in range(0, part_count, self.batch_size):
            batch_end = min(batch_start + self.batch_size, part_count)
            batch_num += 1
            total_batches = (part_count + self.batch_size - 1) // self.batch_size

            logger.info(f"Batch {batch_num}/{total_batches}: chunks {batch_start + 1}-{batch_end}")

            batch_tasks = []
            for part_index in range(batch_start, batch_end):
                await self.upload_semaphore.acquire()
                task = self.loop.create_task(
                    self._send_file_part_with_read(
                        file_path, file_id, part_index, part_count,
                        part_size, file_size, is_big, hash_md5, progress_callback
                    ),
                    name=f"upload-task-{part_index}"
                )
                batch_tasks.append(task)

            await asyncio.gather(*batch_tasks)
            logger.info(f"Batch {batch_num}/{total_batches} done")

            if batch_end < part_count:
                await asyncio.sleep(0.1)

        total_time = time.time() - start_time
        total_batches = (part_count + self.batch_size - 1) // self.batch_size
        logger.info(f"Upload complete! {total_time:.2f}s, {part_count} chunks, {total_batches} batches")

        if is_big:
            return types.InputFileBig(file_id, part_count, file_name)
        else:
            return InputSizedFile(file_id, part_count, file_name, md5=hash_md5, size=file_size)

    # ---- Thumbnail & Media Info ----

    async def find_thumbnail(self, video_path: str) -> Optional[str]:
        thumb_path = os.path.splitext(video_path)[0] + '.jpg'
        if os.path.exists(thumb_path):
            logger.info(f"Found thumbnail: {thumb_path}")
            return thumb_path
        return None

    def _get_accurate_video_info(self, video_path: str):
        try:
            import subprocess

            try:
                subprocess.run(['ffprobe', '-version'], capture_output=True, timeout=5)
            except (FileNotFoundError, subprocess.TimeoutExpired):
                logger.warning("ffprobe not available, using default video dimensions")
                return 1920, 1080, 0

            cmd = [
                'ffprobe', '-v', 'quiet', '-print_format', 'json',
                '-show_streams', '-select_streams', 'v:0',
                video_path
            ]

            result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
            if result.returncode == 0:
                data = json.loads(result.stdout)
                streams = data.get('streams', [])

                if streams:
                    video_stream = streams[0]
                    width = int(video_stream.get('width', 1920))
                    height = int(video_stream.get('height', 1080))

                    sample_aspect_ratio = video_stream.get('sample_aspect_ratio', '1:1')
                    if sample_aspect_ratio and sample_aspect_ratio != '1:1':
                        try:
                            sar_parts = sample_aspect_ratio.split(':')
                            if len(sar_parts) == 2:
                                sar = float(sar_parts[0]) / float(sar_parts[1])
                                if sar != 1.0:
                                    width = int(width * sar)
                        except Exception:
                            pass

                    duration = float(video_stream.get('duration', 0))

                    if duration <= 0:
                        format_cmd = [
                            'ffprobe', '-v', 'quiet', '-print_format', 'json',
                            '-show_format', video_path
                        ]
                        format_result = subprocess.run(format_cmd, capture_output=True, text=True, timeout=10)
                        if format_result.returncode == 0:
                            format_data = json.loads(format_result.stdout)
                            duration = float(format_data.get('format', {}).get('duration', 0))

                    return width, height, duration

            return 1920, 1080, 0

        except Exception as e:
            logger.warning(f"Failed to get video info: {e}")
            return 1920, 1080, 0

    def _get_image_attributes(self, image_path: str):
        try:
            from PIL import Image
            from telethon.tl.types import DocumentAttributeImageSize, DocumentAttributeFilename
            attrs = []
            with Image.open(image_path) as img:
                width, height = img.size
                attrs.append(DocumentAttributeImageSize(w=width, h=height))
            return attrs
        except Exception:
            from telethon.tl.types import DocumentAttributeFilename
            return [DocumentAttributeFilename(os.path.basename(image_path))]

    def _get_video_attributes(self, video_path: str):
        try:
            from telethon.tl.types import DocumentAttributeVideo, DocumentAttributeFilename

            width, height, duration = self._get_accurate_video_info(video_path)

            if width <= 0 or height <= 0 or width > 10000 or height > 10000:
                return [DocumentAttributeFilename(os.path.basename(video_path))]

            supports_streaming = video_path.lower().endswith(('.mp4', '.mov', '.m4v'))

            return [DocumentAttributeVideo(
                duration=int(duration) if duration > 0 else 0,
                w=int(width), h=int(height),
                round_message=False,
                supports_streaming=supports_streaming,
            )]
        except Exception:
            from telethon.tl.types import DocumentAttributeFilename
            return [DocumentAttributeFilename(os.path.basename(video_path))]

    # ---- Caption Helpers ----

    def _extract_hashtags_from_filename(self, file_path: str) -> str:
        """Extract #model_name #dateYYYYMMDD from filename."""
        try:
            file_name = os.path.basename(file_path)
            name_without_ext = os.path.splitext(file_name)[0]
            parts = name_without_ext.split('_')

            if len(parts) < 2:
                return ""

            platform_keywords = ['Chaturbate', 'StripChat', 'OnlyFans', 'ManyVids', 'Cam4', 'Streamate', 'LiveJasmin']
            platform_index = -1
            for i, part in enumerate(parts):
                if part in platform_keywords:
                    platform_index = i
                    break

            if platform_index > 0:
                model_name = '_'.join(parts[:platform_index])
                search_parts = parts[platform_index + 1:]
            else:
                model_name = parts[0]
                search_parts = parts[1:]

            date_part = None
            for part in search_parts:
                if re.search(r'\d{4}[-]?\d{2}[-]?\d{2}', part):
                    date_part = part
                    break

            if date_part:
                date_match = re.search(r'(\d{4})[-]?(\d{2})[-]?(\d{2})', date_part)
                if date_match:
                    year, month, day = date_match.groups()
                    return f"#{model_name} #date{year}{month}{day}"

            return f"#{model_name}" if model_name else ""
        except Exception:
            path_parts = Path(file_path).parts
            if len(path_parts) >= 2:
                parent = path_parts[-2]
                if parent and not parent.startswith('.'):
                    return f"#{parent}"
            return ""

    def _format_duration(self, duration: float) -> str:
        if duration <= 0:
            return "unknown"
        hours = int(duration // 3600)
        minutes = int((duration % 3600) // 60)
        seconds = int(duration % 60)
        if hours > 0:
            return f"{hours:02d}:{minutes:02d}:{seconds:02d}"
        return f"{minutes:02d}:{seconds:02d}"

    def _create_enhanced_caption(self, video_file: str) -> str:
        try:
            file_size = os.path.getsize(video_file)
            file_name = os.path.basename(video_file)
            hashtag = self._extract_hashtags_from_filename(video_file)
            width, height, duration = self._get_accurate_video_info(video_file)
            size_str = format_file_size(file_size)
            duration_str = self._format_duration(duration)

            return f"""```
{file_name}
Resolution: {width}x{height}
Duration: {duration_str}
Size: {size_str}
```
{hashtag}"""
        except Exception:
            return f"```\n{os.path.basename(video_file)}\n```"

    def _create_image_caption(self, image_file: str) -> str:
        try:
            file_size = os.path.getsize(image_file)
            file_name = os.path.basename(image_file)
            hashtag = self._extract_hashtags_from_filename(image_file)
            size_str = format_file_size(file_size)

            width, height = 0, 0
            try:
                from PIL import Image
                with Image.open(image_file) as img:
                    width, height = img.size
            except Exception:
                pass

            if width > 0 and height > 0:
                return f"""```
{file_name}
Resolution: {width}x{height}
Size: {size_str}
```
{hashtag}"""
            else:
                return f"""```
{file_name}
Size: {size_str}
```
{hashtag}"""
        except Exception:
            return os.path.basename(image_file)

    # ---- File Post-Upload Handling ----

    def _handle_post_upload(self, file_path: str):
        """Handle file after successful upload (delete or mark)."""
        if self.settings.delete_after_upload:
            if self._is_file_in_exempt_folder(file_path):
                logger.info(f"File in exempt folder, skip delete")
                if self.settings.mark_uploaded_files:
                    self._mark_file_uploaded(file_path)
            else:
                try:
                    os.remove(file_path)
                    logger.info(f"Deleted: {file_path}")
                except (OSError, PermissionError) as e:
                    logger.error(f"Delete failed: {e}")
        elif self.settings.mark_uploaded_files:
            self._mark_file_uploaded(file_path)

    def _mark_file_uploaded(self, file_path: str):
        try:
            new_name = f"{file_path}{self.settings.uploaded_suffix}"
            os.rename(file_path, new_name)
            logger.info(f"Marked uploaded: {os.path.basename(file_path)}")
        except (OSError, PermissionError) as e:
            logger.error(f"Rename failed: {e}")

    # ---- Send File Enhanced ----

    async def send_file_enhanced(self, entity, file_path: str,
                                  caption: Optional[str] = None,
                                  force_document: bool = False,
                                  progress_callback: Optional[Callable] = None,
                                  thumb_path: Optional[str] = None, **kwargs):
        file_size = os.path.getsize(file_path)

        # Check file size limit
        is_premium = kwargs.pop('is_premium', False)
        max_size = MAX_FILE_SIZE_PREMIUM if is_premium else MAX_FILE_SIZE_FREE
        if file_size > max_size:
            logger.error(f"File too large: {format_file_size(file_size)}, limit: {format_file_size(max_size)}")
            return None

        start_time = time.time()

        if not caption:
            caption = self._create_enhanced_caption(file_path)

        if not thumb_path:
            thumb_path = await self.find_thumbnail(file_path)

        # Parallel upload
        input_file = await self.upload_file_parallel(file_path, progress_callback=progress_callback)

        # Upload thumbnail
        thumb = None
        if thumb_path and os.path.exists(thumb_path):
            logger.info(f"Uploading thumbnail: {thumb_path}")
            thumb = await self.upload_file(thumb_path)

        video_attributes = self._get_video_attributes(file_path)
        filtered_kwargs = {k: v for k, v in kwargs.items() if k not in ['supports_streaming', 'attributes']}

        message = await self.send_file(
            entity, input_file,
            caption=caption, force_document=force_document,
            thumb=thumb, attributes=video_attributes,
            supports_streaming=True, **filtered_kwargs
        )

        logger.info(f"Video uploaded: message ID {message.id}")

        # Upload thumbnail as separate image
        if thumb_path and os.path.exists(thumb_path):
            try:
                image_attributes = self._get_image_attributes(thumb_path)
                thumb_caption = self._create_image_caption(thumb_path)

                thumb_message = await self.send_file(
                    entity, thumb_path,
                    caption=thumb_caption, force_document=False,
                    attributes=image_attributes,
                    **{k: v for k, v in filtered_kwargs.items() if k != 'supports_streaming'}
                )
                logger.info(f"Thumbnail uploaded: message ID {thumb_message.id}")
                self._handle_post_upload(thumb_path)
            except Exception as e:
                logger.warning(f"Thumbnail upload failed (video OK): {e}")

        elapsed_time = time.time() - start_time
        upload_speed = file_size / elapsed_time / 1024 / 1024
        logger.info(f"Done! {elapsed_time:.2f}s, avg speed: {upload_speed:.2f} MB/s")

        return message

    # ---- Directory Upload ----

    async def upload_directory(self, dir_path: str, recursive: bool = True):
        if not self.channel_entity:
            logger.error("Channel not set. Call find_channel() first.")
            return False

        if not os.path.isdir(dir_path):
            logger.error(f"Directory not found: {dir_path}")
            return False

        video_extensions = {'.mp4', '.avi', '.mov', '.mkv', '.webm', '.m4v', '.flv', '.wmv'}
        directory = Path(dir_path)

        def is_available(fp):
            s = str(fp)
            if s.endswith(('.uploaded', '.big')):
                return False
            return not (os.path.exists(s + '.uploaded') or os.path.exists(s + '.big'))

        if recursive:
            file_iter = directory.rglob('*')
        else:
            file_iter = directory.iterdir()

        video_files = [
            str(fp) for fp in file_iter
            if fp.is_file() and fp.suffix.lower() in video_extensions and is_available(fp)
        ]

        logger.info(f"Found {len(video_files)} video files to upload")

        if self.settings.delete_after_upload:
            if self.settings.exempt_folders:
                logger.warning(f"Mode: delete after upload ({len(self.settings.exempt_folders)} exempt folders)")
            else:
                logger.warning(f"Mode: delete after upload (no exemptions)")
        elif self.settings.mark_uploaded_files:
            logger.info(f"Mode: mark uploaded with '{self.settings.uploaded_suffix}'")

        # Group by folder
        folder_groups = defaultdict(list)
        for vf in video_files:
            folder_groups[os.path.dirname(vf)].append(vf)

        available_folders = self._select_available_folders(folder_groups)
        if not available_folders:
            logger.warning("No available folders to upload")
            return False

        logger.info(f"Uploading from {len(available_folders)} folders")

        success_count = 0
        skipped_count = 0
        current_locked_folder = None
        big_threshold = 2 * 1024 * 1024 * 1024  # 2GB

        try:
            for folder_path in available_folders:
                if not self._create_folder_lock(folder_path):
                    logger.warning(f"Cannot lock folder, skipping: {folder_path}")
                    continue

                current_locked_folder = folder_path
                folder_videos = sorted(folder_groups[folder_path])
                logger.info(f"Processing folder: {folder_path} ({len(folder_videos)} files)")

                for video_file in folder_videos:
                    try:
                        file_size = os.path.getsize(video_file)

                        if file_size > big_threshold:
                            logger.warning(f"Skipping oversized file: {os.path.basename(video_file)} ({format_file_size(file_size)})")
                            skipped_count += 1
                            try:
                                os.rename(video_file, f"{video_file}.big")
                            except (OSError, PermissionError) as e:
                                logger.error(f"Rename failed: {e}")
                            continue

                        if file_size > MAX_FILE_SIZE_FREE:
                            logger.warning(f"File exceeds Telegram limit: {os.path.basename(video_file)} ({format_file_size(file_size)})")
                            skipped_count += 1
                            continue

                        progress_bar = ProgressBar(file_size, f"Upload {os.path.basename(video_file)}")
                        thumb_path = await self.find_thumbnail(video_file)

                        logger.info(f"\nUploading: {os.path.basename(video_file)}")
                        message = await self.send_file_enhanced(
                            self.channel_entity, video_file,
                            progress_callback=progress_bar,
                            thumb_path=thumb_path,
                            supports_streaming=True,
                        )

                        if message:
                            success_count += 1
                            logger.info(f"Success: {os.path.basename(video_file)}")
                            self._handle_post_upload(video_file)
                            await asyncio.sleep(1)

                    except FileNotFoundError as e:
                        logger.error(f"File not found {os.path.basename(video_file)}: {e}")
                    except PermissionError as e:
                        logger.error(f"Permission error {os.path.basename(video_file)}: {e}")
                    except Exception as e:
                        logger.error(f"Upload failed {os.path.basename(video_file)}: {e}")

                self._remove_folder_lock(current_locked_folder)
                current_locked_folder = None

        except KeyboardInterrupt:
            logger.info("\nUser interrupted")
            if current_locked_folder:
                self._remove_folder_lock(current_locked_folder)
            raise
        except Exception as e:
            logger.error(f"Fatal error during batch upload: {e}")
            if current_locked_folder:
                self._remove_folder_lock(current_locked_folder)
            raise
        finally:
            if current_locked_folder:
                self._remove_folder_lock(current_locked_folder)

        logger.info(f"\nBatch complete: {success_count}/{len(video_files)} uploaded, {skipped_count} skipped")
        return success_count > 0


# ============================================================
# CLI Commands
# ============================================================

async def cmd_upload(args):
    account = AccountConfig.load(args.account)
    settings = SharedSettings.load()

    upload_dir = args.dir or settings.default_upload_dir
    if not upload_dir or not os.path.isdir(upload_dir):
        print(f"Error: directory not found: {upload_dir}")
        sys.exit(1)

    uploader = EnhancedParallelUploader(
        account=account, settings=settings,
        batch_size=args.batch_size,
        concurrent_tasks=args.concurrent,
        custom_delay=args.delay,
    )

    try:
        await uploader.connect()

        if not await uploader.is_user_authorized():
            print(f"Error: session expired for '{args.account}'. Run:")
            print(f"  python enhanced_uploader.py login --account {args.account}")
            sys.exit(1)

        me = await uploader.get_me()
        is_premium = getattr(me, 'premium', False)
        print(f"[{account.name}] Logged in: {me.first_name} ({me.phone}) {'Premium' if is_premium else 'Free'}")

        if not await uploader.find_channel(account.channel_id):
            print(f"Error: channel not found: {account.channel_id}")
            sys.exit(1)

        await uploader.upload_directory(upload_dir, recursive=args.recursive)

    except KeyboardInterrupt:
        print(f"\n[{account.name}] Interrupted")
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        await uploader.disconnect()


async def cmd_login(args):
    account = AccountConfig.load(args.account)

    session_file = account.session_path + ".session"
    if os.path.exists(session_file):
        os.remove(session_file)
        print(f"Removed old session: {session_file}")

    client = TelegramClient(account.session_path, account.api_id, account.api_hash)
    await client.connect()
    await client.start(phone=account.phone)

    if await client.is_user_authorized():
        me = await client.get_me()
        is_premium = getattr(me, 'premium', False)
        print(f"Login successful: {me.first_name} ({me.phone}) {'Premium' if is_premium else 'Free'}")
    else:
        print("Login failed")

    await client.disconnect()


async def cmd_accounts(_args):
    accounts = AccountConfig.list_all()
    if not accounts:
        print("No accounts configured. Edit .account_configs/accounts.json")
        return

    print(f"{'Name':<10} {'Phone':<18} {'Channel':<20} {'Session'}")
    print("-" * 68)
    for acc in accounts:
        session_exists = "OK" if os.path.exists(acc.session_path + ".session") else "MISSING"
        print(f"{acc.name:<10} {acc.phone:<18} {acc.channel_id:<20} {session_exists}")


# ============================================================
# Main
# ============================================================

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Telegram Video Uploader - parallel upload for live stream recordings"
    )
    subparsers = parser.add_subparsers(dest="command")

    # upload
    p_upload = subparsers.add_parser("upload", help="Upload videos from a directory")
    p_upload.add_argument("--account", "-a", required=True, help="Account name (e.g., wang, aoo, ajx, jane, pussy)")
    p_upload.add_argument("--dir", "-d", help="Directory to upload from (default: from .env DEFAULT_UPLOAD_DIR)")
    p_upload.add_argument("--recursive", "-r", action="store_true", default=True, help="Recursively scan subdirectories (default: true)")
    p_upload.add_argument("--no-recursive", dest="recursive", action="store_false", help="Do not scan subdirectories")
    p_upload.add_argument("--batch-size", type=int, default=100, help="Chunks per batch (default: 100)")
    p_upload.add_argument("--concurrent", type=int, default=1, help="Concurrent tasks per batch (default: 1)")
    p_upload.add_argument("--delay", type=float, default=1.0, help="Delay between tasks in seconds (default: 1.0)")

    # login
    p_login = subparsers.add_parser("login", help="Login or re-login an account")
    p_login.add_argument("--account", "-a", required=True, help="Account name to login")

    # accounts
    subparsers.add_parser("accounts", help="List all configured accounts")

    return parser


async def main():
    parser = build_parser()
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    if args.command == "upload":
        await cmd_upload(args)
    elif args.command == "login":
        await cmd_login(args)
    elif args.command == "accounts":
        await cmd_accounts(args)


if __name__ == "__main__":
    asyncio.run(main())
