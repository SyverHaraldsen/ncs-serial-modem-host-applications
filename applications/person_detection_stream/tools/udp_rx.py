#!/usr/bin/env python3
#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

"""Receive the video UDP stream, reassemble frames and serve them as MJPEG.

Open http://<host>:8080/ to watch. Camera JPEGs are passed through untouched and the
person detection boxes are pushed to the browser as server-sent events (/boxes), which
draws them over the video, so the server never decodes or re-encodes a frame.
Throughput, loss, frame rate and lag per sender are printed every --interval seconds
and served as /stats.json.
"""

import argparse
import io
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from PIL import Image

HDR = struct.Struct(">HBBIIHBBHHII")
MAGIC = 0x5653
VERSION = 1
PKT_FRAME, PKT_DETECT = 1, 2
FMT_RGB565_BE, FMT_JPEG = 1, 3
FLAG_LAST = 0x01
DETECTION = struct.Struct(">hhhhH")
DETECTION_HEADER_SIZE = 2
FRAME_TIMEOUT_S = 2.0
# The device gets a new source port whenever it reopens its socket or the carrier NAT rebinds.
PEER_TIMEOUT_S = 30.0
# Detection runs behind the stream; the browser keeps showing the latest boxes this long.
BOX_HOLD_MS = 1000

PAGE = b"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Person detection</title>
<style>
:root{--bg:#0b0d10;--card:#14181d;--line:#232a32;--text:#e8edf2;--muted:#8a96a3;
 --ok:#00e676;--bad:#ff4d4f}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--text);
 font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
 display:flex;justify-content:center;padding:24px 16px}
main{width:100%;max-width:880px;display:flex;flex-direction:column;gap:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px}
h1{margin:0;font-size:20px;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:8px;padding:6px 12px;border-radius:999px;
 background:var(--card);border:1px solid var(--line);font-size:13px;font-weight:600;color:var(--bad)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--bad)}
.live{color:var(--ok)}
.live .dot{background:var(--ok);animation:pulse 1.6s infinite}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(0,230,118,.6)}70%,100%{box-shadow:0 0 0 8px rgba(0,230,118,0)}}
.video{position:relative;background:#000;border:1px solid var(--line);border-radius:14px;
 overflow:hidden;aspect-ratio:4/3}
