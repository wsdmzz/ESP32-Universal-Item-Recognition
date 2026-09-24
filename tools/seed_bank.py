#!/usr/bin/env python3
"""
PC 端题库批量注入工具：并发调用 scnet Qwen3.8-Flash 生成初中+选择题，
去重后写出设备可读的 JSONL 题库文件（每行一题），供打包进 SPIFFS 分区。

这样设备端几乎不需要为"填库"发 AI 请求（设备只在消耗后做小规模补货）。

用法：
    python3 tools/seed_bank.py --target 10000 --workers 6
    # 生成 spiffs_data/queue.jsonl 后：
    idf.py build
    # 单独烧录 storage 分区（不碰 app，保留设备其他数据）：
    python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
        --before default_reset --after hard_reset \
        write_flash 0x190000 build/storage.bin

API Key 默认从 sdkconfig 的 CONFIG_QWEN_API_KEY 读取，可用 --key 覆盖。
"""
import argparse
import json
import os
import re
import ssl
import sys
import threading
import time
import urllib.request

API_URL = "https://api.scnet.cn/api/llm/v1/chat/completions"
MODEL = "Qwen3.8-Flash"

SUBJECT_MAP = {"数学": 0, "语文": 1, "英语": 2, "科学": 3, "历史": 4}
DIFF_MAP = {"easy": 0, "medium": 1, "hard": 2, "简单": 0, "中等": 1, "困难": 2}

PROMPT_TMPL = (
    "出{count}道适合初中生及以上水平的选择题（可含高中基础题），学科要求："
    "按分布混合：数学约10%、英语约10%、语文约20%、"
    "科学约30%（以物理通识为主，如力/光/电/热/能量）、"
    "历史约30%（中国史与世界史常识，如朝代/四大发明/重大事件与年代）。要求："
    "1.每题4个选项，有且仅有一个正确答案；"
    "2.题目多样不重复，难度以初中为主、可含高中基础（中等与困难搭配）；"
    "3.多为知识性、通识性题目，数学计算题只占少数；"
    "4.只返回JSON对象，格式："
    '{{"questions":[{{"question":"题干","options":["选1","选2","选3","选4"],'
    '"correct":0,"subject":"数学","difficulty":"medium"}}]}}。'
    "correct是正确选项的下标(0-3)。"
)

lock = threading.Lock()
seen = set()          # 题干去重
results = []          # 设备 JSONL 记录 dict 列表
stats = {"requests": 0, "failed": 0, "dups": 0, "bad": 0}
stop_flag = threading.Event()


def read_key_from_sdkconfig(root):
    p = os.path.join(root, "sdkconfig")
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            m = re.search(r'CONFIG_QWEN_API_KEY="([^"]+)"', f.read())
            return m.group(1) if m else None
    except OSError:
        return None


def fetch_batch(key, batch_size, timeout):
    body = json.dumps({
        "model": MODEL,
        "messages": [
            {"role": "system",
             "content": "你是中小学出题AI，只输出JSON，不输出其他内容。"},
            {"role": "user", "content": PROMPT_TMPL.format(count=batch_size)},
        ],
        "temperature": 0.9,
        "max_tokens": 8000,
        "response_format": {"type": "json_object"},
    }).encode("utf-8")
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={
        "Authorization": "Bearer " + key,
        "Content-Type": "application/json",
    })
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        d = json.loads(r.read().decode("utf-8"))
    content = d["choices"][0]["message"].get("content") or ""
    return json.loads(content).get("questions", [])


def normalize(q):
    """校验并转成设备端 quiz_store 的 JSONL 记录格式"""
    text = (q.get("question") or "").strip()
    opts = [str(o).strip() for o in (q.get("options") or [])]
    if not text or len(opts) < 2:
        return None
    if len(opts) > 4:
        opts = opts[:4]
    try:
        correct = int(q.get("correct", 0))
    except (TypeError, ValueError):
        correct = 0
    correct = max(0, min(correct, len(opts) - 1))
    subj = SUBJECT_MAP.get((q.get("subject") or "").strip(), 3)
    diff_raw = (q.get("difficulty") or "medium").strip().lower()
    diff = next((v for k, v in DIFF_MAP.items() if k in diff_raw), 1)
    # 字段截断与设备端 quiz_types.h 保持一致：题干191B，选项47B（UTF-8）
    def clip(s, nbytes):
        b = s.encode("utf-8")[:nbytes]
        return b.decode("utf-8", "ignore")
    return {
        "question": clip(text, 191),
        "options": [clip(o, 47) for o in opts],
        "correct": correct,
        "subject": subj,
        "difficulty": diff,
        "ai": True,
    }


def worker(key, batch_size, timeout):
    while not stop_flag.is_set():
        with lock:
            if len(results) >= args_target:
                return
        try:
            qs = fetch_batch(key, batch_size, timeout)
        except Exception as e:
            with lock:
                stats["requests"] += 1
                stats["failed"] += 1
            print(f"  [warn] request failed: {e}", file=sys.stderr)
            time.sleep(3)
            continue
        added = 0
        with lock:
            stats["requests"] += 1
            for raw in qs:
                rec = normalize(raw)
                if rec is None:
                    stats["bad"] += 1
                    continue
                if rec["question"] in seen:
                    stats["dups"] += 1
                    continue
                seen.add(rec["question"])
                rec["id"] = len(results) + 1
                results.append(rec)
                added += 1
                if len(results) >= args_target:
                    stop_flag.set()
                    break
        print(f"  +{added:3d} (total {len(results)}/{args_target}, "
              f"req {stats['requests']})")


def main():
    global args_target
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", type=int, default=10000)
    ap.add_argument("--batch", type=int, default=50, help="每次请求的题数")
    ap.add_argument("--workers", type=int, default=6, help="并发请求数")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--key", default=None)
    ap.add_argument("--out", default=os.path.join(root, "spiffs_data", "queue.jsonl"))
    args = ap.parse_args()
    args_target = args.target

    key = args.key or read_key_from_sdkconfig(root)
    if not key:
        sys.exit("找不到 API Key：--key 传入或在 sdkconfig 配置 CONFIG_QWEN_API_KEY")

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(key, args.batch, args.timeout),
                                daemon=True) for _ in range(args.workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        for rec in results[:args.target]:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    dt = time.time() - t0
    print(f"\n完成：{min(len(results), args.target)} 题 -> {args.out}")
    print(f"请求 {stats['requests']} 次（失败 {stats['failed']}），"
          f"重复丢弃 {stats['dups']}，无效 {stats['bad']}，耗时 {dt/60:.1f} 分钟")
    print("下一步：idf.py build 后单独烧录 storage 分区：")
    print("  python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \\\n"
          "      --before default_reset --after hard_reset \\\n"
          "      write_flash 0x190000 build/storage.bin")


if __name__ == "__main__":
    main()
