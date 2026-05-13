#!/usr/bin/env python3
"""
Mould HTTP 网关计数器端点压测工具 — 多线程并发 GET /api/v1/counter。

测试目的: 测量 muduo HTTP 服务器在极简 handler 下的纯并发性能（QPS、延迟分布）。

用法:
  # 默认压测（8 线程, 30 秒）
  python3 bench_counter.py http://127.0.0.1:8080

  # 自定义参数
  python3 bench_counter.py http://127.0.0.1:8080 --threads 16 --duration 60

  # 单次请求
  python3 bench_counter.py http://127.0.0.1:8080 --single
"""

import argparse
import http.client
import json
import signal
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from typing import Optional

# ============================================================
# 常量
# ============================================================

COUNTER_PATH = "/api/v1/counter"


# ============================================================
# 统计
# ============================================================

@dataclass
class ThreadStats:
    sent: int = 0
    success: int = 0
    failed: int = 0
    latencies: list[float] = field(default_factory=list)
    status_hist: Counter = field(default_factory=Counter)


@dataclass
class AggregateStats:
    duration: float = 0.0
    total_sent: int = 0
    total_success: int = 0
    total_failed: int = 0
    throughput: float = 0.0
    p50: float = 0.0
    p90: float = 0.0
    p95: float = 0.0
    p99: float = 0.0
    min_lat: float = 0.0
    max_lat: float = 0.0
    avg_lat: float = 0.0
    status_hist: Counter = field(default_factory=Counter)


def percentile(sorted_data: list[float], p: float) -> float:
    if not sorted_data:
        return 0.0
    n = len(sorted_data)
    k = (p / 100.0) * (n - 1)
    f = int(k)
    c = k - f
    if f + 1 < n:
        return sorted_data[f] * (1 - c) + sorted_data[f + 1] * c
    return sorted_data[-1]


def aggregate(stats_list: list[ThreadStats], duration: float) -> AggregateStats:
    agg = AggregateStats(duration=duration)
    all_lat = []
    for s in stats_list:
        agg.total_sent += s.sent
        agg.total_success += s.success
        agg.total_failed += s.failed
        agg.status_hist.update(s.status_hist)
        all_lat.extend(s.latencies)
    agg.throughput = agg.total_sent / duration if duration > 0 else 0.0
    if all_lat:
        all_lat.sort()
        agg.min_lat = all_lat[0]
        agg.max_lat = all_lat[-1]
        agg.avg_lat = sum(all_lat) / len(all_lat)
        agg.p50 = percentile(all_lat, 50)
        agg.p90 = percentile(all_lat, 90)
        agg.p95 = percentile(all_lat, 95)
        agg.p99 = percentile(all_lat, 99)
    return agg


def print_report(agg: AggregateStats):
    hist_lines = []
    if agg.status_hist:
        for code, n in sorted(agg.status_hist.items(), key=lambda x: (-x[1], x[0])):
            pct = n / agg.total_sent * 100 if agg.total_sent > 0 else 0.0
            label = "tcp/http异常" if code < 0 else f"HTTP {code}"
            hist_lines.append(f"    {label}: {n} ({pct:.1f}%)")
    hist_block = "\n".join(hist_lines) if hist_lines else "    (无)"
    report = f"""
{'=' * 50}
  Muduo 计数器端点压测报告
{'=' * 50}
  测试时长:       {agg.duration:.1f} 秒
  总请求数:       {agg.total_sent}
  成功:           {agg.total_success}
  失败:           {agg.total_failed}
  吞吐量:         {agg.throughput:.1f} req/s

  HTTP 状态分布:
{hist_block}

  延迟统计 (ms):
    min:    {agg.min_lat:.2f}
    p50:    {agg.p50:.2f}
    p90:    {agg.p90:.2f}
    p95:    {agg.p95:.2f}
    p99:    {agg.p99:.2f}
    max:    {agg.max_lat:.2f}
    avg:    {agg.avg_lat:.2f}
{'=' * 50}
"""
    print(report, file=sys.stderr)


# ============================================================
# 工作线程
# ============================================================

def worker(
    host: str,
    port: int,
    path: str,
    stop_event: threading.Event,
    stats: ThreadStats,
    thread_index: int,
    use_post: bool = False,
):
    """单个工作线程：持续发送 HTTP 请求直到收到停止信号。"""
    conn = http.client.HTTPConnection(host, port, timeout=30)
    period_report_interval = 5.0
    last_report = time.monotonic()
    period_sent = 0

    def report_progress(force=False):
        nonlocal period_sent, last_report
        now = time.monotonic()
        if force or (now - last_report >= period_report_interval):
            elapsed = now - last_report if not force else 1.0
            rate = period_sent / elapsed if elapsed > 0 else 0.0
            print(f"  [t{thread_index:02d}] {rate:8.1f} req/s  "
                  f"suc={stats.success} fail={stats.failed} total={stats.sent}",
                  file=sys.stderr)
            period_sent = 0
            last_report = now

    try:
        while not stop_event.is_set():
            try:
                start = time.perf_counter()
                if use_post:
                    conn.request("POST", path)
                else:
                    conn.request("GET", path)
                resp = conn.getresponse()
                resp.read()
                elapsed = (time.perf_counter() - start) * 1000

                stats.sent += 1
                stats.latencies.append(elapsed)
                stats.status_hist[resp.status] += 1
                if resp.status == 200:
                    stats.success += 1
                else:
                    stats.failed += 1
                period_sent += 1
            except (ConnectionRefusedError,
                    http.client.HTTPException,
                    OSError) as e:
                if stop_event.is_set():
                    break
                stats.failed += 1
                stats.sent += 1
                stats.status_hist[-1] += 1
                period_sent += 1
                try:
                    conn.close()
                except Exception:
                    pass
                conn = http.client.HTTPConnection(host, port, timeout=30)

            report_progress()
    finally:
        conn.close()
        report_progress(force=True)


