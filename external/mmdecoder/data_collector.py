"""Persistent collectors for the QO-100 beacon's text channels.

Input: CRC-validated frame events from rx_probe. No audio/UI dependencies.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
from pathlib import Path
import re
import sqlite3
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zipfile

CHANNELS = ("cw-skimmer", "dx-cluster", "bulletin")
PAYLOAD = 219
MAX_FILE = PAYLOAD * 1024 - 55
MAX_TEXT = 4 * 1024 * 1024


def crc16(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value


def decode_text(data):
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp1252", "replace")


class PlainHTML(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts, self.hidden, self.link = [], 0, None

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.hidden += 1
        if self.hidden:
            return
        if tag in ("br", "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "pre"):
            self.parts.append("\n")
        if tag == "li":
            self.parts.append("• ")
        if tag == "a":
            self.link = dict(attrs).get("href")

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.hidden = max(0, self.hidden-1)
            return
        if self.hidden:
            return
        if tag == "a" and self.link:
            if self.link.startswith(("http://", "https://")) and self.link.rstrip("/") not in "".join(self.parts[-3:]):
                self.parts.append(" (" + self.link + ")")
            self.link = None
        if tag in ("p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "pre"):
            self.parts.append("\n")

    def handle_data(self, text):
        if not self.hidden:
            self.parts.append(text)


def bulletin_text(data):
    text = decode_text(data).replace("\r\n", "\n").replace("\r", "\n")
    metadata = {}
    match = re.match(r"^~([A-Za-z]):(\d+):([^~\n]*)~\n", text)
    if match:
        metadata = {"category": match[1], "page": int(match[2]), "title": match[3]}
        text = text[match.end():]
    if re.search(r"</?[A-Za-z][^>]*>", text):
        parser = PlainHTML()
        parser.feed(text)
        text = "".join(parser.parts)
    text = "\n".join(line.rstrip() for line in text.splitlines()).strip()
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text, metadata


def parse_spot(payload):
    subtype = payload[0]
    if subtype == 0:
        # Type 0 has one length byte; ignore padding/extra bytes after NUL.
        if payload[1] > 217:
            raise ValueError("Ungültige DX-Cluster-Textlänge")
        raw = decode_text(payload[2:2+payload[1]].split(b"\0", 1)[0]).strip()
        keys = ("reporter", "frequency", "callsign", "comment", "utc", "locator")
    else:
        # The beacon's receiver HTML reads type 2 directly after its type byte.
        raw = decode_text(payload[1:].split(b"\0", 1)[0]).strip()
        keys = ("reporter", "frequency", "callsign", "signal_db", "wpm", "comment", "utc")
    if not raw:
        return None
    parts = raw.split("~")
    fields = dict(zip(keys, (x.strip() for x in parts))) if len(parts) >= len(keys) else {}
    if fields:
        fields["reporter"] = fields["reporter"].strip(": ")
        unit = " kHz" if subtype == 0 else ""
        detail = fields.get("comment", "")
        if subtype == 2:
            detail = f"{fields['signal_db']} dB · {fields['wpm']} WPM · {detail}"
        text = (f"{fields['utc']:<6} {fields['callsign']:<12} "
                f"{fields['frequency']}{unit} · {fields['reporter']} · {detail}")
        if fields.get("locator"):
            text += " · " + fields["locator"]
    else:
        text = raw
    return raw, text.strip(), fields


class Collector:
    """Single writer via ingest(); thread-safe snapshots and HTTP reads."""
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        (self.directory / "bulletins").mkdir(exist_ok=True)
        self.lock = threading.RLock()
        self.db = sqlite3.connect(self.directory / "receiver.sqlite3", check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS events (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, channel TEXT NOT NULL,
                received_at TEXT NOT NULL, epoch REAL NOT NULL, raw_text TEXT NOT NULL,
                text TEXT NOT NULL, fields TEXT NOT NULL, fingerprint TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS event_dedup ON events(channel,fingerprint,epoch);
            CREATE TABLE IF NOT EXISTS bulletins (
                seq INTEGER PRIMARY KEY, filename TEXT NOT NULL, digest TEXT NOT NULL,
                UNIQUE(filename,digest)
            );
            CREATE TABLE IF NOT EXISTS transfers (
                id TEXT PRIMARY KEY, filename TEXT NOT NULL, file_id INTEGER NOT NULL,
                size INTEGER NOT NULL, kind INTEGER NOT NULL, expected INTEGER NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS blocks (
                transfer_id TEXT NOT NULL, idx INTEGER NOT NULL, data BLOB NOT NULL,
                PRIMARY KEY(transfer_id,idx)
            );
        """)
        self.active = None
        self.rx_status, self.rx_at = {}, 0.0
        self.last_error = ""
        self.malformed = 0
        self.revision = int(self.db.execute("SELECT coalesce(max(seq),0) FROM events").fetchone()[0])
        self._export_all()

    def begin_session(self):
        with self.lock:
            self.active = None
            self.rx_status, self.rx_at = {}, 0.0

    @staticmethod
    def _timestamp(event):
        stamp = event.get("received_at") or datetime.now(timezone.utc).isoformat()
        if not isinstance(stamp, str):
            raise ValueError("Ungültiger Empfangszeitpunkt")
        dt = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        epoch = dt.timestamp()
        if not math.isfinite(epoch):
            raise ValueError("Ungültiger Empfangszeitpunkt")
        return dt.isoformat(), epoch

    @staticmethod
    def _event(row):
        value = dict(row)
        value["fields"] = json.loads(value["fields"])
        value.pop("epoch", None)
        value.pop("fingerprint", None)
        return value

    def _insert(self, channel, stamp, epoch, raw, text, fields):
        fingerprint = hashlib.sha256(raw.encode()).hexdigest()
        cur = self.db.execute("INSERT INTO events(channel,received_at,epoch,raw_text,text,fields,fingerprint) VALUES(?,?,?,?,?,?,?)",
                              (channel, stamp, epoch, raw, text, json.dumps(fields, ensure_ascii=False), fingerprint))
        return self._event(self.db.execute("SELECT * FROM events WHERE seq=?", (cur.lastrowid,)).fetchone())

    def ingest(self, event):
        if event.get("event") == "status":
            with self.lock:
                self.rx_status = {k: v for k, v in event.items() if k != "points"}
                self.rx_at = time.monotonic()
            return []
        if event.get("event") == "session":
            self.begin_session()
            return []
        if event.get("event") != "frame":
            return []
        with self.lock:
            try:
                payload = bytes.fromhex(event["payload_hex"])
                kind, idx, info = event["frame_type"], event["frame_number"], event["frame_info"]
                if len(payload) != PAYLOAD or type(kind) is not int or not 0 <= kind <= 15 or type(idx) is not int or not 0 <= idx < 1024 or type(info) is not int or not 0 <= info <= 3:
                    raise ValueError("Ungültiger Empfangsblock")
                stamp, epoch = self._timestamp(event)
                result = None
                if kind == 8 and payload[0] in (0, 2):
                    parsed = parse_spot(payload)
                    if parsed:
                        raw, text, fields = parsed
                        channel = "dx-cluster" if payload[0] == 0 else "cw-skimmer"
                        fingerprint = hashlib.sha256(raw.encode()).hexdigest()
                        duplicate = self.db.execute("SELECT 1 FROM events WHERE channel=? AND fingerprint=? AND epoch BETWEEN ? AND ? LIMIT 1",
                                                    (channel, fingerprint, epoch-120, epoch+120)).fetchone()
                        if not duplicate:
                            result = self._insert(channel, stamp, epoch, raw, text, fields)
                elif kind in (2, 3, 4, 5):
                    result = self._file_frame(kind, idx, info, payload, stamp, epoch)
                self.db.commit()
                if result:
                    self.revision = result["seq"]
                    self._publish(result)
                    return [result]
            except (ValueError, KeyError, TypeError, OverflowError, zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
                self.db.rollback()
                self.malformed += 1
                self.last_error = str(exc)
            return []

    def _file_frame(self, kind, idx, info, payload, stamp, epoch):
        if info in (0, 3):
            self.active = None  # Every new file supersedes the current stream context.
            name = decode_text(payload[:50].split(b"\0", 1)[0])
            name = name.replace("\\", "/").rsplit("/", 1)[-1]
            if kind not in (3, 4, 5) or not name.lower().endswith(".blt"):
                return None
            size = int.from_bytes(payload[52:55], "big")
            file_id = int.from_bytes(payload[50:52], "big")
            expected = (size + 55 + PAYLOAD - 1) // PAYLOAD
            if not 1 <= size <= MAX_FILE or (info == 0 and idx != 0) or (info == 3 and expected != 1):
                raise ValueError("Ungültiger Bulletin-Dateikopf")
            key = hashlib.sha256(payload[:55] + bytes([kind])).hexdigest()
            self.db.execute("INSERT INTO transfers VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET updated_at=excluded.updated_at",
                            (key, name, file_id, size, kind, expected, stamp))
            self.active = key
            if info == 3:
                idx = 0  # Single frames can carry the previous transfer's counter.
            # Bound the partial cache; the normal carousel contains only a few files.
            old = self.db.execute("SELECT id FROM transfers ORDER BY updated_at DESC LIMIT -1 OFFSET 32").fetchall()
            for row in old:
                if row[0] != self.active:
                    self.db.execute("DELETE FROM blocks WHERE transfer_id=?", (row[0],))
                    self.db.execute("DELETE FROM transfers WHERE id=?", (row[0],))
        if self.active is None:
            return None
        transfer = self.db.execute("SELECT * FROM transfers WHERE id=?", (self.active,)).fetchone()
        if not transfer or kind != transfer["kind"]:
            return None
        if idx >= transfer["expected"] or (info == 2 and idx != transfer["expected"]-1):
            raise ValueError("Bulletin-Blocknummer passt nicht zur Dateilänge")
        self.db.execute("INSERT INTO blocks VALUES(?,?,?) ON CONFLICT(transfer_id,idx) DO UPDATE SET data=excluded.data",
                        (self.active, idx, payload))
        self.db.execute("UPDATE transfers SET updated_at=? WHERE id=?", (stamp, self.active))
        blocks = self.db.execute("SELECT idx,data FROM blocks WHERE transfer_id=? ORDER BY idx", (self.active,)).fetchall()
        if len(blocks) != transfer["expected"]:
            return None
        data = b"".join(bytes(row["data"]) for row in blocks)[55:55+transfer["size"]]
        if crc16(data) != transfer["file_id"]:
            # A complete but inconsistent assembly must be repaired from a repeat.
            self.db.execute("DELETE FROM blocks WHERE transfer_id=? AND idx!=0", (self.active,))
            self.last_error = "Bulletin-Datei-CRC falsch; warte auf Wiederholung"
            return None
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries = [item for item in archive.infolist() if not item.is_dir()]
            if len(entries) != 1 or not entries[0].filename.lower().endswith(".blt") or entries[0].file_size > MAX_TEXT:
                raise ValueError("Ungültiger Bulletin-ZIP-Inhalt")
            # Read in memory; ZIP member paths are never extracted to disk.
            with archive.open(entries[0]) as source:
                content = source.read(MAX_TEXT+1)
            if len(content) > MAX_TEXT:
                raise ValueError("Bulletin ist zu groß")
        digest = hashlib.sha256(content).hexdigest()
        exists = self.db.execute("SELECT seq FROM bulletins WHERE filename=? AND digest=?", (transfer["filename"], digest)).fetchone()
        result = None
        if not exists:
            text, fields = bulletin_text(content)
            fields.update({"filename": transfer["filename"], "sha256": digest,
                           "file_id": transfer["file_id"], "bytes": len(content)})
            result = self._insert("bulletin", stamp, epoch, decode_text(content), text, fields)
            self.db.execute("INSERT INTO bulletins VALUES(?,?,?)", (result["seq"], transfer["filename"], digest))
        self.db.execute("DELETE FROM blocks WHERE transfer_id=?", (self.active,))
        self.db.execute("DELETE FROM transfers WHERE id=?", (self.active,))
        self.active = None
        self.last_error = ""
        return result

    def events(self, after=0, limit=500, channel=None):
        with self.lock:
            query, params = "SELECT * FROM events WHERE seq>?", [after]
            if channel:
                query += " AND channel=?"
                params.append(channel)
            query += " ORDER BY seq LIMIT ?"
            params.append(limit)
            return [self._event(r) for r in self.db.execute(query, params)]

    def recent(self, channel, limit=1000):
        with self.lock:
            rows = self.db.execute("SELECT * FROM events WHERE channel=? ORDER BY epoch DESC,seq DESC LIMIT ?", (channel, limit)).fetchall()
            return [self._event(r) for r in reversed(rows)]

    def bulletins(self):
        with self.lock:
            return [self._event(r) for r in self.db.execute("SELECT e.* FROM events e JOIN bulletins b ON b.seq=e.seq ORDER BY e.epoch DESC,e.seq DESC")]

    def text(self, channel):
        rows = self.bulletins()[:1] if channel == "bulletin" else self.recent(channel)
        if channel == "bulletin":
            return rows[0]["text"] + "\n" if rows else ""
        return "".join(f"{r['received_at']}  {r['text']}\n" for r in rows)

    def status(self):
        with self.lock:
            counts = {channel: 0 for channel in CHANNELS}
            counts.update(dict(self.db.execute("SELECT channel,count(*) FROM events GROUP BY channel")))
            pending = [dict(r) for r in self.db.execute("SELECT t.filename,t.expected,count(b.idx) AS received FROM transfers t LEFT JOIN blocks b ON t.id=b.transfer_id GROUP BY t.id ORDER BY t.updated_at DESC")]
            rx = dict(self.rx_status)
            rx["locked"] = bool(rx.get("locked") and time.monotonic()-self.rx_at < 2)
            return {"counts": counts, "last_sequence": self.revision, "pending_bulletins": pending,
                    "malformed_frames": self.malformed, "last_error": self.last_error, "receiver": rx}

    def _atomic_text(self, path, text):
        temporary = path.with_name(path.name+".tmp")
        temporary.write_text(text, encoding="utf-8")
        temporary.replace(path)

    def _export_all(self):
        for channel in CHANNELS:
            self._atomic_text(self.directory/(channel+".txt"), self.text(channel))
        # SQLite is authoritative; regenerating repairs an interrupted export.
        temporary = self.directory/"events.jsonl.tmp"
        with temporary.open("w", encoding="utf-8") as out:
            for row in self.db.execute("SELECT * FROM events ORDER BY seq"):
                out.write(json.dumps(self._event(row), ensure_ascii=False)+"\n")
        temporary.replace(self.directory/"events.jsonl")
        for event in self.bulletins():
            self._archive(event)

    def _archive(self, event):
        filename = re.sub(r"[^a-zA-Z0-9._-]", "_", event["fields"]["filename"])
        filename = f"{filename}-{event['fields']['sha256'][:12]}.txt"
        self._atomic_text(self.directory/"bulletins"/filename, event["text"]+"\n")

    def _publish(self, event):
        with (self.directory/"events.jsonl").open("a", encoding="utf-8") as out:
            out.write(json.dumps(event, ensure_ascii=False)+"\n")
        self._atomic_text(self.directory/(event["channel"]+".txt"), self.text(event["channel"]))
        if event["channel"] == "bulletin":
            self._archive(event)

    def close(self):
        with self.lock:
            self.db.close()


def replay_logs(collector, paths):
    """Import old validated frames; chronological replay is safe to repeat."""
    total = 0
    for path in sorted(map(Path, paths)):
        collector.begin_session()
        with path.open(encoding="utf-8") as source:
            for line in source:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue  # A live log may have an unfinished final line.
                if isinstance(event, dict) and event.get("event") in ("frame", "session"):
                    total += len(collector.ingest(event))
    return total


class TextAPI:
    def __init__(self, collector, port=8765, diagnostics=None):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                url = urlsplit(self.path)
                path = url.path
                try:
                    if path in ("/v1/cw-skimmer.txt", "/v1/dx-cluster.txt", "/v1/bulletin.txt"):
                        body, mime = collector.text(path.split("/")[-1][:-4]), "text/plain"
                    elif path == "/v1/diagnostics.json" and diagnostics is not None:
                        body, mime = json.dumps(diagnostics(), ensure_ascii=False, allow_nan=False), "application/json"
                    elif path == "/v1/status.json":
                        body, mime = json.dumps(collector.status(), ensure_ascii=False), "application/json"
                    elif path == "/v1/bulletins.json":
                        body = json.dumps([{k: v for k, v in row.items() if k not in ("text", "raw_text")} for row in collector.bulletins()], ensure_ascii=False)
                        mime = "application/json"
                    elif re.fullmatch(r"/v1/bulletins/\d+\.txt", path):
                        seq = int(path.rsplit("/", 1)[-1][:-4])
                        rows = collector.events(seq-1, 1)
                        if not rows or rows[0]["seq"] != seq or rows[0]["channel"] != "bulletin":
                            self.send_error(404)
                            return
                        body, mime = rows[0]["text"]+"\n", "text/plain"
                    elif path == "/v1/events":
                        query = parse_qs(url.query)
                        after, limit = int(query.get("after", ["0"])[0]), int(query.get("limit", ["500"])[0])
                        channel = query.get("channel", [None])[0]
                        if after < 0 or not 1 <= limit <= 1000 or (channel and channel not in CHANNELS):
                            raise ValueError("Ungültiger Cursor, Kanal oder Limit")
                        rows = collector.events(after, limit, channel)
                        body = "".join(json.dumps(r, ensure_ascii=False)+"\n" for r in rows)
                        mime = "application/x-ndjson"
                    else:
                        self.send_error(404)
                        return
                    encoded = body.encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", mime+"; charset=utf-8")
                    self.send_header("Content-Length", str(len(encoded)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(encoded)
                except ValueError as exc:
                    self.send_error(400, str(exc))
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
