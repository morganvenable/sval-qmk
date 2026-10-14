"""Render a captured JSONL stream as a self-contained, offline HTML replay."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--blank-wipe', action='store_true', help='Label a recording made with the verified blank/wipe build')
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.capture.read_text().splitlines() if line.strip()]
    if not rows:
        parser.error('Empty capture')
    start = rows[0]['timestamp']
    data = [[((r['timestamp'] - start) & 0xffffffff) / 1e6, r['sequence'], r['epoch'],
             r['flags'], r['mask'], r['contacts']] for r in rows]
    template = Path(__file__).with_name('replay.html').read_text()
    if args.blank_wipe:
        template = template.replace(
            'This session used a nonblank keymap on a bare MCU. Keyboard activity confounds the failure measurements. Repeat with the blank keymap before assigning a cause. Gaps are not interpolated.',
            'Recorded with the blank/wipe firmware and 64-byte HID reports. All 1,920 keymap bytes were verified zero before testing. Gaps and invalid acquisitions remain visible; missing motion is not interpolated.')
    maximum = max((r['mask'].bit_count() for r in rows if not r['flags'] & 127), default=0)
    template = template.replace('<div class="number">5</div>maximum contacts',
                                f'<div class="number">{maximum}</div>maximum contacts')
    args.output.write_text(template.replace('/*CAPTURE_DATA*/[]', json.dumps(data, separators=(',', ':'))))
    print(args.output.resolve())


if __name__ == '__main__':
    main()
