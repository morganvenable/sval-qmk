/* Settings start at 0x160000 (config.h); firmware must stay below them. */
ASSERT(__flash_binary_end <= 0x10160000, "Svalboard firmware overlaps the settings flash at 0x160000")
