#pragma once
typedef enum { USB_PROTOCOL_BOOT, USB_PROTOCOL_REPORT } usb_hid_protocol_t;
enum { USB_DEVICE_STATE_CONFIGURED = 2, USB_DEVICE_STATE_SUSPEND = 3 };
int usb_device_state_get_configure_state(void);
usb_hid_protocol_t usb_device_state_get_protocol(void);
