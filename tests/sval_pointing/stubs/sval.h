#pragma once
#include <stdbool.h>
bool sval_storage_write_failed(void);
// Pointing tools commands; must match modules/svalboard/core/sval.h.
enum { sval_cmd_point_tools_get = 0x34, sval_cmd_point_tools_set = 0x35, sval_cmd_point_tools_status = 0x36, sval_cmd_point_tools_stop = 0x37 };
