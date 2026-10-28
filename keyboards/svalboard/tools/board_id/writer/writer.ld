/* The ID-writer: one RAM image from 0x20000000. The boot ROM and picotool
   enter a RAM image at its lowest address, so the entry function comes first.
   The station patches the params at 0x20000100 and reads the report at
   0x20010000 (writer.h). Stay below 0x20010000 and clear of the boot ROM's
   scratch RAM at 0x20040000. */
MEMORY
{
    RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 64k
}

ENTRY(writer_entry)

SECTIONS
{
    .entry 0x20000000 : { KEEP(*(.entry)) } > RAM
    .params 0x20000100 : { KEEP(*(.params)) } > RAM
    .text 0x20000200 : { *(.text*) *(.rodata*) . = ALIGN(4); } > RAM
    .data : { *(.data*) . = ALIGN(4); } > RAM
    .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } > RAM
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
ASSERT(SIZEOF(.entry) <= 0x100, "the entry function must fit before the params")
ASSERT(SIZEOF(.params) == 0x100, "params are 256 bytes")
ASSERT(__bss_end__ <= 0x20010000, "the image must end below the report")
