#!/usr/bin/env python3
"""
统计 fail2ban 封禁 IP 的地理来源
使用 ip-api.com 免费批量查询接口，支持断点续传
"""

import subprocess
import json
import time
import sys
import os
from collections import Counter
from urllib.request import Request, urlopen
from urllib.error import URLError

BATCH_SIZE = 100  # ip-api.com 批量接口每次最多 100 个
RATE_LIMIT_DELAY = 4  # 秒，免费接口限制 15 次/分钟
MAX_RETRIES = 3
CACHE_FILE = "banned_ip_cache.json"


def get_banned_ips():
    """从 fail2ban 获取被封禁的 IP 列表"""
    try:
        result = subprocess.run(
            ["sudo", "fail2ban-client", "status", "sshd"],
            capture_output=True, text=True, timeout=30
        )
        for line in result.stdout.splitlines():
            if "Banned IP list" in line:
                ip_str = line.split("Banned IP list:")[1].strip()
                return ip_str.split()
    except Exception as e:
        print(f"获取 fail2ban 数据失败: {e}")
    return []


def load_cache():
    """加载已查询的缓存"""
    if os.path.exists(CACHE_FILE):
        with open(CACHE_FILE, "r") as f:
            return json.load(f)
    return {}


def save_cache(cache):
    """保存缓存"""
    with open(CACHE_FILE, "w") as f:
        json.dump(cache, f)


def batch_query_ips(ip_list):
    """批量查询 IP 地理信息，带重试"""
    payload = json.dumps([{"query": ip, "fields": "query,status,country,regionName,city,isp"} for ip in ip_list])
    for attempt in range(MAX_RETRIES):
        try:
            req = Request(
                "http://ip-api.com/batch",
                data=payload.encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST"
            )
            with urlopen(req, timeout=30) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as e:
            wait = (attempt + 1) * 10
            print(f"\n  请求失败({e})，{wait}秒后重试 ({attempt+1}/{MAX_RETRIES})...")
            time.sleep(wait)
    return None


def main():
    print("正在获取 fail2ban 封禁 IP 列表...")
    ips = get_banned_ips()
    if not ips:
        print("没有获取到被封禁的 IP")
        sys.exit(1)

    total = len(ips)
    cache = load_cache()
    cached_count = sum(1 for ip in ips if ip in cache)
    print(f"共 {total} 个被封禁 IP，缓存命中 {cached_count} 个，需查询 {total - cached_count} 个\n")

    # 找出未缓存的 IP
    uncached_ips = [ip for ip in ips if ip not in cache]

    if uncached_ips:
        batches = [uncached_ips[i:i + BATCH_SIZE] for i in range(0, len(uncached_ips), BATCH_SIZE)]
        for idx, batch in enumerate(batches):
            done = cached_count + (idx + 1) * BATCH_SIZE
            done = min(done, total)
            print(f"\r  查询进度: {done}/{total} ({done * 100 // total}%)", end="", flush=True)

            results = batch_query_ips(batch)
            if results is None:
                print(f"\n  跳过批次 {idx+1}（重试用尽）")
                continue

            for r in results:
                ip = r.get("query", "")
                cache[ip] = r

            # 每批次保存缓存（断点续传）
            save_cache(cache)
            time.sleep(RATE_LIMIT_DELAY)

        print("\n")

    # 统计
    country_counter = Counter()
    region_counter = Counter()
    city_counter = Counter()
    isp_counter = Counter()
    failed = 0

    for ip in ips:
        r = cache.get(ip)
        if r and r.get("status") == "success":
            country = r.get("country", "Unknown")
            region = r.get("regionName", "Unknown")
            city = r.get("city", "Unknown")
            isp = r.get("isp", "Unknown")
            country_counter[country] += 1
            region_counter[f"{country} - {region}"] += 1
            city_counter[f"{country} - {city}"] += 1
            isp_counter[isp] += 1
        else:
            failed += 1

    resolved = total - failed

    print("=" * 60)
    print(f"  fail2ban 封禁 IP 来源统计")
    print(f"  总计: {total} | 成功解析: {resolved} | 失败: {failed}")
    print("=" * 60)

    top1_count = country_counter.most_common(1)[0][1] if country_counter else 1

    print(f"\n{'='*60}")
    print(f"  按国家/地区统计 (Top 30)")
    print(f"{'='*60}")
    for country, count in country_counter.most_common(30):
        bar = "█" * (count * 40 // top1_count)
        pct = count * 100 / resolved if resolved else 0
        print(f"  {country:<25} {count:>6}  ({pct:5.1f}%)  {bar}")

    print(f"\n{'='*60}")
    print(f"  按省/州统计 (Top 30)")
    print(f"{'='*60}")
    for region, count in region_counter.most_common(30):
        pct = count * 100 / resolved if resolved else 0
        print(f"  {region:<40} {count:>6}  ({pct:5.1f}%)")

    print(f"\n{'='*60}")
    print(f"  按城市统计 (Top 30)")
    print(f"{'='*60}")
    for city, count in city_counter.most_common(30):
        pct = count * 100 / resolved if resolved else 0
        print(f"  {city:<40} {count:>6}  ({pct:5.1f}%)")

    print(f"\n{'='*60}")
    print(f"  按 ISP 统计 (Top 20)")
    print(f"{'='*60}")
    for isp, count in isp_counter.most_common(20):
        pct = count * 100 / resolved if resolved else 0
        print(f"  {isp:<45} {count:>6}  ({pct:5.1f}%)")

    # 保存报告
    output_file = "banned_ip_report.json"
    report = {
        "total": total,
        "resolved": resolved,
        "failed": failed,
        "by_country": dict(country_counter.most_common()),
        "by_region": dict(region_counter.most_common()),
        "by_city": dict(city_counter.most_common()),
        "by_isp": dict(isp_counter.most_common()),
    }
    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(f"\n详细报告已保存到: {output_file}")


if __name__ == "__main__":
    main()