.video img{width:100%;height:100%;object-fit:contain;display:block}
.video svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}
.video rect.box{fill:none;stroke:var(--ok);stroke-width:3}
.video rect.tag{fill:var(--ok)}
.video text{fill:#0b0d10;font-weight:650;dominant-baseline:hanging}
.empty{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
 color:var(--muted);font-size:14px}
table{width:100%;border-collapse:separate;border-spacing:0;background:var(--card);
 border:1px solid var(--line);border-radius:12px;overflow:hidden;font-variant-numeric:tabular-nums}
th,td{padding:10px 14px;text-align:right}
th:first-child,td:first-child{text-align:left}
th{font-size:12px;font-weight:600;color:var(--muted);text-transform:uppercase;letter-spacing:.6px;
 border-bottom:1px solid var(--line)}
td{font-size:15px}
tr+tr td{border-top:1px solid var(--line)}
td.sender{color:var(--muted);font-size:13px}
td.persons{color:var(--ok);font-weight:650}
.none td{color:var(--muted);text-align:center}
tbody tr.peer{cursor:pointer}
tbody tr.peer:hover td{background:#1a1f25}
tbody tr.current td{background:#18231e}
tbody tr.current td:first-child{box-shadow:inset 3px 0 0 var(--ok);color:var(--text)}
.hint{font-size:12px;color:var(--muted);margin:-8px 2px 0}
</style></head><body><main>
<header>
 <h1>Person detection</h1>
 <div id="status" class="pill"><span class="dot"></span><span id="status-text">No video</span></div>
</header>
<div class="video">
 <img id="img" alt="Video stream">
 <svg id="boxes" preserveAspectRatio="xMidYMid meet"></svg>
 <div id="empty" class="empty">No video yet</div>
</div>
<table>
 <thead><tr><th>Sender</th><th>FPS</th><th>Persons</th><th>kbit/s</th><th>Lag s</th></tr></thead>
 <tbody id="rows"><tr class="none"><td colspan="5">No senders</td></tr></tbody>
</table>
<div id="hint" class="hint" hidden></div>
</main>
<script>
const $=id=>document.getElementById(id);
let pinned=null, shown=null, events=null, boxes=[], boxesAt=0, streamUrl='';
const SVG='http://www.w3.org/2000/svg';
function el(tag,attrs){const e=document.createElementNS(SVG,tag);
 for(const [k,v] of Object.entries(attrs))e.setAttribute(k,v);return e}
/* The svg viewBox is the image's own pixel grid and "meet" matches object-fit:contain,
   so box coordinates need no scaling; only the label size follows the display. */
function draw(){
 const img=$('img'), svg=$('boxes'), w=img.naturalWidth, h=img.naturalHeight;
 svg.replaceChildren();
 if(!w||!h||Date.now()-boxesAt>BOX_HOLD_MS)return;
 svg.setAttribute('viewBox',`0 0 ${w} ${h}`);
 const k=w/Math.min(img.clientWidth,img.clientHeight*w/h), fs=13*k, pad=4*k;
 for(const [x1,y1,x2,y2,score] of boxes){
  svg.append(el('rect',{class:'box',x:x1,y:y1,width:x2-x1,height:y2-y1,
   'vector-effect':'non-scaling-stroke'}));
  const label='person '+Math.floor(score/10)+'%', th=fs+2*pad, tw=label.length*fs*0.6+2*pad,
   ty=y1-th>=0?y1-th:y1;
  svg.append(el('rect',{class:'tag',x:x1,y:ty,width:tw,height:th}));
  const t=el('text',{x:x1+pad,y:ty+pad,'font-size':fs});t.textContent=label;svg.append(t)}
}
setInterval(draw,100);
$('img').addEventListener('load',()=>{$('empty').hidden=true});
/* One corrupt JPEG puts the img in an error state that later MJPEG parts do not clear. */
function reconnect(){$('img').src=streamUrl+(streamUrl.includes('?')?'&':'?')+'r='+Date.now()}
$('img').addEventListener('error',()=>setTimeout(reconnect,300));
function cell(text,cls){const td=document.createElement('td');td.textContent=text;
 if(cls)td.className=cls;return td}
function pick(peers){
 if(pinned&&peers[pinned])return pinned;
 let best=null;
 for(const [n,p] of Object.entries(peers)){
  if(p.last_frame_age_s===null)continue;
  if(!best||p.last_frame_age_s<peers[best].last_frame_age_s-1||
     (Math.abs(p.last_frame_age_s-peers[best].last_frame_age_s)<=1&&p.kbit_s>peers[best].kbit_s))best=n}
 return best;
}
function show(name){
 if(name===shown)return;
 shown=name;
 const q=name?'?peer='+encodeURIComponent(name):'';
 streamUrl='/stream.mjpg'+q;
 $('img').src=streamUrl;
 if(events)events.close();
 boxes=[];
 events=new EventSource('/boxes'+q);
 events.onmessage=e=>{boxes=JSON.parse(e.data);boxesAt=Date.now();draw()};
}
async function tick(){
 let s;
 try{s=await (await fetch('/stats.json',{cache:'no-store'})).json()}catch(e){s=null}
 const peers=s?s.peers||{}:{}, current=pick(peers);
 show(current);
 const age=current?peers[current].last_frame_age_s:null, live=age!==null&&age<3;
 $('status').className='pill'+(live?' live':'');
 $('status-text').textContent=live?'Live':'No video';
 $('empty').hidden=age!==null;
 const img=$('img');
 if(live&&img.complete&&img.naturalWidth===0)reconnect();
 const rows=$('rows'), names=Object.keys(peers);
 rows.replaceChildren();
 if(!names.length){const tr=document.createElement('tr');tr.className='none';
  const td=cell('No senders');td.colSpan=5;tr.append(td);rows.append(tr)}
 for(const n of names){const p=peers[n], tr=document.createElement('tr');
  tr.className='peer'+(n===current?' current':'');
  tr.title=n===pinned?'Pinned. Click to follow the newest sender again.':'Click to watch this sender';
  tr.append(cell((n===pinned?'\\u25CF ':'')+n,'sender'),cell(p.fps.toFixed(1)),
   cell(p.persons,'persons'),cell(Math.round(p.kbit_s)),
   cell(p.lag_s.toFixed(1)));
  tr.onclick=()=>{pinned=pinned===n?null:n;tick()};
  rows.append(tr)}
 $('hint').hidden=names.length<2;
 $('hint').textContent=pinned?'Pinned to '+pinned+'. Click it again to follow the newest sender.'
  :'Click a sender to watch it.';
}
show(null);tick();setInterval(tick,1000);
</script></body></html>""".replace(b"BOX_HOLD_MS", str(BOX_HOLD_MS).encode())


class Latest:
    """Latest frame and statistics per sender, named "<ip>:<port>"."""

    def __init__(self):
        self.cond = threading.Condition()
        self.frames = {}
        self.count = 0
        self.stats = {}
        self.boxes = {}
        self.box_count = 0

    def publish(self, name, jpeg):
        with self.cond:
            self.count += 1
            self.frames[name] = (jpeg, self.count, time.monotonic())
            self.cond.notify_all()

    def publish_boxes(self, name, boxes):
        with self.cond:
            self.box_count += 1
            self.boxes[name] = (boxes, self.box_count)
            self.cond.notify_all()

    def forget(self, name):
        with self.cond:
            self.frames.pop(name, None)
            self.stats.pop(name, None)
            self.boxes.pop(name, None)

    def newest(self):
        """Name of the sender with the newest frame, or empty."""
        if not self.frames:
            return ""
        return max(self.frames.items(), key=lambda f: f[1][1])[0]

    def latest_boxes(self, name):
        """Return (boxes, count) for one sender, or for the newest sender when name is empty."""
        return self.boxes.get(name or self.newest(), (None, -1))

    def frame(self, name):
        """Return (jpeg, count) for one sender, or for the newest frame when name is empty."""
        if name:
            jpeg, count, _ = self.frames.get(name, (None, -1, 0))
            return jpeg, count
        if not self.frames:
            return None, -1
        jpeg, count, _ = max(self.frames.values(), key=lambda f: f[1])
        return jpeg, count

    def snapshot(self):
        now = time.monotonic()
        with self.cond:
            ages = {n: round(now - t, 1) for n, (_, _, t) in self.frames.items()}
            stats = dict(self.stats)
        empty = {"kbit_s": 0.0, "fps": 0.0, "persons": 0}
        peers = {n: {**stats.get(n, empty), "last_frame_age_s": ages.get(n)}
                 for n in sorted(set(stats) | set(ages))}
        return {"peers": peers, "frames_total": self.count}


def parse_detections(payload):
    if len(payload) < DETECTION_HEADER_SIZE:
        return []
    count = min(payload[0], (len(payload) - DETECTION_HEADER_SIZE) // DETECTION.size)
    return [DETECTION.unpack_from(payload, DETECTION_HEADER_SIZE + i * DETECTION.size)
            for i in range(count)]


def rgb565_image(w, h, data):
    if len(data) != w * h * 2:
        return None
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        v = (data[2 * i] << 8) | data[2 * i + 1]
        rgb[3 * i] = (v >> 8) & 0xF8
        rgb[3 * i + 1] = (v >> 3) & 0xFC
        rgb[3 * i + 2] = (v << 3) & 0xF8
    return Image.frombytes("RGB", (w, h), bytes(rgb))


def to_jpeg(fmt, w, h, data):
    """Return a JPEG the browser can show, or None for a frame that is not one."""
    if fmt == FMT_JPEG:
        # A truncated or corrupt camera frame would stall the MJPEG stream; skip it.
        if data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
            return None
        return bytes(data)
    if fmt == FMT_RGB565_BE:
        img = rgb565_image(w, h, data)
        if img is None:
            return None
        out = io.BytesIO()
        img.save(out, "JPEG", quality=90)
        return out.getvalue()
    return None


class Frame:
    """A frame being reassembled; its size is known up front or from its last chunk."""

    def __init__(self, fmt, w, h, size, now):
        self.fmt, self.w, self.h = fmt, w, h
        self.buf = bytearray(size)
        self.total = size or None
        self.got = 0
        self.first_seen = now

    def add(self, offset, payload, last):
        end = offset + len(payload)
        if self.total is not None and end > self.total:
            return False
        if end > len(self.buf):
            self.buf.extend(bytes(end - len(self.buf)))
        self.buf[offset:end] = payload
        self.got += len(payload)
        if last:
            self.total = end
        return self.total is not None and self.got >= self.total


class Peer:
    def __init__(self, now):
        self.frames = {}
        self.persons = 0
        self.next_seq = None
        self.last_seen = now
        self.base_offset = None
        self.last_uptime_ms = 0
        self.lag = 0.0
        self.reset(now)

    def reset(self, now):
        self.window_start = now
        self.lag_max = 0.0
        self.bytes = self.received = self.lost = self.reordered = 0
        self.complete = self.incomplete = 0

    # The device clock is not synced to ours, so lag is the transit time above that of
    # the quickest datagram so far: 0 is live, growth means a queue on the way here.
    def on_lag(self, uptime_ms, now):
        offset = now - uptime_ms / 1000
        if self.base_offset is None or uptime_ms < self.last_uptime_ms:
            self.base_offset = offset
        self.base_offset = min(self.base_offset, offset)
        self.last_uptime_ms = uptime_ms
        self.lag = offset - self.base_offset
        self.lag_max = max(self.lag_max, self.lag)

    def on_datagram(self, seq, size, now):
        self.last_seen = now
        self.bytes += size
        self.received += 1
        if self.next_seq is None or seq >= self.next_seq:
            if self.next_seq is not None:
                self.lost += seq - self.next_seq
            self.next_seq = seq + 1
        else:
            # A late datagram was already counted as lost.
            self.reordered += 1
            self.lost -= 1

    def on_detections(self, payload):
        boxes = parse_detections(payload)
        self.persons = len(boxes)
        return boxes

    def on_chunk(self, frame_id, fmt, w, h, size, offset, payload, last, now):
        frame = self.frames.get(frame_id)
        if frame is None:
            frame = self.frames[frame_id] = Frame(fmt, w, h, size, now)
        if not frame.add(offset, payload, last):
            return None
        del self.frames[frame_id]
        self.complete += 1
        return frame

    def expire(self, now):
        for frame_id in [k for k, f in self.frames.items()
                         if now - f.first_seen > FRAME_TIMEOUT_S]:
            del self.frames[frame_id]
            self.incomplete += 1

    def report(self, now):
        elapsed = now - self.window_start
        expected = self.received + self.lost
        stats = {
            "kbit_s": round(self.bytes * 8 / elapsed / 1000, 1),
            "pkt_s": round(self.received / elapsed, 1),
            "loss_pct": round(100.0 * self.lost / expected, 1) if expected else 0.0,
            "fps": round(self.complete / elapsed, 2),
            "incomplete_frames": self.incomplete,
            "reordered": self.reordered,
            "persons": self.persons,
            "lag_s": round(self.lag, 2),
            "lag_max_s": round(self.lag_max, 2),
        }
        self.reset(now)
        return stats


def udp_loop(args, latest):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((args.bind, args.port))
    sock.settimeout(args.interval)
    print(f"Listening on {args.bind}:{args.port}", flush=True)

    peers = {}
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data, addr = None, None

        now = time.monotonic()
        if data is not None and len(data) >= HDR.size:
            (magic, version, ptype, seq, uptime_ms, frame_id, fmt, flags,
             w, h, size, offset) = HDR.unpack_from(data)
            if magic == MAGIC and version == VERSION:
                peer = peers.setdefault(addr, Peer(now))
                peer.on_datagram(seq, len(data), now)
                peer.on_lag(uptime_ms, now)
                name = f"{addr[0]}:{addr[1]}"
                if ptype == PKT_DETECT:
                    latest.publish_boxes(name, peer.on_detections(data[HDR.size:]))
                elif ptype == PKT_FRAME:
                    frame = peer.on_chunk(frame_id, fmt, w, h, size, offset,
                                          data[HDR.size:], flags & FLAG_LAST, now)
                    if frame is not None:
                        jpeg = to_jpeg(frame.fmt, frame.w, frame.h, frame.buf)
                        if jpeg:
                            latest.publish(name, jpeg)
                        else:
                            print(f"{name}  frame {frame_id} dropped: not a valid image",
                                  flush=True)

        for peer_addr in [a for a, p in peers.items() if now - p.last_seen > PEER_TIMEOUT_S]:
            del peers[peer_addr]
            latest.forget(f"{peer_addr[0]}:{peer_addr[1]}")

        for peer_addr, peer in peers.items():
            peer.expire(now)
            if now - peer.window_start >= args.interval:
                stats = peer.report(now)
                name = f"{peer_addr[0]}:{peer_addr[1]}"
                with latest.cond:
                    latest.stats[name] = stats
                print(f"{name}  {stats['kbit_s']:7.1f} kbit/s  {stats['pkt_s']:6.1f} pkt/s  "
                      f"loss {stats['loss_pct']:5.1f}%  {stats['fps']:5.2f} fps  "
                      f"incomplete {stats['incomplete_frames']}  "
                      f"persons {stats['persons']}  lag {stats['lag_s']:.2f} s "
                      f"(max {stats['lag_max_s']:.2f})", flush=True)


def make_handler(latest):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send_body(self, ctype, body):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            url = urlsplit(self.path)
            if url.path == "/":
                self.send_body("text/html; charset=utf-8", PAGE)
            elif url.path == "/stats.json":
                self.send_body("application/json", json.dumps(latest.snapshot()).encode())
            elif url.path == "/stream.mjpg":
                self.stream(parse_qs(url.query).get("peer", [""])[0])
            elif url.path == "/boxes":
                self.box_events(parse_qs(url.query).get("peer", [""])[0])
            else:
                self.send_error(404)

        def stream(self, name):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            seen = -1
            try:
                while True:
                    with latest.cond:
                        latest.cond.wait_for(lambda: latest.frame(name)[1] != seen, timeout=10)
                        jpeg, seen = latest.frame(name)
                    if jpeg is None:
                        continue
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                     b"Content-Length: " + str(len(jpeg)).encode() +
                                     b"\r\n\r\n" + jpeg + b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass

        def box_events(self, name):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            seen = -1
            try:
                while True:
                    with latest.cond:
                        latest.cond.wait_for(lambda: latest.latest_boxes(name)[1] != seen,
                                             timeout=10)
                        boxes, count = latest.latest_boxes(name)
                    # A comment keeps idle proxies from closing the connection.
                    if count == seen or boxes is None:
                        self.wfile.write(b": idle\n\n")
                    else:
                        seen = count
                        self.wfile.write(b"data: " + json.dumps(boxes).encode() + b"\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="UDP bind address")
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--interval", type=float, default=5.0, help="report period in seconds")
    args = parser.parse_args()

    latest = Latest()
    http = ThreadingHTTPServer(("0.0.0.0", args.http_port), make_handler(latest))
    http.daemon_threads = True
    threading.Thread(target=http.serve_forever, daemon=True).start()
    print(f"Viewer on http://0.0.0.0:{args.http_port}/", flush=True)

    udp_loop(args, latest)


if __name__ == "__main__":
    main()
