"""Bounded prototype session for the existing Keybard Host device worker.

One dispatcher handles both SvalReader commands and contacts. Injection and lease
renewal run on a separate worker; Qt rendering is never on the input path.
"""
import queue
import os
import threading
import time

try:
    from .transport import Assembler, FrameGuard, HidDispatcher, StreamClient
except ImportError:
    from transport import Assembler, FrameGuard, HidDispatcher, StreamClient


class TouchSession:
    def __init__(self, device, relay=False, status=lambda text: None, duration=45):
        self.packets = queue.Queue(maxsize=128)
        self.stop = threading.Event()
        self.status, self.relay, self.duration = status, relay, min(duration, 45)
        self.dispatcher = HidDispatcher(device, self.packets.put_nowait,
                                        report_size=int(os.environ.get("KEYBARD_TOUCH_REPORT_SIZE", "32")))
        self.thread = threading.Thread(target=self._run, name="Keybard touch input", daemon=True)
        self.thread.start()

    def _run(self):
        client = StreamClient(self.dispatcher)
        backend = None
        active = False
        count = maximum = 0
        try:
            client.command(0)
            if self.relay:
                try:
                    from .windows_touchpad import WindowsTouchpad
                except ImportError:
                    from windows_touchpad import WindowsTouchpad
                backend = WindowsTouchpad()
            client.command(1, int(self.relay))
            active = True
            assembler = Assembler(client.client, client.token)
            guard = FrameGuard(backend) if backend else None
            started = renewed = reported = time.monotonic()
            self.status("Touch prototype active · 45-second maximum")
            while not self.stop.is_set() and time.monotonic() - started < self.duration:
                if self.dispatcher.failure:
                    raise self.dispatcher.failure
                if time.monotonic() - renewed >= .1:
                    client.command(2)
                    renewed = time.monotonic()
                try:
                    frame = assembler.accept(self.packets.get(timeout=.005))
                except queue.Empty:
                    frame = None
                if frame:
                    count += 1
                    maximum = max(maximum, frame.mask.bit_count())
                    if guard:
                        guard.feed(frame)
                if guard:
                    guard.tick()
                if time.monotonic() - reported >= 1:
                    self.status(f"Touch prototype · {count} frames · {maximum} maximum contacts")
                    reported = time.monotonic()
        except Exception as exc:
            self.status(f"Touch prototype stopped: {exc}")
        finally:
            try:
                if backend:
                    backend.close()
            finally:
                if active and not self.dispatcher.failure:
                    try:
                        client.command(3)
                    except Exception as exc:
                        self.status(f"Touch lease will expire: {exc}")
                self.status(f"Touch prototype ended · {count} frames · {maximum} maximum contacts")

    def close(self):
        self.stop.set()
        self.thread.join(1)
        self.dispatcher.close()
        self.thread.join(1)
        if self.thread.is_alive():
            raise OSError("Touch input worker did not stop")
