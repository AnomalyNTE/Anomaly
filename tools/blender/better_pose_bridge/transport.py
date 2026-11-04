"""Threaded localhost transport for the BetterPose Blender bridge."""

import http.client
import json
import queue
import socket
import threading
import time


class _NoDelayHTTPConnection(http.client.HTTPConnection):
    """Avoid the small-packet delay that throttles JSON frame requests."""

    def connect(self):
        super().connect()
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class Client:
    def __init__(self, port=18923):
        self.host = "127.0.0.1"
        self.port = port
        self.commands = queue.Queue(maxsize=8)
        self.results = queue.Queue()
        self.lock = threading.Lock()
        self.latest = None
        self.heartbeat = None
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self._run, name="BetterPose Blender", daemon=True)
        self.thread.start()

    def request(self, path, data, callback=None):
        task = (path, data, callback)
        if path == "ping":
            # Idle heartbeats replace one another instead of occupying the
            # control queue when the game is busy or in the background.
            with self.lock:
                self.heartbeat = task
            return
        if path == "end":
            with self.lock:
                self.latest = self.heartbeat = None
            while True:
                try:
                    self.commands.get_nowait()
                except queue.Empty:
                    break
        try:
            self.commands.put_nowait(task)
        except queue.Full:
            raise RuntimeError("游戏正在处理上一项 Blender 操作，请稍后重试") from None

    def preview(self, data, callback=None):
        with self.lock:
            self.latest = ("frame", data, callback)
            self.heartbeat = None

    def _run(self):
        connection = _NoDelayHTTPConnection(self.host, self.port, timeout=3)
        last_request = time.monotonic()
        while not self.closed.is_set():
            try:
                task = self.commands.get(timeout=0.002)
            except queue.Empty:
                with self.lock:
                    task, self.latest = self.latest, None
                    if task is None:
                        task, self.heartbeat = self.heartbeat, None
                if task is None:
                    continue
            path, data, callback = task
            # The game's keep-alive endpoint closes an idle socket. Reconnect
            # before reusing it, and retry one transient failure so a first
            # Connect/Pull click is not lost after the editor has been idle.
            if time.monotonic() - last_request >= 2.0:
                connection.close()
            attempts = 2
            for attempt in range(attempts):
                try:
                    body = json.dumps(data, ensure_ascii=False,
                                      allow_nan=False).encode("utf-8")
                    connection.request(
                        "POST", f"/api/blender/{path}", body=body,
                        headers={"Content-Type": "application/json",
                                 "Connection": "keep-alive"})
                    response = connection.getresponse()
                    payload = response.read(4 * 1024 * 1024 + 1)
                    if len(payload) > 4 * 1024 * 1024:
                        raise ValueError("游戏响应过大")
                    result = json.loads(payload)
                    if response.will_close:
                        connection.close()
                    last_request = time.monotonic()
                    if result.get("retryable") and attempt + 1 < attempts:
                        connection.close()
                        connection = _NoDelayHTTPConnection(
                            self.host, self.port, timeout=3)
                        continue
                    break
                except Exception as exc:
                    connection.close()
                    retryable = isinstance(
                        exc, (OSError, TimeoutError, http.client.HTTPException))
                    result = {"ok": False, "retryable": retryable,
                              "error": str(exc)}
                    if attempt + 1 >= attempts or not retryable:
                        break
                    connection = _NoDelayHTTPConnection(
                        self.host, self.port, timeout=3)
                last_request = time.monotonic()
            if callback:
                self.results.put((callback, result))
        connection.close()

    def poll(self):
        for _ in range(32):
            try:
                callback, result = self.results.get_nowait()
            except queue.Empty:
                break
            callback(result)

    def close(self):
        self.closed.set()
        with self.lock:
            self.latest = None
            self.heartbeat = None
