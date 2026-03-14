#!/usr/bin/env python3
"""
从 banned_ip_report.json 生成 HTML 可视化报告
使用 Chart.js（CDN）绘制图表，纯静态 HTML
"""

import json
import sys
from datetime import datetime

REPORT_FILE = "banned_ip_report.json"
OUTPUT_FILE = "banned_ip_report.html"


def load_report():
    with open(REPORT_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def top_n(data: dict, n: int):
    items = sorted(data.items(), key=lambda x: x[1], reverse=True)
    return items[:n]


def generate_html(report):
    total = report["total"]
    resolved = report["resolved"]
    failed = report["failed"]
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    countries = top_n(report["by_country"], 30)
    regions = top_n(report["by_region"], 30)
    cities = top_n(report["by_city"], 30)
    isps = top_n(report["by_isp"], 20)

    # 饼图用 top 10 + Others
    pie_top = top_n(report["by_country"], 10)
    pie_other = total - sum(v for _, v in pie_top)
    pie_labels = [c for c, _ in pie_top] + (["Others"] if pie_other > 0 else [])
    pie_values = [v for _, v in pie_top] + ([pie_other] if pie_other > 0 else [])

    # 颜色
    colors = [
        "#e74c3c", "#3498db", "#2ecc71", "#f39c12", "#9b59b6",
        "#1abc9c", "#e67e22", "#34495e", "#e91e63", "#00bcd4",
        "#8bc34a", "#ff9800", "#795548", "#607d8b", "#673ab7",
        "#ff5722", "#009688", "#cddc39", "#ffc107", "#03a9f4",
        "#4caf50", "#f44336", "#2196f3", "#ffeb3b", "#9c27b0",
        "#00e676", "#ff1744", "#651fff", "#14213d", "#fca311",
    ]

    def table_rows(items, rank=True):
        rows = []
        for i, (name, count) in enumerate(items, 1):
            pct = count * 100 / resolved if resolved else 0
            bar_width = count * 100 / items[0][1] if items[0][1] else 0
            rank_col = f"<td>{i}</td>" if rank else ""
            rows.append(f"""<tr>
                {rank_col}
                <td>{name}</td>
                <td class="num">{count:,}</td>
                <td class="num">{pct:.1f}%</td>
                <td><div class="bar" style="width:{bar_width:.1f}%"></div></td>
            </tr>""")
        return "\n".join(rows)

    html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Fail2ban 封禁 IP 来源统计</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.7/dist/chart.umd.min.js"></script>
<style>
* {{ margin: 0; padding: 0; box-sizing: border-box; }}
body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #e2e8f0; padding: 20px; }}
.container {{ max-width: 1200px; margin: 0 auto; }}
h1 {{ text-align: center; font-size: 1.8em; margin-bottom: 8px; color: #f8fafc; }}
.subtitle {{ text-align: center; color: #94a3b8; margin-bottom: 30px; font-size: 0.9em; }}

.stats-grid {{
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 16px;
    margin-bottom: 30px;
}}
.stat-card {{
    background: #1e293b;
    border-radius: 12px;
    padding: 20px;
    text-align: center;
    border: 1px solid #334155;
}}
.stat-card .number {{ font-size: 2.2em; font-weight: 700; color: #38bdf8; }}
.stat-card .label {{ color: #94a3b8; font-size: 0.85em; margin-top: 4px; }}

.charts-row {{
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
    margin-bottom: 30px;
}}
@media (max-width: 768px) {{ .charts-row {{ grid-template-columns: 1fr; }} }}

.card {{
    background: #1e293b;
    border-radius: 12px;
    padding: 24px;
    border: 1px solid #334155;
    margin-bottom: 20px;
}}
.card h2 {{ font-size: 1.2em; margin-bottom: 16px; color: #f1f5f9; }}
.chart-container {{ position: relative; width: 100%; }}

table {{ width: 100%; border-collapse: collapse; font-size: 0.85em; }}
th {{ text-align: left; padding: 10px 8px; border-bottom: 2px solid #334155; color: #94a3b8; font-weight: 600; text-transform: uppercase; font-size: 0.8em; letter-spacing: 0.5px; }}
td {{ padding: 8px; border-bottom: 1px solid #1e293b; }}
tr:hover {{ background: #1e293b; }}
.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
.bar {{ height: 8px; background: linear-gradient(90deg, #3b82f6, #06b6d4); border-radius: 4px; min-width: 2px; }}
td:last-child {{ width: 30%; }}

.tabs {{ display: flex; gap: 8px; margin-bottom: 16px; flex-wrap: wrap; }}
.tab {{
    padding: 8px 16px;
    border-radius: 8px;
    background: #334155;
    color: #94a3b8;
    cursor: pointer;
    border: none;
    font-size: 0.85em;
    transition: all 0.2s;
}}
.tab:hover {{ background: #475569; color: #e2e8f0; }}
.tab.active {{ background: #3b82f6; color: white; }}
.tab-content {{ display: none; }}
.tab-content.active {{ display: block; }}
</style>
</head>
<body>
<div class="container">
    <h1>Fail2ban SSH 封禁 IP 来源分析</h1>
    <p class="subtitle">报告生成时间: {now}</p>

    <div class="stats-grid">
        <div class="stat-card">
            <div class="number">{total:,}</div>
            <div class="label">封禁 IP 总数</div>
        </div>
        <div class="stat-card">
            <div class="number">{resolved:,}</div>
            <div class="label">成功解析</div>
        </div>
        <div class="stat-card">
            <div class="number">{len(report['by_country']):,}</div>
            <div class="label">涉及国家/地区</div>
        </div>
        <div class="stat-card">
            <div class="number">{len(report['by_isp']):,}</div>
            <div class="label">涉及 ISP</div>
        </div>
    </div>

    <div class="charts-row">
        <div class="card">
            <h2>国家/地区分布 (Top 10)</h2>
            <div class="chart-container">
                <canvas id="pieChart"></canvas>
            </div>
        </div>
        <div class="card">
            <h2>攻击来源排行</h2>
            <div class="chart-container">
                <canvas id="barChart"></canvas>
            </div>
        </div>
    </div>

    <div class="card">
        <div class="tabs">
            <button class="tab active" onclick="switchTab('country')">按国家/地区</button>
            <button class="tab" onclick="switchTab('region')">按省/州</button>
            <button class="tab" onclick="switchTab('city')">按城市</button>
            <button class="tab" onclick="switchTab('isp')">按 ISP</button>
        </div>

        <div id="tab-country" class="tab-content active">
            <table>
                <thead><tr><th>#</th><th>国家/地区</th><th>数量</th><th>占比</th><th>分布</th></tr></thead>
                <tbody>{table_rows(countries)}</tbody>
            </table>
        </div>
        <div id="tab-region" class="tab-content">
            <table>
                <thead><tr><th>#</th><th>省/州</th><th>数量</th><th>占比</th><th>分布</th></tr></thead>
                <tbody>{table_rows(regions)}</tbody>
            </table>
        </div>
        <div id="tab-city" class="tab-content">
            <table>
                <thead><tr><th>#</th><th>城市</th><th>数量</th><th>占比</th><th>分布</th></tr></thead>
                <tbody>{table_rows(cities)}</tbody>
            </table>
        </div>
        <div id="tab-isp" class="tab-content">
            <table>
                <thead><tr><th>#</th><th>ISP</th><th>数量</th><th>占比</th><th>分布</th></tr></thead>
                <tbody>{table_rows(isps)}</tbody>
            </table>
        </div>
    </div>

    <div class="card">
        <h2>ISP 分布 (Top 15)</h2>
        <div class="chart-container" style="height: 400px;">
            <canvas id="ispChart"></canvas>
        </div>
    </div>
</div>

<script>
Chart.defaults.color = '#94a3b8';
Chart.defaults.borderColor = '#334155';

// 饼图
new Chart(document.getElementById('pieChart'), {{
    type: 'doughnut',
    data: {{
        labels: {json.dumps(pie_labels)},
        datasets: [{{
            data: {json.dumps(pie_values)},
            backgroundColor: {json.dumps(colors[:len(pie_labels)])},
            borderWidth: 0
        }}]
    }},
    options: {{
        responsive: true,
        plugins: {{
            legend: {{ position: 'right', labels: {{ padding: 12, font: {{ size: 11 }} }} }}
        }}
    }}
}});

// 柱状图 Top 15
new Chart(document.getElementById('barChart'), {{
    type: 'bar',
    data: {{
        labels: {json.dumps([c for c, _ in countries[:15]])},
        datasets: [{{
            label: '封禁 IP 数',
            data: {json.dumps([v for _, v in countries[:15]])},
            backgroundColor: '#3b82f6',
            borderRadius: 4
        }}]
    }},
    options: {{
        indexAxis: 'y',
        responsive: true,
        plugins: {{ legend: {{ display: false }} }},
        scales: {{
            x: {{ grid: {{ color: '#1e293b' }} }},
            y: {{ grid: {{ display: false }} }}
        }}
    }}
}});

// ISP 柱状图
new Chart(document.getElementById('ispChart'), {{
    type: 'bar',
    data: {{
        labels: {json.dumps([name[:30] for name, _ in isps[:15]])},
        datasets: [{{
            label: '封禁 IP 数',
            data: {json.dumps([v for _, v in isps[:15]])},
            backgroundColor: {json.dumps(colors[:15])},
            borderRadius: 4
        }}]
    }},
    options: {{
        responsive: true,
        maintainAspectRatio: false,
        plugins: {{ legend: {{ display: false }} }},
        scales: {{
            x: {{ grid: {{ display: false }}, ticks: {{ maxRotation: 45 }} }},
            y: {{ grid: {{ color: '#1e293b' }} }}
        }}
    }}
}});

function switchTab(name) {{
    document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(el => el.classList.remove('active'));
    document.getElementById('tab-' + name).classList.add('active');
    event.target.classList.add('active');
}}
</script>
</body>
</html>"""
    return html


def main():
    report = load_report()
    html = generate_html(report)
    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write(html)
    print(f"HTML 报告已生成: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
