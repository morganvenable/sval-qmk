"""List Svalboards on this computer with the serial unified_lab.py needs."""
import hid

found = [d for d in hid.enumerate(0x303A, 0x4044) if d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]
for d in found:
    print(f"{d.get('serial_number')}  {d.get('product_string')}")
if not found:
    print("No Svalboard found. Plug in the half with the USB cable and try again.")