# ============================================================
# 单次请求
# ============================================================

def single_request(url: str, use_post: bool = False) -> int:
    raw = url
    if raw.startswith("http://"):
        raw = raw[len("http://"):]
    elif raw.startswith("https://"):
        raw = raw[len("https://"):]
    host_port, _, _ = raw.partition("/")
    if ":" in host_port:
        host, port_str = host_port.split(":", 1)
        port = int(port_str)
    else:
        host = host_port
        port = 80

    try:
        conn = http.client.HTTPConnection(host, port, timeout=10)
        start = time.perf_counter()
        if use_post:
            conn.request("POST", COUNTER_PATH)
        else:
            conn.request("GET", COUNTER_PATH)
        resp = conn.getresponse()
        body = resp.read()
        elapsed = (time.perf_counter() - start) * 1000
        conn.close()

        data = json.loads(body) if body else {}
        print(f"响应: HTTP {resp.status} | 耗时: {elapsed:.1f}ms")
        print(f"响应体: {json.dumps(data, indent=2, ensure_ascii=False)}")
        print(f"计数器值: {data.get('image_id', 'N/A')}")
        return 0 if resp.status == 200 else 1
    except Exception as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1


# ============================================================
# 压测入口
# ============================================================

def run_benchmark(host: str, port: int, threads: int, duration: int,
                  use_post: bool = False) -> int:
    print(f"目标: {host}:{port}{COUNTER_PATH} ({'POST' if use_post else 'GET'})",
          file=sys.stderr)
    print(f"线程数: {threads}, 测试时长: {duration}s", file=sys.stderr)
    print(file=sys.stderr)

    stop_event = threading.Event()
    stats_list = [ThreadStats() for _ in range(threads)]

    print(f"启动 {threads} 个线程，压测 {duration} 秒...", file=sys.stderr)
    start_time = time.monotonic()

    threads_handle = []
    for i in range(threads):
        t = threading.Thread(
            target=worker,
            args=(host, port, COUNTER_PATH, stop_event, stats_list[i], i, use_post),
            daemon=True,
        )
        t.start()
        threads_handle.append(t)

    try:
        stop_event.wait(duration)
    except KeyboardInterrupt:
        print("\n用户中断", file=sys.stderr)

    stop_event.set()
    elapsed = time.monotonic() - start_time

    print("\n正在等待线程退出...", file=sys.stderr)
    for t in threads_handle:
        t.join(timeout=10)

    agg = aggregate(stats_list, elapsed)
    print_report(agg)
    return 0 if agg.total_failed == 0 else 1


# ============================================================
# CLI
# ============================================================

def main():
    ap = argparse.ArgumentParser(
        description="Muduo HTTP 服务器计数器端点压测工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python3 bench_counter.py http://127.0.0.1:8080
  python3 bench_counter.py http://127.0.0.1:8080 -t 16 -d 60
  python3 bench_counter.py http://127.0.0.1:8080 --single
  python3 bench_counter.py http://127.0.0.1:8080 --post
        """,
    )

    ap.add_argument("url", help="服务器地址, 如 http://127.0.0.1:8080")
    ap.add_argument("--threads", "-t", type=int, default=8,
                    help="并发线程数 (默认: 8)")
    ap.add_argument("--duration", "-d", type=int, default=30,
                    help="压测持续时间秒数 (默认: 30)")
    ap.add_argument("--single", action="store_true",
                    help="单次请求模式")
    ap.add_argument("--post", action="store_true",
                    help="使用 POST 方法 (默认: GET)")

    args = ap.parse_args()

    # 解析 URL
    raw = args.url
    if raw.startswith("http://"):
        raw = raw[len("http://"):]
    elif raw.startswith("https://"):
        raw = raw[len("https://"):]
    host_port, _, _ = raw.partition("/")
    if ":" in host_port:
        host, port_str = host_port.split(":", 1)
        port = int(port_str)
    else:
        host = host_port
        port = 80

    if args.single:
        return single_request(args.url, args.post)

    return run_benchmark(host, port, args.threads, args.duration, args.post)


if __name__ == "__main__":
    sys.exit(main())
