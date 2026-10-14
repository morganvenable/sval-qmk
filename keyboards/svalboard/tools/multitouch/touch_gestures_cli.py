"""Read and change the firmware's touch gesture settings (Sval 0xDF 0x2E).

  python touch_gestures_cli.py --serial SERIAL get
  python touch_gestures_cli.py --serial SERIAL set swipe_owner=firmware swipe_mm=10
  python touch_gestures_cli.py --serial SERIAL set pinch_owner=firmware pinch_out=0x01D9
  python touch_gestures_cli.py --serial SERIAL defaults
  python touch_gestures_cli.py --serial SERIAL status --watch

SET applies at once and is saved on the board (only when something changed).
A running unified_lab.py picks the change up within two seconds. Keycodes are
16-bit QMK keycodes (Keybard shows them); 0 means the output does nothing.
"""
import argparse
import time

from capture import open_device
import gesture_config as gc
from transport import HidDispatcher, StreamClient


def show(config):
    for name in gc.field_names():
        value = config.keycodes[gc.OUTPUTS.index(name)] if name in gc.OUTPUTS else getattr(config, name)
        print(f"  {name:18} {('0x%04X' % value) if name in gc.OUTPUTS else value}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--serial", required=True, help="USB serial of the half plugged into the computer")
    p.add_argument("action", choices=("get", "set", "defaults", "status"))
    p.add_argument("changes", nargs="*", help="name=value pairs for set: " + ", ".join(gc.field_names()))
    p.add_argument("--watch", action="store_true", help="status: repeat every 200 ms until Ctrl+C")
    args = p.parse_args()
    device = open_device(args.serial)
    dispatcher = HidDispatcher(device, report_size=64)
    try:
        client = StreamClient(dispatcher)
        client.bootstrap()
        if args.action == "get":
            config = gc.get(client)
            if config is None:
                raise SystemExit("This firmware has no touch gesture settings (Sval 0x2E).")
            show(config)
        elif args.action == "set":
            config = gc.get(client)
            if config is None:
                raise SystemExit("This firmware has no touch gesture settings (Sval 0x2E).")
            changes = {}
            for item in args.changes:
                name, _, value = item.partition("=")
                if name not in gc.field_names() or not value:
                    raise SystemExit(f"Unknown setting {item!r}; settings: {', '.join(gc.field_names())}")
                changes[name] = gc.parse_value(name, value)
            config = config.with_settings(**changes)
            if config.problems():
                raise SystemExit("; ".join(config.problems()))
            gc.put(client, config)
            show(gc.get(client))
        elif args.action == "defaults":
            show(gc.defaults(client))
        else:
            while True:
                print(gc.status(client))
                if not args.watch:
                    break
                time.sleep(.2)
    except KeyboardInterrupt:
        pass
    finally:
        dispatcher.close()
        device.close()


if __name__ == "__main__":
    main()
