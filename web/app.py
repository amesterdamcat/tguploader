"""
Model Search API — FastAPI backend for Telegram channel video scanner.
"""

import csv
import io
import json
import os
import secrets
import sqlite3
import time
from pathlib import Path
from typing import Optional

import bcrypt
import jwt
from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# --- Configuration ---

BASE_DIR = Path(__file__).resolve().parent.parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "scanner.db"
USERS_FILE = Path(__file__).resolve().parent / "users.json"

JWT_SECRET = os.environ.get("JWT_SECRET", secrets.token_hex(32))
JWT_ALGORITHM = "HS256"
JWT_EXPIRE_HOURS = 24

# Private channel: -1001810923743 → t.me/c/1810923743/{url_id}
CHANNEL_ID_BARE = "1810923743"

app = FastAPI(title="Model Search API", docs_url=None, redoc_url=None)


# --- Rate limiter (in-memory) ---

class RateLimiter:
    def __init__(self, max_attempts: int = 5, window_seconds: int = 60):
        self.max_attempts = max_attempts
        self.window = window_seconds
        self.attempts: dict[str, list[float]] = {}

    def check(self, key: str) -> bool:
        now = time.time()
        if key not in self.attempts:
            self.attempts[key] = []
        # Prune old entries
        self.attempts[key] = [t for t in self.attempts[key] if now - t < self.window]
        if len(self.attempts[key]) >= self.max_attempts:
            return False
        self.attempts[key].append(now)
        return True


login_limiter = RateLimiter(max_attempts=5, window_seconds=60)


# --- Auth helpers ---

def load_users() -> dict[str, str]:
    if not USERS_FILE.exists():
        return {}
    with open(USERS_FILE) as f:
        return json.load(f)


def verify_password(plain: str, hashed: str) -> bool:
    return bcrypt.checkpw(plain.encode(), hashed.encode())


def create_token(username: str) -> str:
    payload = {
        "sub": username,
        "exp": int(time.time()) + JWT_EXPIRE_HOURS * 3600,
        "iat": int(time.time()),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)


def get_current_user(request: Request) -> str:
    auth = request.headers.get("Authorization", "")
    if not auth.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing token")
    token = auth[7:]
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return payload["sub"]
    except jwt.ExpiredSignatureError:
        raise HTTPException(status_code=401, detail="Token expired")
    except jwt.InvalidTokenError:
        raise HTTPException(status_code=401, detail="Invalid token")


# --- Database helper ---

