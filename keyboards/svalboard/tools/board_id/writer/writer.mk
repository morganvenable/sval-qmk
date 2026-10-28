# The ID-writer (writer.h). Build in WSL with the Arm toolchain QMK uses:
#   make -C keyboards/svalboard/tools/board_id/writer -f writer.mk
# (Not "Makefile": QMK ignores keyboards/**/Makefile.) writer.uf2 is committed
# beside board_id.py (git add -f: *.uf2 is ignored) so the station needs no toolchain;
# tests/sval_board_id/run.sh rebuilds it and checks it matches.
CROSS ?= arm-none-eabi-
KB := ../../..
CFLAGS := -mcpu=cortex-m0plus -mthumb -Os -std=c11 -Wall -Wextra -Werror \
	-ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
	-ffunction-sections -fno-common -I. -I$(KB)
OUT ?= .

all: $(OUT)/writer.uf2

$(OUT)/writer.elf: writer_core.c hal_rp2040.c $(KB)/board_record.c writer.h $(KB)/board_record.h writer.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -Wl,-T,writer.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
		writer_core.c hal_rp2040.c $(KB)/board_record.c -lgcc -o $@

$(OUT)/writer.bin: $(OUT)/writer.elf
	$(CROSS)objcopy -O binary -R .bss $< $@

$(OUT)/writer.uf2: $(OUT)/writer.bin ../uf2.py
	python3 -I ../uf2.py ram $< $@

host: $(OUT)/writer_host

$(OUT)/writer_host: writer_core.c hal_host.c $(KB)/board_record.c writer.h $(KB)/board_record.h
	$${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -I. -I$(KB) writer_core.c hal_host.c $(KB)/board_record.c -o $@

.PHONY: all host
