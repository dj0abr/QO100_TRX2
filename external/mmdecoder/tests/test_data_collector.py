"""Protocol, persistence and text API checks independent of live radio reception."""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen
import zipfile

from data_collector import Collector, TextAPI, bulletin_text, crc16, replay_logs

BASE = datetime(2026, 11, 9, 13, 0, tzinfo=timezone.utc)
LONG = "~B:1:Test~\n<h3>Test-Bulletin</h3>\n" + "\n".join(
    f"Zeile {i}: {hashlib.sha256(str(i).encode()).hexdigest()}" for i in range(45))


def frame(kind, payload, idx=0, info=3, seconds=0):
    return {"event": "frame", "frame_type": kind, "frame_number": idx,
            "frame_info": info, "payload_hex": payload.ljust(219, b"\0").hex(),
            "received_at": (BASE+timedelta(seconds=seconds)).isoformat()}


def dx(text="DL1AAA~14025.0~VK9TEST~CQ DX~1300Z~JO40", seconds=0, padding=b""):
    raw = text.encode()
    return frame(8, b"\0"+bytes([217])+raw+b"\0"+padding, seconds=seconds)


def file_frames(name="news.blt", text=LONG, seconds=0, data=None, file_id=None, single_counter=0):
    if data is None:
        memory = io.BytesIO()
        with zipfile.ZipFile(memory, "w", zipfile.ZIP_DEFLATED) as archive:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, text.encode())
        data = memory.getvalue()
    header = name.encode().ljust(50, b"\0") + (crc16(data) if file_id is None else file_id).to_bytes(2,"big") + len(data).to_bytes(3,"big")
    stream = header+data
    blocks = [stream[i:i+219].ljust(219, b"\0") for i in range(0,len(stream),219)]
    return [frame(3, block, single_counter if len(blocks)==1 else i,
                  3 if len(blocks)==1 else 0 if i==0 else 2 if i==len(blocks)-1 else 1,
                  seconds+i) for i,block in enumerate(blocks)]


class CollectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)/"data"
        self.collector = Collector(self.path)

    def tearDown(self):
        self.collector.close()
        self.temp.cleanup()

    def ingest_all(self, frames):
        return [event for f in frames for event in self.collector.ingest(f)]

    def test_dx_length_and_padding_are_removed(self):
        event = self.collector.ingest(dx(padding=b"\xff\x01BADTAIL"))[0]
        self.assertEqual(event["fields"]["callsign"], "VK9TEST")
        self.assertEqual(event["fields"]["frequency"], "14025.0")
        self.assertNotIn("BADTAIL", event["text"])
        self.assertNotIn("\0", event["raw_text"])
        self.assertNotIn("�", event["text"])

    def test_cw_has_no_length_byte_and_preserves_fields(self):
        event = self.collector.ingest(frame(8,b"\x02DL0ABC:~10489750~DL1TEST~18~23~CQ~1301Z"))[0]
        self.assertEqual(event["channel"], "cw-skimmer")
        self.assertEqual(event["fields"]["reporter"], "DL0ABC")
        self.assertEqual(event["fields"]["signal_db"], "18")
        self.assertEqual(event["fields"]["wpm"], "23")

    def test_duplicates_are_suppressed_but_later_repeat_is_kept(self):
        self.collector.ingest(dx(seconds=0))
        self.assertEqual(self.collector.ingest(dx(seconds=60)), [])
        self.assertEqual(len(self.collector.ingest(dx(seconds=121))), 1)
        self.assertEqual(len(self.collector.events()), 2)

    def test_unrecognized_spot_format_is_kept_as_raw_text(self):
        event = self.collector.ingest(frame(8,b"\x02NEW CW FORMAT"))[0]
        self.assertEqual(event["text"], "NEW CW FORMAT")
        self.assertEqual(event["fields"], {})

    def test_full_bulletin_is_verified_unzipped_and_converted(self):
        events = self.ingest_all(file_frames())
        self.assertEqual(len(events),1)
        event = events[0]
        self.assertEqual(event["fields"]["category"], "B")
        self.assertEqual(event["fields"]["page"], 1)
        self.assertIn("Test-Bulletin",event["text"])
        self.assertNotIn("<h3>",event["text"])
        self.assertEqual((self.path/"bulletin.txt").read_text(),event["text"]+"\n")
        self.assertEqual(len(list((self.path/"bulletins").glob("*.txt"))),1)
        self.assertEqual(self.collector.status()["pending_bulletins"],[])

    def test_interleaved_spectrum_does_not_interrupt_bulletin(self):
        frames = file_frames()
        for f in frames:
            self.collector.ingest(f)
            self.collector.ingest(frame(8,b"\x01"+b"\x20"*218))
            self.collector.ingest(frame(8,b"\x03"+b"\x20"*218))
        self.assertEqual(self.collector.status()["counts"]["bulletin"],1)
        self.assertEqual(len(self.collector.events()),1)

    def test_missing_block_is_recovered_from_repetition_and_restart(self):
        frames = file_frames()
        self.ingest_all([f for i,f in enumerate(frames) if i!=2])
        self.assertEqual(self.collector.bulletins(),[])
        self.collector.close()
        self.collector=Collector(self.path)
        self.assertTrue(self.collector.status()["pending_bulletins"])
        self.assertEqual(self.collector.ingest(frames[2]),[])  # Needs a fresh stream header.
        self.collector.ingest(frames[0])
        self.assertEqual(len(self.collector.ingest(frames[2])),1)

    def test_out_of_order_last_block_can_arrive_before_middle(self):
        frames=file_frames()
        events=self.ingest_all([frames[0],frames[-1]]+frames[1:-1])
        self.assertEqual(len(events),1)

    def test_single_frame_can_have_nonzero_counter(self):
        frames=file_frames(text="Kurz",single_counter=77)
        self.assertEqual(len(frames),1)
        self.assertEqual(len(self.ingest_all(frames)),1)

    def test_non_bulletin_file_interrupts_active_context_and_is_ignored(self):
        frames=file_frames()
        self.collector.ingest(frames[0])
        self.collector.ingest(file_frames(name="unwanted.html",text="ignore")[0])
        self.ingest_all(frames[1:])
        self.assertEqual(self.collector.bulletins(),[])
        self.assertFalse(any(self.path.rglob("*.html")))

    def test_bad_file_crc_never_publishes(self):
        frames=file_frames(file_id=123)
        self.assertEqual(self.ingest_all(frames),[])
        self.assertIn("CRC",self.collector.status()["last_error"])
        self.assertEqual((self.path/"bulletin.txt").read_text(),"")

    def test_invalid_zip_never_publishes(self):
        self.assertEqual(self.ingest_all(file_frames(data=b"not a zip")),[])
        self.assertEqual(self.collector.status()["malformed_frames"],1)

    def test_repeated_bulletin_is_deduplicated_new_version_is_archived(self):
        self.ingest_all(file_frames(text="Version eins"))
        self.assertEqual(self.ingest_all(file_frames(text="Version eins",seconds=200)),[])
        self.ingest_all(file_frames(text="Version zwei",seconds=300))
        self.assertEqual(len(self.collector.bulletins()),2)
        self.assertIn("Version zwei",self.collector.text("bulletin"))
        self.assertEqual(len(list((self.path/"bulletins").glob("*.txt"))),2)

    def test_timestamp_keeps_late_replay_from_replacing_newer_bulletin(self):
        self.ingest_all(file_frames(text="Neu",seconds=300))
        self.ingest_all(file_frames(text="Alt",seconds=0))
        self.assertEqual(self.collector.text("bulletin"),"Neu\n")

    def test_text_conversion_handles_metadata_unicode_and_hidden_scripts(self):
        text,meta=bulletin_text('~N:2:Umfrage:20240923~\r\n<h3>Grüße &amp; News</h3><script>evil()</script><p>Text<br>Zeile</p>'.encode())
        self.assertEqual(meta['title'],'Umfrage:20240923')
        self.assertIn('Grüße & News',text)
        self.assertNotIn('evil',text)
        self.assertIn('Text\nZeile',text)

    def test_crc_matches_known_reference(self):
        self.assertEqual(crc16(b"123456789"),0x6f91)

    def test_persistence_cursor_and_export_repair(self):
        self.collector.ingest(dx())
        self.collector.ingest(frame(8,b"\x02CW TEST",seconds=1))
        self.collector.close()
        (self.path/"events.jsonl").write_text("broken")
        self.collector=Collector(self.path)
        self.assertEqual([r['seq'] for r in self.collector.events(1)],[2])
        self.assertEqual(len((self.path/'events.jsonl').read_text().splitlines()),2)
        self.assertEqual(self.collector.ingest(dx()),[])

    def test_concurrent_ingestion_keeps_monotonic_cursor_and_valid_exports(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda i:self.collector.ingest(dx(text=f"R~14000~DX{i}~CQ~1300Z~",seconds=i)),range(40)))
        rows=self.collector.events()
        self.assertEqual([r['seq'] for r in rows],list(range(1,41)))
        lines=[json.loads(x) for x in (self.path/'events.jsonl').read_text().splitlines()]
        self.assertEqual(len(lines),40)

    def test_stale_receiver_status_does_not_remain_locked(self):
        with patch('data_collector.time.monotonic',return_value=10):
            self.collector.ingest({'event':'status','locked':True})
        with patch('data_collector.time.monotonic',return_value=13):
            self.assertFalse(self.collector.status()['receiver']['locked'])

    def test_malformed_frame_does_not_stop_next_valid_frame(self):
        event=dx();event['payload_hex']='00'
        self.assertEqual(self.collector.ingest(event),[])
        self.assertEqual(len(self.collector.ingest(dx())),1)
        self.assertEqual(self.collector.status()['malformed_frames'],1)

    def test_importing_same_log_twice_is_idempotent(self):
        log=Path(self.temp.name)/'input.jsonl'
        log.write_text('\n'.join(json.dumps(r) for r in [dx()]+file_frames())+'\n{"unfinished":')
        self.assertEqual(replay_logs(self.collector,[log]),2)
        self.assertEqual(replay_logs(self.collector,[log]),0)

    def test_local_http_text_cursor_channel_and_error_responses(self):
        self.collector.ingest(dx())
        self.collector.ingest(frame(8,b'\x02CW TEST',seconds=1))
        self.ingest_all(file_frames(text='Test-Bulletin',seconds=2))
        api=TextAPI(self.collector,0)
        try:
            def get(path):
                with urlopen(api.url+path,timeout=3) as response:
                    return response.read().decode()
            self.assertIn('VK9TEST',get('/v1/dx-cluster.txt'))
            self.assertEqual(get('/v1/bulletin.txt'),'Test-Bulletin\n')
            self.assertEqual(get('/v1/bulletins/3.txt'),'Test-Bulletin\n')
            rows=[json.loads(x) for x in get('/v1/events?after=1&limit=1').splitlines()]
            self.assertEqual([x['seq'] for x in rows],[2])
            self.assertEqual(json.loads(get('/v1/status.json'))['counts']['bulletin'],1)
            self.assertEqual(len(json.loads(get('/v1/bulletins.json'))),1)
            self.assertEqual(len(get('/v1/events?channel=cw-skimmer').splitlines()),1)
            for path,code in [('/v1/events?after=-1',400),('/v1/events?channel=bad',400),('/v1/bulletins/999.txt',404)]:
                with self.assertRaises(HTTPError) as caught:get(path)
                self.assertEqual(caught.exception.code,code)
        finally:
            api.close()


if __name__=='__main__':
    unittest.main()
