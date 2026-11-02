#!/usr/bin/env python3.11
# -*- coding: utf-8 -*-
"""自学习闭环的标注端（items103 版）：Web 确认 UI + 实拍样本入库。

流程（设备固件无需任何改动）：
  1. 本服务跑在 PC 上:  python3.11 tools/items103/label_server.py
  2. 浏览器打开        http://localhost:8765
  3. 页面左侧是设备实时画面（/capture.jpg）与端侧当前判定（/result.json）
  4. 点「抓取当前帧」冻结一帧（/frame.rgb，即模型真实输入 320x240）
  5. 认对了点「确认」，认错了在下拉里选真值再点「改判保存」
     -> 该帧以真值类名（英文键）存进 models/items103/real_captures/<类>/
  6. 攒够一批后增量微调 + 量化 + 烧录（见 tools/items103/selflearn.sh）

与 items127 版的差异：类键为英文（apple...），端侧显示为中文名，
保存时自动把设备回传的中文名映射回英文键目录（train_items103.py 按英文键注入）。

为什么样本只进 train 不进 val：val 保持纯增强图，继续充当"几何链路是否对齐"的指标；
实拍帧进 val 会把这两个信号混在一起。见 train_items103.py 的 REAL 注入逻辑。
"""
import base64
import json
import time
import yaml
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REAL = ROOT / 'models' / 'items103' / 'real_captures'
MANIFEST = REAL / 'manifest.jsonl'
PORT = 8765

_spec = yaml.safe_load((ROOT / 'models' / 'items103.yaml').read_text())
CLASSES = _spec['groups']['items103']            # 英文键，训练目录名
ZH = _spec['zh_map']                              # en -> zh
ZH2EN = {v: k for k, v in ZH.items()}             # zh -> en（设备回传中文名映射回键名）

