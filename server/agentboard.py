#!/usr/bin/env python3
"""agentboard — collect local coding-agent session info, serve JSON for the desk display.

Sources (all local files, read-only):
  Claude Code  ~/.claude/projects/<slug>/*.jsonl
  Codex        ~/.codex/sessions/YYYY/M/D/rollout-*.jsonl   (+ ChatGPT rate limits)
  pi           ~/.pi/agent/sessions/<slug>/*.jsonl
  ChatGPT app  ~/.codex/session_index.jsonl (best-effort thread names)

Usage:  python3 agentboard.py [config.json]
API:    GET /            HTML dashboard (debug/preview)
        GET /api/status  JSON snapshot for the ESP32 display
"""

import glob
import json
import os
import re
import socket
import subprocess
import sys
import termios
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HOME = Path.home()

# ---------------------------------------------------------------- config

DEFAULTS = {
    "port": 8899,
    "run_window_sec": 180,        # last write / live process  -> running
    "idle_window_sec": 3600,      # within an hour, no process -> idle
    "history_hours": 24,          # ignore sessions older than this
    "max_sessions_per_agent": 6,
    "max_sessions_total": 14,
    "chatgpt_threads": 4,
    "scan_interval_sec": 5,
    "claude_dir": str(HOME / ".claude" / "projects"),
    "codex_dir": str(HOME / ".codex" / "sessions"),
    "codex_index": str(HOME / ".codex" / "session_index.jsonl"),
    "pi_dir": str(HOME / ".pi" / "agent" / "sessions"),
}


def load_config(argv):
    cfg = dict(DEFAULTS)
    if len(argv) > 1:
        with open(argv[1]) as f:
            cfg.update(json.load(f))
    return cfg


CFG = load_config(sys.argv)

# ------------------------------------------------------- cost estimation
# USD per MTok, from Anthropic's published API pricing (estimate only).

PRICES = {
    "fable": (10.0, 50.0, 0.25),
    "opus-5-5": (4.0, 20.0, 0.20),
    "opus": (5.0, 25.0, 0.50),
    "sonnet-5-5": (2.0, 10.0, 0.20),
    "sonnet-5": (2.0, 10.0, 0.20),
    "sonnet": (3.0, 15.0, 0.30),
    "haiku-5-5": (0.10, 0.50, 0.02),
    "haiku": (1.0, 5.0, 0.10),
}


def cost_of(model, tin, tout, cread, cwrite):
    """Rough cost estimate for one usage record, or None if model unknown."""
    if not model:
        return None
    m = model.lower()
    for key, (pin, pout, pcr) in PRICES.items():
        if key in m:
            return (tin * pin + tout * pout + cread * pcr + cwrite * pin * 1.25) / 1e6
    return None


# ---------------------------------------------------------------- helpers

def iso_ts(s):
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


def project_from_cwd(cwd):
    if not cwd:
        return None
    base = os.path.basename(cwd.rstrip("/"))
    return base or "~"


def slug_to_project(slug):
    """'-Users-x-Documents-infra-iac-phizpay' -> 'phizpay' (fallback only)."""
    return (slug.replace("-", "/").rstrip("/").split("/") or ["?"])[-1]


def clean_title(text, limit=80):
    if not text:
        return None
    text = text.strip()
    if text.startswith(("<", "Caveat:", "[Request interrupted", "local-command", "[Context]")):
        return None
    text = re.sub(r"\[[^\]]*\]\([^)]*\)", " ", text)   # strip [@x](file://...) mentions
    text = re.sub(r"\s+", " ", text)
    return text[:limit] if text else None


