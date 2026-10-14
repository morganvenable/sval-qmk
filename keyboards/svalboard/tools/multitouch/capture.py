"""Capture local Azoteq frames from an exact serial; optionally relay to Windows.

Observation is the default. --relay explicitly requests the firmware's volatile
exclusive output lease. Always closes the synthetic device and releases the lease.
"""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import queue
import sys
import time

from transport import Assembler, FrameGuard, HidDispatcher, StreamClient


def open_device(serial):
    if sys.platform == "win32":
        import hid
    else:
        import hidraw as hid
    matches = [d for d in hid.enumerate(0x303A, 0x4044)
               if d.get("serial_number") == serial and d.get("usage_page") == 0xff61 and d.get("usage") == 0x62]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one exact Sval interface for {serial}; found {len(matches)}")
    device = hid.device()
    device.open_path(matches[0]["path"])
    return device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--relay", action="store_true")
    parser.add_argument("--report-size", type=int, choices=(32, 64), default=32)
    parser.add_argument("--diagnostics", action="store_true", help="Read acquisition diagnostics from the revised prototype")
    parser.add_argument("--retry-diagnostics", action="store_true", help="Read ACK retry counters (firmware 2305+)")
    args = parser.parse_args()
    if args.relay: parser.error("Split-pair prototype is observation-only; two-pad injection is not integrated")
    if not 0 < args.seconds <= 45:
        parser.error("Prototype sessions are bounded to 45 seconds (client renewal not implemented)")
    if args.output.exists():
        parser.error("Refusing to overwrite a capture")
    packets = queue.Queue(maxsize=128)
    def receive(packet):
        packets.put_nowait(packet)  # overflow terminates dispatcher; lease expires
    device = open_device(args.serial)
    dispatcher = HidDispatcher(device, receive, report_size=args.report_size)
    client = StreamClient(dispatcher)
    backend = guard = None
    result = {"frames": 0, "max_contacts": 0, "flags": {}, "sequence_gaps": 0, "serial": args.serial}
    started_stream = False
    try:
        result["before"] = client.command(0)
        if args.diagnostics:
            result["diagnostics_before"] = client.diagnostics()
        if args.retry_diagnostics:
            result["retry_before"] = client.retry_diagnostics()
        if args.relay:
            from windows_touchpad import WindowsTouchpad
            backend = WindowsTouchpad()
            guard = FrameGuard(backend)
        result["start"] = client.command(1, int(args.relay))
        started_stream = True
        assembler = Assembler(client.client, client.token)
        start = renewed = time.monotonic()
        previous = {}
        result["pads"] = {}
        with args.output.open("x", encoding="utf-8") as output:
            while time.monotonic() - start < args.seconds:
                if dispatcher.failure:
                    raise dispatcher.failure
                if time.monotonic() - renewed >= .1:
                    result["latest"] = client.command(2)
                    renewed = time.monotonic()
                try:
                    frame = assembler.accept(packets.get(timeout=.005))
                except queue.Empty:
                    frame = None
                if frame:
                    if frame.pad in previous and (frame.sequence - previous[frame.pad]) & 0xffff != 1:
                        result["sequence_gaps"] += 1
                    previous[frame.pad] = frame.sequence
                    pad = result["pads"].setdefault(str(frame.pad), {"frames": 0, "max_contacts": 0})
                    pad["frames"] += 1
                    pad["max_contacts"] = max(pad["max_contacts"], frame.mask.bit_count())
                    result["frames"] += 1
                    result["max_contacts"] = max(result["max_contacts"], frame.mask.bit_count())
                    result["flags"][str(frame.flags)] = result["flags"].get(str(frame.flags), 0) + 1
                    output.write(json.dumps(dict(received=time.monotonic(), **asdict(frame))) + "\n")
                    if guard:
                        guard.feed(frame)
                if guard:
                    guard.tick()
        result["elapsed"] = time.monotonic() - start
        result["incomplete"] = assembler.incomplete
        if args.diagnostics:
            result["diagnostics_after"] = client.diagnostics()
        if args.retry_diagnostics:
            result["retry_after"] = client.retry_diagnostics()
        if guard:
            result["cancellations"] = guard.cancellations
    finally:
        try:
            if backend:
                backend.close()
        finally:
            try:
                if started_stream and not dispatcher.failure:
                    result["stop"] = client.command(3)
            finally:
                dispatcher.close()
                device.close()
                args.output.with_suffix(".summary.json").write_text(json.dumps(result, indent=2) + "\n")
                print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