HTML = r"""<!doctype html><meta charset=utf-8>
<title>items103 自学习标注台</title>
<style>
 body{font-family:system-ui,sans-serif;background:#14161a;color:#e8e8e8;margin:0;padding:18px}
 h2{margin:0 0 10px;font-size:19px}
 .row{display:flex;gap:18px;flex-wrap:wrap}
 .col{background:#1d2127;border:1px solid #2c323b;border-radius:10px;padding:14px}
 img,canvas{width:340px;image-rendering:pixelated;border:1px solid #3a4150;border-radius:6px;background:#000}
 button{font-size:15px;padding:9px 16px;margin:4px 6px 4px 0;border:0;border-radius:7px;cursor:pointer}
 .ok{background:#2e7d4f;color:#fff}.bad{background:#8a4a2b;color:#fff}.neu{background:#39404b;color:#ddd}
 select{font-size:15px;padding:7px;background:#262b33;color:#eee;border:1px solid #3a4150;border-radius:6px;max-width:340px}
 input{font-size:15px;padding:7px;background:#262b33;color:#eee;border:1px solid #3a4150;border-radius:6px;width:170px}
 #dev{color:#9fd3a8;font-size:16px;margin:6px 0}
 #log{font-size:13px;color:#9aa4b2;max-height:150px;overflow:auto;white-space:pre-wrap}
 .tag{display:inline-block;padding:2px 8px;border-radius:5px;background:#2b313a;font-size:13px;margin-right:6px}
</style>
<h2>items103 自学习标注台 <span class=tag id=cnt></span></h2>
<div class=row>
 <div class=col>
   <b>设备实时画面</b>
   <div style="margin:8px 0">设备 IP <input id=ip value="192.168.8.194"></div>
   <img id=live src="">
   <div id=dev>连接中...</div>
 </div>
 <div class=col>
   <b>冻结帧（模型真实输入）</b>
   <div style="margin:8px 0">
     <button class=neu onclick="grab()">抓取当前帧</button>
     <button class=neu onclick="clearF()">丢弃</button>
   </div>
   <canvas id=cv width=320 height=240></canvas>
   <div id=fpred style="margin:6px 0;color:#c9d1d9">未抓帧</div>
   <div>
     <button class=ok id=bOk onclick="save(false)" disabled>确认：-</button>
     <button class=bad id=bBad onclick="document.getElementById('sel').style.display='block'" disabled>改判...</button>
   </div>
   <select id=sel style="display:none" onchange="save(true)"></select>
 </div>
 <div class=col style="min-width:260px">
   <b>已入库样本</b>
   <div id=log></div>
 </div>
</div>
<script>
const CLASSES=__CLASSES_JSON__;   // [[en,zh],...]
const ZH2EN=__ZH2EN_JSON__;       // 端侧中文名 -> 英文键
const $=id=>document.getElementById(id);
const sel=$('sel'); CLASSES.forEach(([en,zh])=>sel.add(new Option(`${zh} ${en}`,en)));
let frozen=null;   // {b64, label, pct, seq}
function ipBase(){return 'http://'+$('ip').value.trim();}
localStorage.ip && ($('ip').value=localStorage.ip);
$('ip').onchange=()=>localStorage.ip=$('ip').value;

setInterval(async()=>{
  try{
    $('live').src=ipBase()+'/capture.jpg?t='+Date.now();
    const j=await (await fetch(ipBase()+'/result.json')).json();
    $('dev').textContent=`#${j.seq}  ${j.label}  ${j.percent}%  ${j.width}x${j.height}`;
  }catch(e){$('dev').textContent='连不上设备: '+e;}
},1500);

async function grab(){
  const r=await fetch(ipBase()+'/frame.rgb');
  const buf=new Uint8Array(await r.arrayBuffer());
  const dec=s=>decodeURIComponent(s||'');
  frozen={buf, label:dec(r.headers.get('X-Frame-Label')),
          pct:r.headers.get('X-Frame-Pct'), seq:r.headers.get('X-Frame-Seq')};
  const ctx=$('cv').getContext('2d');
  const img=ctx.createImageData(320,240);
  for(let i=0,p=0;i<buf.length;i+=3,p+=4){
    img.data[p]=buf[i]; img.data[p+1]=buf[i+1]; img.data[p+2]=buf[i+2]; img.data[p+3]=255;
  }
  ctx.putImageData(img,0,0);
  $('fpred').textContent=`端侧判定: ${frozen.label} ${frozen.pct}% (seq ${frozen.seq})`;
  $('bOk').disabled=$('bBad').disabled=false;
  $('bOk').textContent='确认：'+frozen.label;
}
function clearF(){frozen=null;$('fpred').textContent='未抓帧';
  $('bOk').disabled=$('bBad').disabled=true;sel.style.display='none';}

async function save(corrected){
  if(!frozen)return;
  // 确认走端侧中文名 -> 映射回英文键；改判直接用下拉的英文键
  const label=corrected?sel.value:(ZH2EN[frozen.label]||frozen.label);
  let bin='';const b=frozen.buf;
  for(let i=0;i<b.length;i+=0x8000)bin+=String.fromCharCode.apply(null,b.subarray(i,i+0x8000));
  const body={rgb:btoa(bin),w:320,h:240,label,
              device_label:frozen.label,device_pct:frozen.pct,seq:frozen.seq,
              corrected:!!corrected};
  const r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},
                              body:JSON.stringify(body)});
  const j=await r.json();
  log(`${j.ok?'✓':'✗'} ${label}  <- 端侧原判 ${frozen.label}${corrected?' (改判)':''}  -> ${j.path||j.err}`);
  clearF(); refresh();
}
async function refresh(){
  const j=await (await fetch('/stats')).json();
  $('cnt').textContent=`共 ${j.total} 帧 / ${j.classes} 类`;
  $('log').textContent=Object.entries(j.per_class).map(([k,v])=>`${v}  ${k}`).join('\n')||'(空)';
}
function log(s){const e=$('log');e.textContent=s+'\n'+e.textContent;}
refresh();
</script>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype='application/json; charset=utf-8'):
        data = body.encode('utf-8') if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self._send(200, '', 'text/plain')

    def do_GET(self):
        if self.path == '/':
            page = (HTML.replace('__CLASSES_JSON__', json.dumps([[c, ZH[c]] for c in CLASSES], ensure_ascii=False))
                        .replace('__ZH2EN_JSON__', json.dumps(ZH2EN, ensure_ascii=False)))
            self._send(200, page, 'text/html; charset=utf-8')
        elif self.path == '/classes':
            self._send(200, json.dumps(CLASSES, ensure_ascii=False))
        elif self.path == '/stats':
            per = {}
            if REAL.exists():
                for d in sorted(p for p in REAL.iterdir() if p.is_dir()):
                    n = len(list(d.glob('*.png')) + list(d.glob('*.jpg')))
                    if n:
                        per[d.name] = n
            self._send(200, json.dumps({'total': sum(per.values()),
                                        'classes': len(per), 'per_class': per},
                                       ensure_ascii=False))
        else:
            self._send(404, '{"err":"not found"}')

    def do_POST(self):
        if self.path != '/save':
            return self._send(404, '{"err":"not found"}')
        try:
            n = int(self.headers.get('Content-Length', 0))
            req = json.loads(self.rfile.read(n).decode('utf-8'))
            label = req['label'].strip().replace('/', '_')
            if label not in CLASSES:
                return self._send(400, json.dumps({'ok': False, 'err': f'未知类别: {label}'},
                                                  ensure_ascii=False))
            from PIL import Image
            raw = base64.b64decode(req['rgb'])
            w, h = int(req['w']), int(req['h'])
            assert len(raw) == w * h * 3, f'字节数 {len(raw)} != {w*h*3}'
            d = REAL / label
            d.mkdir(parents=True, exist_ok=True)
            ts = time.strftime('%Y%m%d_%H%M%S')
            name = f'real_{ts}_s{req.get("seq","?")}.png'
            Image.frombytes('RGB', (w, h), raw).save(d / name)
            MANIFEST.parent.mkdir(parents=True, exist_ok=True)
            with MANIFEST.open('a', encoding='utf-8') as f:
                f.write(json.dumps({'file': f'{label}/{name}', 'ts': ts,
                                    'truth': label,
                                    'device_label': req.get('device_label'),
                                    'device_pct': req.get('device_pct'),
                                    'corrected': req.get('corrected', False)},
                                   ensure_ascii=False) + '\n')
            self._send(200, json.dumps({'ok': True, 'path': f'{label}/{name}'},
                                       ensure_ascii=False))
        except Exception as e:
            self._send(500, json.dumps({'ok': False, 'err': str(e)}, ensure_ascii=False))


if __name__ == '__main__':
    REAL.mkdir(parents=True, exist_ok=True)
    print(f'标注台: http://localhost:{PORT}/   (设备 IP 在页面里填)')
    print(f'样本目录: {REAL}')
    ThreadingHTTPServer(('0.0.0.0', PORT), H).serve_forever()