def text_from_content(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [b.get("text", "") for b in content
                 if isinstance(b, dict)
                 and b.get("type") in ("text", "input_text", "output_text")]
        return " ".join(p for p in parts if p)
    return ""


# ------------------------------------------------------- incremental reads

class FileState:
    """Tracks how much of one session file we've already parsed."""

    __slots__ = ("offset", "meta")

    def __init__(self):
        self.offset = 0
        self.meta = {}          # per-agent session dict, updated in place


def iter_new_lines(path, st, max_first_read=32 * 1024 * 1024):
    """Yield dicts parsed from bytes appended since last call."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return
    if size < st.offset:                      # truncated / rotated: start over
        st.offset = 0
        st.meta = {}
    if st.offset == 0 and size > max_first_read:
        st.offset = size - 8 * 1024 * 1024    # huge file: only tail
    try:
        with open(path, "rb") as f:
            f.seek(st.offset)
            data = f.read()
    except OSError:
        return
    cut = data.rfind(b"\n")
    if cut < 0:
        return
    st.offset += cut + 1
    for raw in data[:cut].split(b"\n"):
        if not raw:
            continue
        try:
            yield json.loads(raw)
        except Exception:
            continue


# ------------------------------------------------------------- collectors

class Collector:
    quota_codex = {}   # latest ChatGPT rate_limits snapshot across codex files

    def __init__(self):
        self.files = {}            # path -> FileState
        self.today = None          # local midnight, for daily stats
        self._roll_day()

    def _roll_day(self):
        now = datetime.now()
        self.today = datetime(now.year, now.month, now.day).timestamp()

    def rollover_check(self):
        if datetime.now().timestamp() - self.today > 86400:
            self._roll_day()

    def _touch(self, path, agent):
        st = self.files.setdefault(str(path), FileState())
        if st.meta and not st.meta.get("agent"):
            st.meta["agent"] = agent
        return st

    # ---- Claude Code ----

    def scan_claude(self):
        root = Path(CFG["claude_dir"])
        if not root.is_dir():
            return []
        cutoff = time.time() - CFG["history_hours"] * 3600
        for slug_dir in sorted(root.iterdir()):
            if not slug_dir.is_dir():
                continue
            for p in slug_dir.glob("*.jsonl"):
                try:
                    if p.stat().st_mtime < cutoff:
                        continue
                except OSError:
                    continue
                st = self._touch(p, "claude")
                self._claude_file(p, slug_dir.name, st)
        return []

    def _claude_file(self, path, slug, st):
        m = st.meta
        for d in iter_new_lines(path, st):
            t = d.get("type")
            ts = iso_ts(d.get("timestamp", ""))
            if ts and (not m.get("started") or ts < m["started"]):
                m["started"] = ts
            if ts:
                m["last"] = ts
            if t == "summary":
                s = clean_title(d.get("summary"))
                if s and not m.get("summary"):
                    m["summary"] = s
            elif t == "assistant":
                msg = d.get("message", {})
                if msg.get("model"):
                    m["model"] = msg["model"]
                u = msg.get("usage") or {}
                tin = u.get("input_tokens", 0)
                tout = u.get("output_tokens", 0)
                cr = u.get("cache_read_input_tokens", 0)
                cw = u.get("cache_creation_input_tokens", 0)
                if isinstance(cw, dict):   # newer shape: ephemeral 5m/1h breakdown
                    cw = sum(v for v in cw.values() if isinstance(v, (int, float)))
                m["tokens"] = m.get("tokens", 0) + tin + tout + cr + cw
                if ts and ts >= self.today:
                    m["tok_today"] = m.get("tok_today", 0) + tin + tout + cr + cw
                    c = cost_of(m.get("model"), tin, tout, cr, cw)
                    m["cost_today"] = m.get("cost_today", 0.0) + (c or 0.0)
                m["turns"] = m.get("turns", 0) + 1
            elif t == "user":
                if not m.get("title"):
                    s = clean_title(text_from_content((d.get("message") or {}).get("content")))
                    if s:
                        m["title"] = s
                cwd = d.get("cwd")
                if cwd and not m.get("project"):
                    m["project"] = project_from_cwd(cwd)
        m.setdefault("agent", "claude")
        m.setdefault("project", slug_to_project(slug))
        m.setdefault("id", path.stem)

    # ---- Codex ----

    def scan_codex(self):
        root = Path(CFG["codex_dir"])
        if not root.is_dir():
            return []
        cutoff = time.time() - CFG["history_hours"] * 3600
        for p in root.glob("*/*/*/rollout-*.jsonl"):
            try:
                if p.stat().st_mtime < cutoff:
                    continue
            except OSError:
                continue
            st = self._touch(p, "codex")
            self._codex_file(p, st)
        return []

    def _codex_file(self, path, st):
        m = st.meta
        for d in iter_new_lines(path, st):
            t = d.get("type")
            ts = iso_ts(d.get("timestamp", ""))
            if ts and (not m.get("started") or ts < m["started"]):
                m["started"] = ts
            if ts:
                m["last"] = ts
            p = d.get("payload") or {}
            pt = p.get("type", "")
            if t == "session_meta":
                m["id"] = p.get("id") or path.stem
                if p.get("cwd"):
                    m["project"] = project_from_cwd(p["cwd"])
                origin = p.get("originator") or ""
                m["originator"] = origin or None
                # ChatGPT desktop app threads show up as codex rollouts
                m["agent"] = "chatgpt" if any(
                    k in origin.lower() for k in ("desktop", "chatgpt")) else "codex"
            elif t == "turn_context" and p.get("model"):
                m["model"] = p["model"]
            elif pt == "user_message" and not m.get("title"):
                s = clean_title(p.get("message"))
                if s:
                    m["title"] = s
            elif t == "response_item" and p.get("type") == "message" and p.get("role") == "user":
                if not m.get("title"):
                    s = clean_title(text_from_content(p.get("content")))
                    if s:
                        m["title"] = s
            elif pt == "token_count":
                info = p.get("info") or {}
                tot = info.get("total_token_usage") or {}
                if tot:
                    m["tokens"] = tot.get("input_tokens", 0) + tot.get("output_tokens", 0) \
                        + tot.get("cached_input_tokens", 0)
                rl = p.get("rate_limits")
                if rl and ts:
                    prev = Collector.quota_codex
                    if not prev or ts >= prev.get("ts", 0):
                        Collector.quota_codex = {"ts": ts, "data": rl}
        m.setdefault("agent", "codex")
        m.setdefault("project", m.get("project")
                     or ("ChatGPT" if m.get("agent") == "chatgpt" else "codex"))
        m.setdefault("id", path.stem)

    # ---- pi ----

    def scan_pi(self):
        root = Path(CFG["pi_dir"])
        if not root.is_dir():
            return []
        cutoff = time.time() - CFG["history_hours"] * 3600
        for slug_dir in sorted(root.iterdir()):
            if not slug_dir.is_dir():
                continue
            for p in slug_dir.glob("*.jsonl"):
                try:
                    if p.stat().st_mtime < cutoff:
                        continue
                except OSError:
                    continue
                st = self._touch(p, "pi")
                self._pi_file(p, slug_dir.name, st)
        return []

    def _pi_file(self, path, slug, st):
        m = st.meta
        for d in iter_new_lines(path, st):
            t = d.get("type")
            ts = iso_ts(d.get("timestamp", ""))
            if ts and (not m.get("started") or ts < m["started"]):
                m["started"] = ts
            if ts:
                m["last"] = ts
            if t == "session":
                m["id"] = d.get("id") or path.stem
                m["project"] = project_from_cwd(d.get("cwd"))
            elif t == "model_change" and d.get("modelId"):
                m["model"] = d["modelId"]
            elif t == "message":
                msg = d.get("message") or {}
                role = msg.get("role")
                if role == "user" and not m.get("title"):
                    s = clean_title(text_from_content(msg.get("content")))
                    if s:
                        m["title"] = s
                if role == "assistant":
                    m["turns"] = m.get("turns", 0) + 1
                u = msg.get("usage") or {}
                tin = u.get("input", 0) + u.get("cache_read", 0) + u.get("cache_write", 0)
                tout = u.get("output", 0)
                if tin or tout:
                    m["tokens"] = m.get("tokens", 0) + tin + tout
        m.setdefault("agent", "pi")
        m.setdefault("project", slug_to_project(slug))
        m.setdefault("id", path.stem)

    # ---- ChatGPT desktop app thread index (best effort) ----

    def scan_chatgpt_index(self):
        idx = Path(CFG["codex_index"])
        if not idx.is_file():
            return []
        cutoff = time.time() - CFG["history_hours"] * 3600
        rows = []
        try:
            for line in idx.read_text().splitlines():
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                ts = iso_ts(d.get("updated_at", ""))
                if ts and ts >= cutoff:
                    rows.append((ts, d.get("thread_name") or "", d.get("id", "")))
        except OSError:
            return []
        rows.sort(reverse=True)
        out = []
        for ts, name, sid in rows[:CFG["chatgpt_threads"]]:
            out.append({
                "agent": "chatgpt", "id": sid, "project": "ChatGPT",
                "title": clean_title(name) or "(untitled)",
                "state": "done", "last": int(ts), "age": int(time.time() - ts),
                "started": int(ts), "model": None, "turns": 0, "tokens": 0,
            })
        return out

    # ---- states ----

    def tag_states(self, proc_projects):
        now = time.time()
        run_w = CFG["run_window_sec"]
        idle_w = CFG["idle_window_sec"]
        cutoff = now - CFG["history_hours"] * 3600
        keep = []
        for st in self.files.values():
            m = st.meta
            if not m.get("agent") or m.get("agent") == "chatgpt":
                continue
            last = m.get("last") or m.get("started") or 0
            if last < cutoff:
                continue
            live = m.get("project") in proc_projects
            if live or now - last <= run_w:
                state = "running"
            elif now - last <= idle_w:
                state = "idle"
            else:
                state = "done"
            keep.append({
                "agent": m["agent"], "id": m.get("id", ""), "project": m.get("project") or "?",
                "title": m.get("summary") or m.get("title") or "(no title)",
                "state": state, "last": int(last), "age": int(now - last),
                "started": int(m.get("started") or last), "model": m.get("model"),
                "turns": m.get("turns", 0), "tokens": m.get("tokens", 0),
                "originator": m.get("originator"),
            })
        return keep


# ------------------------------------------------------------- processes

def classify_procs():
    """-> (counts, {cwd: agent}, app_flags)"""
    try:
        out = subprocess.run(["ps", "-axo", "pid=,comm=,args="],
                             capture_output=True, text=True, errors="replace",
                             timeout=10).stdout
    except Exception:
        return {}, {}, {}
    pids = {"claude": [], "codex": [], "pi": []}
    flags = {"chatgpt_app": False}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if len(parts) < 2:
            continue
        pid_s, comm = parts[0], parts[1]
        args = parts[2] if len(parts) > 2 else ""
        arg0 = args.split(" ", 1)[0] if args else ""
        # macOS truncates comm to 31 chars, so also look at argv[0]
        names = {os.path.basename(comm).lower(), os.path.basename(arg0).lower()}
        if "ChatGPT.app/Contents/MacOS/ChatGPT" in args or "chatgpt" in names:
            flags["chatgpt_app"] = True
        low_line = (comm + " " + args).lower()
        if any(x in low_line for x in ("crashpad", "helper", "(service)", "(renderer)")):
            continue
        agent = None
        if "claude" in names and "helper" not in comm.lower():
            agent = "claude"
        elif "codex" in names:
            agent = "codex"
        elif "pi" in names or ("pi-acp" in args and names & {"node", "python", "bun"}):
            agent = "pi"
        elif names & {"node", "bun"} and re.search(r"(^|[ /])claude([ /]|$)", args):
            agent = "claude"          # claude CLI running under a node launcher
        else:
            continue
        try:
            pids[agent].append(int(pid_s))
        except ValueError:
            continue
    cwd_map = {}
    for agent, plist in pids.items():
        for pid in plist[:12]:                       # cap lsof cost
            try:
                r = subprocess.run(["lsof", "-a", "-p", str(pid), "-d", "cwd", "-Fn"],
                                   capture_output=True, text=True, timeout=5)
                for l in r.stdout.splitlines():
                    if l.startswith("n/"):
                        cwd_map[l[1:]] = agent
                        break
            except Exception:
                continue
    counts = {k: len(v) for k, v in pids.items()}
    return counts, cwd_map, flags


# ---------------------------------------------------------------- serving

STATE_LOCK = threading.Lock()
SNAPSHOT = {"generated_at": 0, "shown": [], "agents": {}, "quota": {}, "procs": {}}


def build_snapshot(col: Collector):
    col.rollover_check()
    counts, cwd_map, flags = classify_procs()

    by_proj = {}                                    # project basename -> agents live there
    for cwd, agent in cwd_map.items():
        by_proj.setdefault(project_from_cwd(cwd), set()).add(agent)

    col.scan_claude()
    col.scan_codex()
    col.scan_pi()
    listed = col.tag_states(by_proj) + col.scan_chatgpt_index()

    for s in listed:
        s["live_agents"] = sorted(by_proj.get(s["project"], set()))

    order = {"running": 0, "idle": 1, "done": 2}
    listed.sort(key=lambda s: (order[s["state"]], -s["last"]))

    per_agent = {}
    for s in listed:
        per_agent.setdefault(s["agent"], []).append(s)
    shown = []
    for name, arr in per_agent.items():
        shown += arr[:CFG["max_sessions_per_agent"]]
    shown.sort(key=lambda s: (order[s["state"]], -s["last"]))
    shown = shown[:CFG["max_sessions_total"]]

    # ---- quota: codex (ChatGPT account) + claude local stats ----
    quota = {}
    q = Collector.quota_codex
    if q:
        d = q["data"]
        quota["codex"] = {
            "plan": d.get("plan_type"),
            "primary": d.get("primary"), "secondary": d.get("secondary"),
            "credits": d.get("credits"), "ts": q["ts"],
        }
    tok = cost = 0.0
    n_today = 0
    models = {}
    for st in col.files.values():
        m = st.meta
        if m.get("agent") != "claude":
            continue
        if (m.get("started") or 0) >= col.today:
            n_today += 1
        t = m.get("tok_today", 0)
        if t:
            tok += t
            cost += m.get("cost_today", 0.0)
            mdl = m.get("model") or "?"
            models[mdl] = models.get(mdl, 0) + t
    quota["claude"] = {
        "today_tokens": int(tok), "today_cost": round(cost, 2),
        "sessions_today": n_today,
        "by_model": sorted(models.items(), key=lambda kv: -kv[1])[:3],
        "note": "estimate",
    }

    return {
        "generated_at": int(time.time()),
        "clock": datetime.now().strftime("%H:%M"),
        "date": datetime.now().strftime("%m-%d"),
        "host": socket.gethostname(),
        "shown": shown,
        "agents": {k: v for k, v in sorted(per_agent.items())},
        "quota": quota,
        "procs": {**counts, **flags},
    }


def refresher():
    col = Collector()
    global SNAPSHOT
    while True:
        try:
            snap = build_snapshot(col)
            with STATE_LOCK:
                SNAPSHOT = snap
        except Exception as e:
            print(f"[agentboard] refresh error: {e}", flush=True)
        time.sleep(CFG["scan_interval_sec"])


# ------------------------------------------------------------- USB push

SERIAL_GLOBS = ["/dev/cu.usbmodem*"]


def wire_payload(snap):
    """Trimmed snapshot for the display (it only uses these keys)."""
    w = {k: snap[k] for k in ("generated_at", "clock", "date", "host",
                              "shown", "quota", "procs") if k in snap}
    return json.dumps(w, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"


def find_serial_device():
    for pat in SERIAL_GLOBS:
        devs = sorted(glob.glob(pat))
        if devs:
            return devs[0]
    return None


def push_serial(payload: bytes):
    """Open the CDC serial port, configure raw 8N1, write one JSON line."""
    dev = find_serial_device()
    if not dev:
        return False
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)     # blocking: flow-controlled
    except OSError:
        return False
    try:
        attrs = termios.tcgetattr(fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                   termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        oflag &= ~termios.OPOST
        lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG |
                   termios.IEXTEN)
        cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
        cflag |= termios.CS8 | termios.CLOCAL | termios.CREAD
        crtscts = getattr(termios, "CRTSCTS", 0)
        cflag &= ~crtscts
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        for name in ("B115200",):
            ispeed = ospeed = getattr(termios, name)
        termios.tcsetattr(fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
        os.write(fd, payload)
        return True
    except OSError:
        return False
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def serial_pusher():
    global SNAPSHOT
    while True:
        try:
            with STATE_LOCK:
                snap = SNAPSHOT
            if snap.get("generated_at"):
                ok = push_serial(wire_payload(snap))
                if not ok:
                    time.sleep(2)     # no device / busy: retry soon
        except Exception as e:
            print(f"[agentboard] serial push error: {e}", flush=True)
        time.sleep(CFG.get("push_interval_sec", 10))


DASHBOARD = """<!doctype html><html lang=zh><meta charset=utf-8>
<title>AgentBoard</title>
<style>
 body{font:14px/1.5 -apple-system,system-ui,sans-serif;margin:24px;background:#f5f5f4;color:#1c1917}
 h1{font-size:20px} h2{font-size:15px;margin:18px 0 6px}
 .bar{height:8px;background:#e7e5e4;border-radius:4px;overflow:hidden;width:180px;display:inline-block;vertical-align:middle}
 .bar i{display:block;height:100%;background:#16a34a}
 .bar.warn i{background:#d97706}.bar.hot i{background:#dc2626}
 table{border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.08)}
 td,th{padding:4px 10px;text-align:left;font-size:13px}
 th{background:#f0efee;font-weight:600}
 .running{color:#16a34a;font-weight:600}.idle{color:#d97706}.done{color:#a8a29e}
 .mut{color:#78716c;font-size:12px}
</style>
<h1>AgentBoard <span id=clock class=mut></span></h1>
<div id=quota></div>
<h2>会话</h2>
<table id=t></table>
<p class=mut id=procs></p>
<script>
const AG={claude:'Claude Code',codex:'Codex',pi:'pi',chatgpt:'ChatGPT'};
const ST={running:'● 运行中',idle:'◐ 空闲',done:'○ 结束'};
function fmtAge(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m';if(s<86400)return Math.floor(s/3600)+'h';return Math.floor(s/86400)+'d'}
function fmtT(n){if(n>1e6)return (n/1e6).toFixed(1)+'M';if(n>1e3)return (n/1e3).toFixed(0)+'k';return n}
function bar(p){const c=p>80?'hot':p>50?'warn':'';return `<div class="bar ${c}"><i style="width:${Math.min(100,p)}%"></i></div> ${p}%`}
async function tick(){
 const d=await (await fetch('/api/status')).json();
 document.getElementById('clock').textContent=`${d.host} · ${d.date} ${d.clock}`;
 let q=d.quota||{};let h='';
 if(q.codex){const c=q.codex;h+=`<h2>ChatGPT 额度 (${c.plan||'?'})</h2>`;
  if(c.primary)h+=`<div>5h 窗口 ${bar(c.primary.used_percent)} <span class=mut>重置 ${new Date(c.primary.resets_at*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'})}</span></div>`;
  if(c.secondary)h+=`<div>每周 ${bar(c.secondary.used_percent)}</div>`;}
 if(q.claude){const c=q.claude;h+=`<h2>Claude 今日 (≈)</h2><div>${fmtT(c.today_tokens)} tokens · $${c.today_cost} · ${c.sessions_today} 会话</div>`;
  if(c.by_model&&c.by_model.length)h+=`<div class=mut>${c.by_model.map(([m,t])=>m+': '+fmtT(t)).join(' · ')}</div>`;}
 document.getElementById('quota').innerHTML=h;
 const rows=(d.shown||[]).map(s=>`<tr><td>${AG[s.agent]||s.agent}</td><td class=${s.state}>${ST[s.state]}</td><td>${s.project}</td><td>${s.title}</td><td class=mut>${s.model||''}${s.originator?'<br>'+s.originator:''}</td><td class=mut>${fmtAge(s.age)}前</td></tr>`).join('');
 document.getElementById('t').innerHTML='<tr><th>Agent</th><th>状态</th><th>项目</th><th>标题</th><th>模型</th><th>最近活动</th></tr>'+rows;
 const p=d.procs||{};document.getElementById('procs').textContent=
  `进程: claude×${p.claude||0} codex×${p.codex||0} pi×${p.pi||0}${p.chatgpt_app?' · ChatGPT.app 运行中':''}`;
}
tick();setInterval(tick,5000);
</script>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/api/status":
            with STATE_LOCK:
                body = json.dumps(SNAPSHOT, ensure_ascii=False, separators=(",", ":"))
            self._send(200, body.encode("utf-8"), "application/json")
        elif path == "/":
            self._send(200, DASHBOARD.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/healthz":
            self._send(200, b"ok", "text/plain")
        else:
            self._send(404, b"not found", "text/plain")


def main():
    threading.Thread(target=refresher, daemon=True).start()
    threading.Thread(target=serial_pusher, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", CFG["port"]), Handler)
    print(f"[agentboard] serving on http://0.0.0.0:{CFG['port']}  (dashboard /, api /api/status, "
          f"usb push to {SERIAL_GLOBS[0]})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
