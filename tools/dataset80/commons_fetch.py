#!/usr/bin/env python3
"""从 Wikimedia Commons 分类抓取图片到 models/dataset80/images/<class>/。

用法: commons_fetch.py <class> "<Category:Xxx>" [max_imgs]
"""
import hashlib
import io
import json
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import quote
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'models' / 'dataset80' / 'images'
API = 'https://commons.wikimedia.org/w/api.php'
UA = {'User-Agent': 'esp32-item-recognition-dataset/1.0 (research; contact: local)'}
THUMB_W = 512


def api(params):
    params = dict(params, format='json')
    url = API + '?' + '&'.join(f'{k}={quote(str(v), safe=":")}' for k, v in params.items())
    req = Request(url, headers=UA)
    for _ in range(4):
        try:
            with urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception:
            time.sleep(2)
    return {}


def list_files(category, depth=2):
    """抓取分类下的文件，并递归展开 1-2 层子分类（Commons 植物类多为子分类组织）。"""
    files, subs, cont = [], [], None
    while True:
        p = {'action': 'query', 'list': 'categorymembers', 'cmtitle': category,
             'cmtype': 'file|subcat', 'cmlimit': 500}
        if cont:
            p['cmcontinue'] = cont
        d = api(p)
        for m in d.get('query', {}).get('categorymembers', []):
            t = m['title']
            if t.startswith('Category:'):
                subs.append(t)
            elif t.lower().endswith(('.jpg', '.jpeg', '.png')):
                files.append(t)
        cont = d.get('continue', {}).get('cmcontinue')
        if not cont or len(files) > 1500:
            break
    if depth > 1:
        for s in subs:
            files += list_files(s, depth - 1)
            if len(files) > 1500:
                break
    return files


def thumb_url(title):
    d = api({'action': 'query', 'titles': title, 'prop': 'imageinfo',
             'iiprop': 'url|size|mime', 'iiurlwidth': THUMB_W})
    pages = d.get('query', {}).get('pages', {})
    for pg in pages.values():
        ii = pg.get('imageinfo', [{}])[0]
        u = ii.get('thumburl') or ii.get('url')
        if u and ii.get('mime', 'image/jpeg') in ('image/jpeg', 'image/png'):
            return u
    return None


def download(url, dst: Path):
    try:
        req = Request(url, headers=UA)
        with urlopen(req, timeout=40) as r:
            data = r.read()
        if len(data) < 8000:
            return 0
        with open(dst, 'wb') as f:
            f.write(data)
        return 1
    except Exception:
        return 0


def main():
    cls, category = sys.argv[1], sys.argv[2]
    maxn = int(sys.argv[3]) if len(sys.argv) > 3 else 600
    dst = OUT / cls
    dst.mkdir(parents=True, exist_ok=True)
    have = {p.name for p in dst.iterdir()}
    files = list_files(category)
    urls = []
    infos = []
    # 批量 imageinfo：每次 50 个标题
    for i in range(0, len(files), 50):
        chunk = files[i:i + 50]
        titles = '|'.join(chunk)
        d = api({'action': 'query', 'titles': titles, 'prop': 'imageinfo',
                 'iiprop': 'url|size|mime', 'iiurlwidth': THUMB_W})
        for t, pg in d.get('query', {}).get('pages', {}).items():
            if pg.get('missing') or 'imageinfo' not in pg:
                continue
            ii = pg['imageinfo'][0]
            if ii.get('mime') not in ('image/jpeg', 'image/png'):
                continue
            u = ii.get('thumburl') or ii.get('url')
            if u:
                infos.append(u)
    tasks = []
    for u in infos:
        name = hashlib.md5(u.encode()).hexdigest()[:16] + '.jpg'
        if name in have:
            continue
        tasks.append((u, dst / name))
        if len(tasks) >= maxn:
            break
    n = 0
    with ThreadPoolExecutor(max_workers=24) as ex:
        for ok in ex.map(lambda t: download(*t), tasks):
            n += ok
    print(f'{cls}: files_in_cat={len(files)} candidates={len(infos)} downloaded={n}/{len(tasks)}')


if __name__ == '__main__':
    main()
