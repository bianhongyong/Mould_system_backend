#!/usr/bin/env python3
"""
HTTP 网关性能压测工具 — 多线程并发向 HttpGatewayModule 发送图片。

用法:
  # 压测模式：多线程并发循环发送
  python3 send_image.py http://127.0.0.1:8080 /path/to/images --threads 8 --duration 60

  # 单次发送（缺省路径 /api/v1/frames/upload）
  python3 send_image.py http://127.0.0.1:8080 /path/to/image.jpg

  # 健康检查
  python3 send_image.py http://127.0.0.1:8080 --health
"""

import argparse
import http.client
import json
import mimetypes
import os
import signal
import sys
import threading
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ============================================================
# 图片发现
# ============================================================

IMAGE_EXTENSIONS = {'.jpg', '.jpeg', '.png', '.bmp', '.gif', '.webp', '.tiff', '.tif'}

# 路由注册的图片上传路径，与 HttpGatewayModule::DoInit() 中注册的路径一致
DEFAULT_UPLOAD_PATH = "/api/v1/frames/upload"


def find_images(root: str) -> list[str]:
    """递归查找文件夹下所有图片文件。"""
    root_path = Path(root)
    if root_path.is_file():
        return [str(root_path)]
    images = []
    for f in root_path.rglob('*'):
        if f.is_file() and f.suffix.lower() in IMAGE_EXTENSIONS:
            images.append(str(f))
    return sorted(images)


# ============================================================
# 请求体构建 — 图片二进制作为 body，元数据通过 HTTP Headers
# ============================================================

def detect_mime(path: str) -> str:
    """根据文件扩展名推断 MIME 类型。"""
    mime, _ = mimetypes.guess_type(path)
    if mime:
        return mime
    ext = Path(path).suffix.lower()
    return {
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".png": "image/png",
        ".bmp": "image/bmp",
    }.get(ext, "application/octet-stream")


def build_headers(
    content_type: str,
    node_id: str,
    capture_time: str = "",
    extra_fields: dict[str, str] = None,
) -> dict[str, str]:
    """构建 HTTP Headers，元数据放在 X-* 头中。"""
    headers = {
        "Content-Type": content_type,
        "X-Node-Id": node_id,
    }
    if capture_time:
        headers["X-Capture-Time"] = capture_time
    for key, val in (extra_fields or {}).items():
        headers[f"X-Mould-{key}"] = val
    return headers


# ============================================================
# 统计
# ============================================================

@dataclass
class ThreadStats:
    sent: int = 0
    success: int = 0
    failed: int = 0
    total_bytes: int = 0
    latencies: list[float] = field(default_factory=list)
    # HTTP 状态码计数；键 -1 表示未拿到合法 HTTP 响应（连接/解析异常）
    status_hist: Counter = field(default_factory=Counter)


@dataclass
class AggregateStats:
    duration: float = 0.0
    total_sent: int = 0
    total_success: int = 0
    total_failed: int = 0
    total_bytes: int = 0
    throughput: float = 0.0
    bandwidth_mbps: float = 0.0
    p50: float = 0.0
    p90: float = 0.0
    p95: float = 0.0
    p99: float = 0.0
    min_lat: float = 0.0
    max_lat: float = 0.0
    avg_lat: float = 0.0
    status_hist: Counter = field(default_factory=Counter)


