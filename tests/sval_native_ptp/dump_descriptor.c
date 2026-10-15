// Print the native touchpad report descriptor (ptp_descriptor.h) as hex, so
// the Python tests parse exactly the bytes the firmware is built from.
#include <stdint.h>
#include <stdio.h>
#include "../../keyboards/svalboard/azoteq/ptp_descriptor.h"

static const uint8_t descriptor[] = {SVAL_PTP_REPORT_DESCRIPTOR};

int main(void) {
    for (size_t i = 0; i < sizeof(descriptor); i++)
        printf("%02x", descriptor[i]);
    printf("\n");
    return 0;
}
