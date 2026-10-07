/* Reject firmware that would overwrite the read-only upgrade source. */
ASSERT(__flash_binary_end <= 0x10160000, "Svalboard firmware overlaps reserved settings flash")
