"""Small stdlib-only WebSocket client for the adapter's integration tests."""
import base64
from collections import deque
import hashlib
import json
import os
import socket
import struct
import threading
import time


class Client:
    def __init__(self, port, heartbeat=True, read=True, origin=None, pong=True):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.socket.settimeout(1)
        self.lock = threading.Lock()
        self.messages = deque(maxlen=30000)
        self.closed = False
        self.reader = None
        self.pong = pong
        self.pings = 0
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                   f'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n')
        if origin:
            request += f'Origin: {origin}\r\n'
        self.socket.sendall((request+'\r\n').encode())
        response = b''
        while not response.endswith(b'\r\n\r\n'):
            response += self.socket.recv(1)
            assert len(response) < 8192
        assert b'101 Switching Protocols' in response, response
        accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert accept.lower() in response.lower()
        if read:
            self.start_reading()
        if heartbeat:
            threading.Thread(target=self._heartbeat, daemon=True).start()

    def start_reading(self):
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _heartbeat(self):
        while not self.closed:
            self.send({'type': 'heartbeat'})
            time.sleep(.5)

    def _exact(self, n):
        data = bytearray()
        while len(data) < n and not self.closed:
            try:
                chunk = self.socket.recv(n-len(data))
            except socket.timeout:
                continue
            if not chunk:
                raise EOFError()
            data.extend(chunk)
        if len(data) != n:
            raise EOFError()
        return bytes(data)

    def _read(self):
        try:
            while not self.closed:
                first, second = self._exact(2)
                assert first & 128, 'Server unexpectedly fragmented a message'
                opcode, count = first & 15, second & 127
                if count == 126:
                    count = struct.unpack('!H', self._exact(2))[0]
                elif count == 127:
                    count = struct.unpack('!Q', self._exact(8))[0]
                assert not second & 128
                data = self._exact(count)
                if opcode == 8:
                    self._frame(8, data)
                    break
                if opcode == 9:
                    self.pings += 1
                    if self.pong:
                        self._frame(10, data)
                elif opcode == 1:
                    self.messages.append(json.loads(data))
                elif opcode == 2:
                    self.messages.append(data)
        except (OSError, EOFError):
            pass
        finally:
            self.closed = True

    def _frame(self, opcode, data):
        if self.closed:
            return
        mask = os.urandom(4)
        n = len(data)
        packet = bytes([128 | opcode])
        packet += bytes([128 | n]) if n < 126 else bytes([128 | 126])+struct.pack('!H', n)
        packet += mask+bytes(value ^ mask[i % 4] for i, value in enumerate(data))
        try:
            with self.lock:
                self.socket.sendall(packet)
        except OSError:
            self.closed = True

    def send(self, data):
        self._frame(2 if isinstance(data, bytes) else 1,
                    data if isinstance(data, bytes) else json.dumps(data).encode())

    def wait(self, predicate, timeout=5):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            for value in list(self.messages):
                if predicate(value):
                    return value
            time.sleep(.01)
        raise AssertionError(f'WebSocket timeout; recent JSON: {[v for v in list(self.messages)[-60:] if isinstance(v, dict)]}')

    def state(self, predicate=lambda s: s.get('ready'), timeout=5):
        return self.wait(lambda s: isinstance(s, dict) and s.get('type') == 'state' and predicate(s), timeout)

    def role(self, role, owner=None):
        return self.wait(lambda s: isinstance(s, dict) and s.get('type') == 'role'
                         and s.get('role') == role and (owner is None or s['owner_present'] == owner))

    def close(self):
        try:
            self._frame(8, b'\x03\xe8')
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.closed = True
        self.socket.close()