def get_db() -> sqlite3.Connection:
    conn = sqlite3.connect(str(DB_PATH), timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


# --- Models ---

class LoginRequest(BaseModel):
    username: str
    password: str


# --- Constants ---

RESOLUTION_CASE = """
CASE
    WHEN height >= 2160 THEN '4K'
    WHEN height >= 1080 THEN '1080p'
    WHEN height >= 720 THEN '720p'
    WHEN height >= 480 THEN '480p'
    WHEN height > 0 THEN 'Other'
    ELSE 'Unknown'
END
"""

SORT_OPTIONS = {
    "date_desc": "v.record_date DESC, v.record_time DESC",
    "date_asc": "v.record_date ASC, v.record_time ASC",
    "duration_desc": "v.duration DESC",
    "size_desc": "v.file_size DESC",
}


# --- Endpoints ---

@app.post("/api/login")
def login(body: LoginRequest, request: Request):
    client_ip = request.headers.get("X-Real-IP", request.client.host if request.client else "unknown")
    if not login_limiter.check(client_ip):
        raise HTTPException(status_code=429, detail="Too many login attempts, try again later")

    users = load_users()
    if body.username not in users:
        raise HTTPException(status_code=401, detail="Invalid credentials")

    if not verify_password(body.password, users[body.username]):
        raise HTTPException(status_code=401, detail="Invalid credentials")

    token = create_token(body.username)
    return {"token": token}


@app.get("/api/videos")
def search_videos(
    q: str = "",
    platform: str = "",
    date_from: str = "",
    date_to: str = "",
    duration_min: int = 0,
    duration_max: int = 0,
    sort: str = "date_desc",
    page: int = 1,
    per_page: int = 50,
    _user: str = Depends(get_current_user),
):
    if per_page > 200:
        per_page = 200
    if page < 1:
        page = 1
    offset = (page - 1) * per_page
    order_by = SORT_OPTIONS.get(sort, SORT_OPTIONS["date_desc"])

    conn = get_db()
    try:
        conditions = []
        params = []

        if q:
            fts_query = q.strip().replace('"', '') + '*'
            conditions.append(
                "v.message_id IN (SELECT rowid FROM videos_fts WHERE videos_fts MATCH ?)"
            )
            params.append(fts_query)

        if platform:
            conditions.append("v.platform = ?")
            params.append(platform)

        if date_from:
            conditions.append("v.record_date >= ?")
            params.append(date_from)

        if date_to:
            conditions.append("v.record_date <= ?")
            params.append(date_to)

        if duration_min > 0:
            conditions.append("v.duration >= ?")
            params.append(duration_min)

        if duration_max > 0:
            conditions.append("v.duration <= ?")
            params.append(duration_max)

        where = " AND ".join(conditions) if conditions else "1=1"

        count_sql = f"SELECT COUNT(*) FROM videos v WHERE {where}"
        total = conn.execute(count_sql, params).fetchone()[0]

        query_sql = (
            f"SELECT v.* FROM videos v WHERE {where} "
            f"ORDER BY {order_by} "
            "LIMIT ? OFFSET ?"
        )
        rows = conn.execute(query_sql, params + [per_page, offset]).fetchall()

        thumbs_dir = DATA_DIR / "thumbs"
        items = []
        for row in rows:
            # Detect HD thumbnail (photo > 100KB vs video cover < 100KB)
            thumb_sz = 0
            if row["thumb_path"]:
                tp = thumbs_dir / f"{row['url_id']}.jpg"
                try:
                    thumb_sz = tp.stat().st_size
                except OSError:
                    pass
            items.append({
                "message_id": row["message_id"],
                "url_id": row["url_id"],
                "file_name": row["file_name"],
                "model_name": row["model_name"],
                "platform": row["platform"],
                "record_date": row["record_date"],
                "record_time": row["record_time"],
                "duration": row["duration"],
                "width": row["width"],
                "height": row["height"],
                "file_size": row["file_size"],
                "thumb_path": row["thumb_path"],
                "caption_text": row["caption_text"],
                "uploaded_at": row["uploaded_at"],
                "tg_link": f"https://t.me/c/{CHANNEL_ID_BARE}/{row['url_id']}",
                "thumb_hd": thumb_sz >= 100_000,
            })

        return {
            "total": total,
            "page": page,
            "per_page": per_page,
            "pages": (total + per_page - 1) // per_page if per_page > 0 else 0,
            "items": items,
        }
    finally:
        conn.close()


@app.get("/api/models")
def list_models(
    q: str = "",
    _user: str = Depends(get_current_user),
):
    conn = get_db()
    try:
        if q:
            sql = (
                "SELECT model_name, COUNT(*) as count FROM videos "
                "WHERE model_name LIKE ? "
                "GROUP BY model_name COLLATE NOCASE "
                "ORDER BY count DESC LIMIT 100"
            )
            rows = conn.execute(sql, [q + "%"]).fetchall()
        else:
            sql = (
                "SELECT model_name, COUNT(*) as count FROM videos "
                "GROUP BY model_name COLLATE NOCASE "
                "ORDER BY count DESC LIMIT 100"
            )
            rows = conn.execute(sql).fetchall()

        return [{"model_name": r["model_name"], "count": r["count"]} for r in rows]
    finally:
        conn.close()


@app.get("/api/platforms")
def list_platforms(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        sql = (
            "SELECT platform, COUNT(*) as count FROM videos "
            "WHERE platform != '' "
            "GROUP BY platform ORDER BY count DESC"
        )
        rows = conn.execute(sql).fetchall()
        return [{"platform": r["platform"], "count": r["count"]} for r in rows]
    finally:
        conn.close()


@app.get("/api/stats")
def get_stats(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        total = conn.execute("SELECT COUNT(*) FROM videos").fetchone()[0]
        models = conn.execute(
            "SELECT COUNT(DISTINCT model_name) FROM videos"
        ).fetchone()[0]
        date_range = conn.execute(
            "SELECT MIN(record_date), MAX(record_date) FROM videos WHERE record_date != ''"
        ).fetchone()
        total_size = conn.execute("SELECT SUM(file_size) FROM videos").fetchone()[0] or 0
        total_duration = conn.execute("SELECT SUM(duration) FROM videos").fetchone()[0] or 0
        now = int(time.time())
        recent_24h = conn.execute(
            "SELECT COUNT(*) FROM videos WHERE uploaded_at > ?", [now - 86400]
        ).fetchone()[0]
        recent_7d = conn.execute(
            "SELECT COUNT(*) FROM videos WHERE uploaded_at > ?", [now - 86400 * 7]
        ).fetchone()[0]
        return {
            "total_videos": total,
            "total_models": models,
            "total_size": total_size,
            "total_duration": total_duration,
            "date_from": date_range[0] or "",
            "date_to": date_range[1] or "",
            "recent_24h": recent_24h,
            "recent_7d": recent_7d,
        }
    finally:
        conn.close()


# --- Dashboard endpoints ---

@app.get("/api/dashboard/daily-trend")
def daily_trend(
    days: int = 30,
    _user: str = Depends(get_current_user),
):
    if days > 90:
        days = 90
    conn = get_db()
    try:
        cutoff = int(time.time()) - days * 86400
        rows = conn.execute("""
            SELECT DATE(uploaded_at, 'unixepoch') as day, COUNT(*) as count
            FROM videos WHERE uploaded_at > ?
            GROUP BY day ORDER BY day
        """, [cutoff]).fetchall()
        return [{"date": r["day"], "count": r["count"]} for r in rows if r["day"]]
    finally:
        conn.close()


@app.get("/api/dashboard/hourly-heatmap")
def hourly_heatmap(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        rows = conn.execute("""
            SELECT CAST(SUBSTR(record_time, 1, 2) AS INTEGER) as hour,
                   CAST(STRFTIME('%w', record_date) AS INTEGER) as dow,
                   COUNT(*) as count
            FROM videos
            WHERE record_time != '' AND record_date != ''
              AND STRFTIME('%w', record_date) IS NOT NULL
            GROUP BY hour, dow
        """).fetchall()
        # Build 7x24 matrix (dow 0=Sun, 1=Mon, ..., 6=Sat)
        matrix = [[0] * 24 for _ in range(7)]
        for r in rows:
            dow = r["dow"]
            hour = r["hour"]
            if dow is not None and hour is not None and 0 <= dow <= 6 and 0 <= hour <= 23:
                matrix[dow][hour] = r["count"]
        return {"matrix": matrix}
    finally:
        conn.close()


@app.get("/api/dashboard/platform-stats")
def platform_stats(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        rows = conn.execute("""
            SELECT platform,
                   COUNT(*) as video_count,
                   SUM(file_size) as total_size,
                   SUM(duration) as total_duration
            FROM videos WHERE platform != ''
            GROUP BY platform ORDER BY video_count DESC
        """).fetchall()
        return [
            {
                "platform": r["platform"],
                "video_count": r["video_count"],
                "total_size": r["total_size"] or 0,
                "total_duration": r["total_duration"] or 0,
            }
            for r in rows
        ]
    finally:
        conn.close()


@app.get("/api/dashboard/resolution-stats")
def resolution_stats(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        rows = conn.execute(f"""
            SELECT {RESOLUTION_CASE} as res_class,
                   COUNT(*) as video_count,
                   SUM(file_size) as total_size,
                   SUM(duration) as total_duration,
                   COUNT(DISTINCT model_name) as model_count
            FROM videos
            GROUP BY res_class
            ORDER BY video_count DESC
        """).fetchall()
        return [
            {
                "resolution": r["res_class"],
                "video_count": r["video_count"],
                "total_size": r["total_size"] or 0,
                "total_duration": r["total_duration"] or 0,
                "model_count": r["model_count"],
            }
            for r in rows
        ]
    finally:
        conn.close()


# --- Leaderboard ---

@app.get("/api/leaderboard")
def leaderboard(
    sort: str = "count",
    resolution: str = "",
    period: str = "",
    page: int = 1,
    per_page: int = 50,
    _user: str = Depends(get_current_user),
):
    if per_page > 200:
        per_page = 200
    if page < 1:
        page = 1
    offset = (page - 1) * per_page

    order = "total_duration DESC" if sort == "duration" else "video_count DESC"

    conn = get_db()
    try:
        conditions = []
        params: list = []

        if resolution:
            conditions.append(f"{RESOLUTION_CASE} = ?")
            params.append(resolution)

        if period:
            now = int(time.time())
            period_map = {"7d": 7, "30d": 30}
            days = period_map.get(period, 0)
            if days:
                conditions.append("uploaded_at > ?")
                params.append(now - days * 86400)

        where = "WHERE " + " AND ".join(conditions) if conditions else ""

        count_sql = f"SELECT COUNT(DISTINCT model_name) FROM videos {where}"
        total_models = conn.execute(count_sql, params).fetchone()[0]

        sql = f"""
            SELECT model_name,
                   COUNT(*) as video_count,
                   SUM(duration) as total_duration,
                   CAST(AVG(duration) AS INTEGER) as avg_duration,
                   SUM(file_size) as total_size,
                   MIN(record_date) as first_date,
                   MAX(record_date) as last_date
            FROM videos
            {where}
            GROUP BY model_name COLLATE NOCASE
            ORDER BY {order}
            LIMIT ? OFFSET ?
        """
        rows = conn.execute(sql, params + [per_page, offset]).fetchall()

        items = []
        for i, row in enumerate(rows):
            items.append({
                "rank": offset + i + 1,
                "model_name": row["model_name"],
                "video_count": row["video_count"],
                "total_duration": row["total_duration"] or 0,
                "avg_duration": row["avg_duration"] or 0,
                "total_size": row["total_size"] or 0,
                "first_date": row["first_date"] or "",
                "last_date": row["last_date"] or "",
            })

        return {
            "total": total_models,
            "page": page,
            "per_page": per_page,
            "pages": (total_models + per_page - 1) // per_page if per_page > 0 else 0,
            "items": items,
        }
    finally:
        conn.close()


@app.get("/api/leaderboard/export")
def leaderboard_export(
    sort: str = "count",
    resolution: str = "",
    period: str = "",
    _user: str = Depends(get_current_user),
):
    order = "total_duration DESC" if sort == "duration" else "video_count DESC"

    conn = get_db()
    try:
        conditions = []
        params: list = []
        if resolution:
            conditions.append(f"{RESOLUTION_CASE} = ?")
            params.append(resolution)
        if period:
            now = int(time.time())
            period_map = {"7d": 7, "30d": 30}
            days = period_map.get(period, 0)
            if days:
                conditions.append("uploaded_at > ?")
                params.append(now - days * 86400)

        where = "WHERE " + " AND ".join(conditions) if conditions else ""

        sql = f"""
            SELECT model_name,
                   COUNT(*) as video_count,
                   SUM(duration) as total_duration,
                   CAST(AVG(duration) AS INTEGER) as avg_duration,
                   SUM(file_size) as total_size,
                   MIN(record_date) as first_date,
                   MAX(record_date) as last_date
            FROM videos {where}
            GROUP BY model_name COLLATE NOCASE
            ORDER BY {order}
        """
        rows = conn.execute(sql, params).fetchall()

        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow(["Rank", "Model", "Videos", "Total Duration (s)",
                         "Avg Duration (s)", "Total Size (bytes)", "First Date", "Last Date"])
        for i, row in enumerate(rows):
            writer.writerow([
                i + 1, row["model_name"], row["video_count"],
                row["total_duration"] or 0, row["avg_duration"] or 0,
                row["total_size"] or 0, row["first_date"] or "", row["last_date"] or "",
            ])

        buf.seek(0)
        return StreamingResponse(
            buf,
            media_type="text/csv",
            headers={"Content-Disposition": "attachment; filename=leaderboard.csv"},
        )
    finally:
        conn.close()


@app.get("/api/resolutions")
def list_resolutions(_user: str = Depends(get_current_user)):
    conn = get_db()
    try:
        sql = f"""
            SELECT {RESOLUTION_CASE} as res_class,
                   COUNT(*) as video_count,
                   COUNT(DISTINCT model_name) as model_count
            FROM videos
            GROUP BY res_class
            ORDER BY video_count DESC
        """
        rows = conn.execute(sql).fetchall()
        return [
            {
                "resolution": r["res_class"],
                "video_count": r["video_count"],
                "model_count": r["model_count"],
            }
            for r in rows
        ]
    finally:
        conn.close()


@app.get("/api/model/{model_name}/resolutions")
def model_resolutions(
    model_name: str,
    _user: str = Depends(get_current_user),
):
    conn = get_db()
    try:
        sql = f"""
            SELECT {RESOLUTION_CASE} as res_class,
                   COUNT(*) as count
            FROM videos
            WHERE model_name = ? COLLATE NOCASE
            GROUP BY res_class
            ORDER BY count DESC
        """
        rows = conn.execute(sql, [model_name]).fetchall()
        return [{"resolution": r["res_class"], "count": r["count"]} for r in rows]
    finally:
        conn.close()


@app.get("/api/model/{model_name}/detail")
def model_detail(
    model_name: str,
    _user: str = Depends(get_current_user),
):
    conn = get_db()
    try:
        row = conn.execute("""
            SELECT COUNT(*) as video_count,
                   SUM(duration) as total_duration,
                   CAST(AVG(duration) AS INTEGER) as avg_duration,
                   SUM(file_size) as total_size,
                   MIN(record_date) as first_date,
                   MAX(record_date) as last_date
            FROM videos WHERE model_name = ? COLLATE NOCASE
        """, [model_name]).fetchone()

        if not row or row["video_count"] == 0:
            raise HTTPException(status_code=404, detail="Model not found")

        plats = conn.execute("""
            SELECT platform, COUNT(*) as count FROM videos
            WHERE model_name = ? COLLATE NOCASE AND platform != ''
            GROUP BY platform ORDER BY count DESC
        """, [model_name]).fetchall()

        res = conn.execute(f"""
            SELECT {RESOLUTION_CASE} as res_class, COUNT(*) as count
            FROM videos WHERE model_name = ? COLLATE NOCASE
            GROUP BY res_class ORDER BY count DESC
        """, [model_name]).fetchall()

        timeline = conn.execute("""
            SELECT SUBSTR(record_date, 1, 7) as month, COUNT(*) as count
            FROM videos
            WHERE model_name = ? COLLATE NOCASE AND record_date != ''
            GROUP BY month ORDER BY month DESC LIMIT 12
        """, [model_name]).fetchall()

        thumb_row = conn.execute("""
            SELECT url_id FROM videos
            WHERE model_name = ? COLLATE NOCASE AND thumb_path != ''
            ORDER BY record_date DESC, record_time DESC LIMIT 1
        """, [model_name]).fetchone()

        return {
            "model_name": model_name,
            "video_count": row["video_count"],
            "total_duration": row["total_duration"] or 0,
            "avg_duration": row["avg_duration"] or 0,
            "total_size": row["total_size"] or 0,
            "first_date": row["first_date"] or "",
            "last_date": row["last_date"] or "",
            "platforms": [{"platform": p["platform"], "count": p["count"]} for p in plats],
            "resolutions": [{"resolution": r["res_class"], "count": r["count"]} for r in res],
            "timeline": [{"month": t["month"], "count": t["count"]} for t in reversed(list(timeline))],
            "thumb_url_id": thumb_row["url_id"] if thumb_row else None,
        }
    finally:
        conn.close()