def percentile(sorted_data: list[float], p: float) -> float:
    """计算百分位值（线性插值）。"""
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
    """合并所有线程的统计结果。"""
    agg = AggregateStats(duration=duration)
    all_latencies = []
    for s in stats_list:
        agg.total_sent += s.sent
        agg.total_success += s.success
        agg.total_failed += s.failed
        agg.total_bytes += s.total_bytes
        agg.status_hist.update(s.status_hist)
        all_latencies.extend(s.latencies)
    agg.throughput = agg.total_sent / duration if duration > 0 else 0.0
    agg.bandwidth_mbps = (agg.total_bytes * 8 / 1_000_000) / duration if duration > 0 else 0.0
    if all_latencies:
        all_latencies.sort()
        agg.min_lat = all_latencies[0]
        agg.max_lat = all_latencies[-1]
        agg.avg_lat = sum(all_latencies) / len(all_latencies)
        agg.p50 = percentile(all_latencies, 50)
        agg.p90 = percentile(all_latencies, 90)
        agg.p95 = percentile(all_latencies, 95)
        agg.p99 = percentile(all_latencies, 99)
    return agg


def print_report(agg: AggregateStats):
    """打印格式化的压测报告。"""
    total_gb = agg.total_bytes / (1024 ** 3)
    hist_lines = []
    if agg.status_hist:
        for code, n in sorted(agg.status_hist.items(), key=lambda x: (-x[1], x[0])):
            pct = n / agg.total_sent * 100 if agg.total_sent > 0 else 0.0
            label = "tcp/http异常" if code < 0 else f"HTTP {code}"
            hist_lines.append(f"    {label}: {n} ({pct:.1f}%)")
    hist_block = "\n".join(hist_lines) if hist_lines else "    (无)"
    report = f"""
{'=' * 50}
  Mould HTTP 网关压测报告
{'=' * 50}
  测试时长:       {agg.duration:.1f} 秒
  总请求数:       {agg.total_sent}
  成功:           {agg.total_success}
  失败:           {agg.total_failed}
  总数据量:       {total_gb:.2f} GB ({agg.total_bytes / (1024 ** 2):.1f} MB)
  吞吐量:         {agg.throughput:.1f} req/s
  带宽:           {agg.bandwidth_mbps:.1f} Mbps

  HTTP 状态分布 (失败多为 429 时表示 Muduo 按客户端 IP 限流):
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
    url: str,
    upload_path: str,
    image_bodies: list[tuple[str, bytes, str]],
    node_id_prefix: str,
    stop_event: threading.Event,
    stats: ThreadStats,
    thread_index: int,
    rate: float = 0,
    capture_time: str = "",
    extra_fields: dict[str, str] = None,
):
    """单个工作线程：循环发送图片直到停止。"""
    # 解析 URL
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

    node_id = f"{node_id_prefix}-{thread_index}"
    extra = extra_fields or {}
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
        last_request_time = 0.0
        while not stop_event.is_set():
            for img_path, body, mime in image_bodies:
                if stop_event.is_set():
                    break
                # 速率限制
                if rate > 0:
                    now = time.monotonic()
                    target_interval = 1.0 / rate
                    elapsed = now - last_request_time if last_request_time > 0 else target_interval
                    if elapsed < target_interval:
                        time.sleep(target_interval - elapsed)
                    last_request_time = time.monotonic()
                try:
                    headers = build_headers(mime, node_id, capture_time, extra)
                    start = time.perf_counter()
                    conn.request("POST", upload_path, body=body, headers=headers)
                    resp = conn.getresponse()
                    resp.read()
                    elapsed = (time.perf_counter() - start) * 1000

                    stats.sent += 1
                    stats.total_bytes += len(body)
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
# 单次发送 / 健康检查（保持原有功能）
# ============================================================

def single_send(url: str, image_path: str, node_id: str, capture_time: str,
                extra_fields: dict, verbose: bool) -> int:
    """单次发送一张图片（图片二进制作为 body，元数据在 header 中）。"""
    raw = url
    if raw.startswith("http://"):
        raw = raw[len("http://"):]
    elif raw.startswith("https://"):
        raw = raw[len("https://"):]
    host_port, _, path = raw.partition("/")
    path = f"/{path}" if path else DEFAULT_UPLOAD_PATH
    if ":" in host_port:
        host, port_str = host_port.split(":", 1)
        port = int(port_str)
    else:
        host = host_port
        port = 80

    mime = detect_mime(image_path)
    with open(image_path, "rb") as f:
        body = f.read()
    headers = build_headers(mime, node_id, capture_time, extra_fields)

    image_size = len(body)
    print(f"发送图片: {image_path} ({image_size} bytes)")
    print(f"  目标: {url}")

    try:
        conn = http.client.HTTPConnection(host, port, timeout=30)
        start = time.perf_counter()
        conn.request("POST", path, body=body, headers=headers)
        resp = conn.getresponse()
        resp_body = resp.read()
        elapsed = (time.perf_counter() - start) * 1000
        conn.close()

        data = json.loads(resp_body) if resp_body else {}
        print(f"响应: HTTP {resp.status} | 耗时: {elapsed:.1f}ms")
        if verbose or resp.status != 200:
            print(f"响应体: {data}")
        else:
            print(f"  请求ID: {data.get('request_id', 'N/A')}")
        return 0 if resp.status == 200 else 1
    except Exception as e:
        print(f"错误: {e}", file=sys.stderr)
        return 1


def health_check(url: str) -> int:
    """健康检查 GET /health。"""
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
        conn.request("GET", "/health")
        resp = conn.getresponse()
        body = resp.read()
        elapsed = (time.perf_counter() - start) * 1000
        conn.close()

        data = json.loads(body) if body else {}
        print(f"健康检查: {url}/health")
        print(f"状态: {resp.status} | 耗时: {elapsed:.1f}ms")
        print(f"响应: {data}")
        return 0 if resp.status == 200 else 1
    except Exception as e:
        print(f"健康检查失败: {e}", file=sys.stderr)
        return 1


# ============================================================
# 压测主函数
# ============================================================

def run_benchmark(url: str, images: list[str], threads: int, duration: int,
                  node_id_prefix: str, capture_time: str,
                  extra_fields: dict, rate: float = 0) -> int:
    """多线程压测入口。"""
    total_bytes = sum(os.path.getsize(p) for p in images)
    rate_str = f", 每线程限速: {rate} req/s" if rate > 0 else ""
    print(f"图片集: {len(images)} 个文件, {total_bytes / (1024**2):.1f} MB", file=sys.stderr)
    print(f"线程数: {threads}, 测试时长: {duration}s{rate_str}", file=sys.stderr)
    print(file=sys.stderr)

    raw_url = url
    if raw_url.startswith("http://"):
        raw_url = raw_url[len("http://"):]
    elif raw_url.startswith("https://"):
        raw_url = raw_url[len("https://"):]
    host_port, _, upload_path = raw_url.partition("/")
    upload_path = f"/{upload_path}" if upload_path else DEFAULT_UPLOAD_PATH

    # 预构建所有图片的请求体（仅读取原始字节，无 multipart 开销）
    print("预构建请求体...", file=sys.stderr)
    image_bodies: list[tuple[str, bytes, str]] = []  # (path, raw_bytes, mime)
    for img_path in images:
        mime = detect_mime(img_path)
        with open(img_path, "rb") as f:
            body = f.read()
        image_bodies.append((img_path, body, mime))

    # 启动线程
    stop_event = threading.Event()
    stats_list = [ThreadStats() for _ in range(threads)]

    print(f"启动 {threads} 个线程，压测 {duration} 秒...", file=sys.stderr)
    start_time = time.monotonic()

    threads_handle = []
    for i in range(threads):
        t = threading.Thread(
            target=worker,
            args=(url, upload_path, image_bodies, node_id_prefix,
                  stop_event, stats_list[i], i, rate),
            kwargs={"capture_time": capture_time, "extra_fields": extra_fields},
            daemon=True,
        )
        t.start()
        threads_handle.append(t)

    # 等待指定时长
    try:
        stop_event.wait(duration)
    except KeyboardInterrupt:
        print("\n用户中断", file=sys.stderr)

    # 停止所有线程
    stop_event.set()

    elapsed = time.monotonic() - start_time
    print("\n正在等待线程退出...", file=sys.stderr)
    for t in threads_handle:
        t.join(timeout=10)

    # 汇总报告
    agg = aggregate(stats_list, elapsed)
    print_report(agg)

    return 0 if agg.total_failed == 0 else 1


# ============================================================
# CLI
# ============================================================

def main():
    ap = argparse.ArgumentParser(
        description="Mould HTTP 网关压测工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
压测示例:
  python3 send_image.py http://127.0.0.1:8080 /data/images --threads 16 --duration 60
  python3 send_image.py http://127.0.0.1:8080 /data/images -t 8 -d 120

单次发送示例:
  python3 send_image.py http://127.0.0.1:8080 /tmp/test.jpg
  python3 send_image.py http://127.0.0.1:8080/api/v1/frames/upload /tmp/test.jpg

健康检查:
  python3 send_image.py http://127.0.0.1:8080 --health
        """,
    )

    # 位置参数
    ap.add_argument("url", help="服务器地址, 如 http://127.0.0.1:8080")
    ap.add_argument("target", nargs="?", default=None,
                    help="图片文件路径、文件夹路径 (不指定则用 --health)")

    # 压测选项
    ap.add_argument("--threads", "-t", type=int, default=4,
                    help="并发线程数 (默认: 4)")
    ap.add_argument("--duration", "-d", type=int, default=30,
                    help="压测持续时间秒数 (默认: 30)")
    ap.add_argument("--rate", "-r", type=float, default=0,
                    help="每线程每秒请求数上限 (默认: 0=不限制)")

    # 通用选项
    ap.add_argument("--node-id", default="",
                    help="采集节点 ID 前缀 (默认: bench-client)")
    ap.add_argument("--capture-time", default="",
                    help="ISO 8601 采集时间 (默认: 当前时间)")
    ap.add_argument("--field", "-F", action="append", default=[],
                    help="额外元数据键值对, 如 batch_id=B001 (可重复)")
    ap.add_argument("--health", action="store_true",
                    help="健康检查模式 (GET /health)")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="单次发送时显示详细响应信息")

    args = ap.parse_args()

    # 健康检查
    if args.health:
        return health_check(args.url)

    # 需要 target
    if not args.target:
        ap.print_help()
        print("\n错误: 请指定图片路径或使用 --health", file=sys.stderr)
        return 1

    # 填充默认值
    node_id_prefix = args.node_id or f"bench-client-{os.uname().nodename}"
    capture_time = args.capture_time or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    extra = {}
    for f in args.field:
        if "=" in f:
            k, v = f.split("=", 1)
            extra[k] = v
        else:
            print(f"警告: 忽略无效的 field 格式: {f}", file=sys.stderr)

    # 如果 target 是文件 → 单次发送
    if os.path.isfile(args.target):
        return single_send(
            args.url, args.target,
            node_id=node_id_prefix,
            capture_time=capture_time,
            extra_fields=extra,
            verbose=args.verbose,
        )

    # 否则 → 递归查找图片 → 压测
    images = find_images(args.target)
    if not images:
        print(f"错误: 在 '{args.target}' 下未找到图片文件", file=sys.stderr)
        return 1

    small_preview = images[:5]
    remainder = len(images) - len(small_preview)
    print(f"找到 {len(images)} 个图片文件:", file=sys.stderr)
    for p in small_preview:
        print(f"  {p}", file=sys.stderr)
    if remainder > 0:
        print(f"  ... 及其他 {remainder} 个文件", file=sys.stderr)
    print(file=sys.stderr)

    return run_benchmark(
        url=args.url,
        images=images,
        threads=args.threads,
        duration=args.duration,
        node_id_prefix=node_id_prefix,
        capture_time=capture_time,
        extra_fields=extra,
        rate=args.rate,
    )


if __name__ == "__main__":
    sys.exit(main())
