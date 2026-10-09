#!/usr/bin/env python3
"""Assign one experimental pointing binding, with a reversible per-key backup.

Run on the USB host with Python and hidapi; close Keybard while using this tool.
No flashing, layout reset, or changes to other keys. Requires point-controls firmware.
"""
import argparse
import json
from pathlib import Path
from keytest import Device, devices

BINDINGS = {
    f'{side}-{control}-{activation}': 0x7E00 + side_index * 8 + control_index * 2 + activation_index
    for side_index, side in enumerate(('left', 'right'))
    for control_index, control in enumerate(('volume', 'arrows', 'tabs', 'tasks'))
    for activation_index, activation in enumerate(('hold', 'toggle'))
}
BINDINGS.update(cancel=0x7E10, trace=0x7E11, finer=0x7E12, coarser=0x7E13)

def assign(device, layer, row, col, binding, backup_path):
    original = device.keymap(layer, row, col)
    assigned = BINDINGS[binding]
    backup = dict(serial=device.serial, layer=layer, row=row, col=col, original=original, assigned=assigned, binding=binding)
    # Refuse overwrite; recovery information is durable before the first write.
    with Path(backup_path).open('x') as output:
        json.dump(backup, output, indent=2)
        output.write('\n')
        output.flush()
        import os
        os.fsync(output.fileno())
    device.keymap(layer, row, col, assigned)
    if device.keymap(layer, row, col) != assigned:
        raise RuntimeError(f'Readback failed; retain {backup_path} for recovery')
    return backup

def restore(device, backup_path):
    backup = json.loads(Path(backup_path).read_text())
    if backup['serial'] != device.serial:
        raise ValueError('Backup belongs to a different device')
    pos = backup['layer'], backup['row'], backup['col']
    current = device.keymap(*pos)
    if current not in (backup['original'], backup['assigned']):
        raise ValueError('This key changed since assignment; refusing to overwrite the later edit')
    device.keymap(*pos, backup['original'])
    if device.keymap(*pos) != backup['original']:
        raise RuntimeError('Restore readback failed; retain the backup')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('list')
    show = commands.add_parser('codes')
    for command in ('assign', 'restore'):
        sub = commands.add_parser(command)
        sub.add_argument('--serial', required=True)
        sub.add_argument('--backup', required=True, type=Path)
        if command == 'assign':
            sub.add_argument('--layer', required=True, type=int, choices=range(16))
            sub.add_argument('--row', required=True, type=int, choices=range(10))
            sub.add_argument('--col', required=True, type=int, choices=range(6))
            sub.add_argument('--binding', required=True, choices=BINDINGS)
    args = parser.parse_args()
    if args.command == 'list':
        print(json.dumps([{'serial': d.get('serial_number'), 'vid': d.get('vendor_id'), 'pid': d.get('product_id')} for d in devices()], indent=2))
        return
    if args.command == 'codes':
        print('\n'.join(f'{name:26} 0x{code:04X}' for name, code in BINDINGS.items()))
        return
    device = Device(args.serial)
    try:
        if args.command == 'assign':
            assign(device, args.layer, args.row, args.col, args.binding, args.backup)
            print(f'Assigned {args.binding}; verified readback. Restore with {args.backup}.')
        else:
            restore(device, args.backup)
            print('Original binding restored; verified readback.')
    finally:
        device.close()

if __name__ == '__main__':
    main()
