"""Exercise the real Keybard profile reader alongside the touch stream on Windows."""
import argparse
import json
from pathlib import Path
import sys
import time

from capture import open_device
from session import TouchSession


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--keybard', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.exists():
        p.error('Output already exists')
    sys.path.insert(0, str(args.keybard / 'companion/overlay-host'))
    from keybard_host.device.protocol import SvalReader
    messages = []
    device = open_device(args.serial)
    session = TouchSession(device, status=messages.append)
    started = time.monotonic()
    result = {'serial': args.serial}
    try:
        reader = SvalReader(session.dispatcher)
        version, uid = reader.info()
        reader.read_profile(uid)
        result.update(version=version, uid=uid, profile_read=True,
                      profile_seconds=time.monotonic() - started)
        result['layer_states'] = [reader.layer_state() for _ in range(25)]
        time.sleep(2)
    finally:
        session.close()
        device.close()
        result['messages'] = messages
        args.output.write_text(json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
