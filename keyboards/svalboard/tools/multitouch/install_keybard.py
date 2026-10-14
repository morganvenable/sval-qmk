"""Install the opt-in prototype in a local Keybard checkout, preserving other edits.

Default is a dry run. --apply adds three modules and narrowly edits two existing
files. It refuses unexpected source layouts instead of overwriting them.
"""
import argparse
from pathlib import Path


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("Keybard source changed or prototype already installed; inspect its diff")
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    root = args.repo / "companion/overlay-host/keybard_host"
    protocol = root / "device/protocol.py"
    worker = root / "device/worker.py"
    updates = {}
    updates[protocol] = replace_once(protocol.read_text(),
        '    def _exchange(self, request, accept):\n        self.checkpoint()\n',
        '    def _exchange(self, request, accept):\n        self.checkpoint()\n'
        '        if hasattr(self.device, "exchange"):\n'
        '            return self.device.exchange(request, accept)\n')
    source = worker.read_text()
    source = replace_once(source, 'import time\n', 'import time\nimport os\n')
    source = replace_once(source, '            device = None\n', '            device = None\n            touch = None\n')
    source = replace_once(source,
        '                reader = SvalReader(device, cancelled=self.stop_event.is_set)\n',
        '                transport = device\n'
        '                # Explicit serial opt-in: never activate on another connected board.\n'
        '                if serial and os.environ.get("KEYBARD_TOUCH_SERIAL") == serial:\n'
        '                    from ..multitouch.session import TouchSession\n'
        '                    touch = TouchSession(device, relay=os.environ.get("KEYBARD_TOUCH_MODE") == "relay",\n'
        '                                         status=self.status.emit)\n'
        '                    transport = touch.dispatcher\n'
        '                reader = SvalReader(transport, cancelled=self.stop_event.is_set)\n')
    source = replace_once(source, '            finally:\n                if device is not None:\n',
        '            finally:\n'
        '                if touch is not None:\n'
        '                    try:\n'
        '                        touch.close()\n'
        '                    except OSError as exc:\n'
        '                        self.status.emit(f"Touch cleanup: {exc}")\n'
        '                if device is not None:\n')
    updates[worker] = source
    module_root = root / "multitouch"
    for name in ("transport.py", "session.py", "windows_touchpad.py"):
        updates[module_root / name] = Path(__file__).with_name(name).read_text()
    updates[module_root / "__init__.py"] = '"""Opt-in, bounded Azoteq multitouch prototype."""\n'
    for path in updates:
        if path.parent == module_root and path.exists():
            raise FileExistsError(path)
    for path, content in updates.items():
        print(path)
        if args.apply:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)


if __name__ == "__main__":
    main()
