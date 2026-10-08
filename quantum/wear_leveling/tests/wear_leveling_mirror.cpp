// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include <numeric>
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "backing_mocks.hpp"

using Data = std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE>;

static constexpr std::size_t COPY_ELEMENTS  = WEAR_LEVELING_BACKING_SIZE / sizeof(backing_store_int_t);
static constexpr std::size_t IMAGE_ELEMENTS = WEAR_LEVELING_LOGICAL_SIZE / sizeof(backing_store_int_t);

class WearLevelingMirror : public ::testing::Test {
   protected:
    void SetUp() override {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
    }

    static MockBackingStoreElement& element(std::size_t copy, std::size_t index) {
        return *(MockBackingStore::Instance().storage_begin() + copy * COPY_ELEMENTS + index);
    }

    static void damage(std::size_t copy, std::size_t index) {
        backing_store_int_t value = ~element(copy, index).get();
        element(copy, index).erase();
        element(copy, index).set(~(value ^ 0x5A));
    }

    static bool copies_equal() {
        for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
            if (element(0, i).get() != element(1, i).get()) {
                return false;
            }
        }
        return true;
    }

    // Consolidated data plus one write log entry.
    static Data seed() {
        Data data;
        std::iota(data.begin(), data.end(), 0x20);
        EXPECT_EQ(wear_leveling_write(0, data.data(), data.size()), WEAR_LEVELING_CONSOLIDATED) << "Write returned incorrect status";
        data[3] = 0x77;
        EXPECT_EQ(wear_leveling_write(3, &data[3], 1), WEAR_LEVELING_SUCCESS) << "Write returned incorrect status";
        return data;
    }

    static Data stored() {
        Data data;
        EXPECT_EQ(wear_leveling_read(0, data.data(), data.size()), WEAR_LEVELING_SUCCESS) << "Failed to read";
        return data;
    }

    static void expect_recovered(const Data& expected) {
        EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
        EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
        EXPECT_EQ(stored(), expected) << "Stored data should have been kept";
        EXPECT_TRUE(copies_equal()) << "Copies should match after init";
    }
};

/**
 * This test verifies that every write reaches both copies.
 */
TEST_F(WearLevelingMirror, WritesReachEveryCopy) {
    seed();
    EXPECT_TRUE(copies_equal()) << "Copies should match";
}

/**
 * This test verifies that a healthy store is loaded without rewriting anything.
 */
TEST_F(WearLevelingMirror, HealthyStore_NoRewrite) {
    auto     data        = seed();
    uint64_t erase_count = MockBackingStore::Instance().erase_invoke_count();
    expect_recovered(data);
    EXPECT_EQ(MockBackingStore::Instance().erase_invoke_count(), erase_count) << "Nothing should have been erased";
}

/**
 * This test verifies that damage to the first copy is recovered from the second, and the first repaired.
 */
TEST_F(WearLevelingMirror, DamagedFirstCopy_LoadsSecond) {
    auto data = seed();
    damage(0, 2);
    expect_recovered(data);
}

/**
 * This test verifies that damage to the second copy is repaired from the first.
 */
TEST_F(WearLevelingMirror, DamagedSecondCopy_Repaired) {
    auto data = seed();
    damage(1, IMAGE_ELEMENTS); // its checksum
    expect_recovered(data);
}

/**
 * This test verifies that a first copy left erased by an interrupted consolidation does not hide the second.
 */
TEST_F(WearLevelingMirror, ErasedFirstCopy_LoadsSecond) {
    auto data = seed();
    for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
        element(0, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that a second copy missing the last log entry, from power loss between the two writes, is
 * repaired with it.
 */
TEST_F(WearLevelingMirror, LaggingSecondCopy_Repaired) {
    auto data = seed();
    for (std::size_t i = IMAGE_ELEMENTS + 4; i < COPY_ELEMENTS; ++i) {
        element(1, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that when neither copy can be read, the store is reset and the loss reported.
 */
TEST_F(WearLevelingMirror, BothCopiesDamaged_Reset) {
    seed();
    damage(0, 2);
    damage(1, 2);
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_CONSOLIDATED) << "Init returned incorrect status";
    EXPECT_TRUE(wear_leveling_data_lost()) << "Lost data should have been reported";
    EXPECT_EQ(stored(), Data{}) << "Store should have been reset";
    EXPECT_TRUE(copies_equal()) << "Copies should match after reset";
}

/**
 * This test verifies that a bad read of the first copy is retried rather than rewriting both copies.
 */
TEST_F(WearLevelingMirror, TransientBadRead_NoRewrite) {
    auto& inst      = MockBackingStore::Instance();
    auto  data      = seed();
    bool  corrupted = false;
    inst.set_read_callback([&](std::uint32_t address, backing_store_int_t& value) {
        if (address == 0 && !corrupted) {
            corrupted = true;
            value ^= 1;
        }
    });
    uint64_t erase_count = inst.erase_invoke_count();
    expect_recovered(data);
    EXPECT_TRUE(corrupted) << "Test did not corrupt a read";
    EXPECT_EQ(inst.erase_invoke_count(), erase_count) << "Nothing should have been erased";
}

/**
 * This test verifies that power loss at any write or erase of a consolidating write leaves every byte either old or
 * new, never reports lost data, and leaves a store that keeps working.
 */
TEST_F(WearLevelingMirror, PowerLossDuringConsolidation) {
    for (int failing_erase = 0; failing_erase <= 1; ++failing_erase) {
        for (std::uint64_t step = 1;; ++step) {
            auto& inst = MockBackingStore::Instance();
            inst.reset_instance();
            wear_leveling_init();
            auto before = seed();
            Data after;
            std::iota(after.begin(), after.end(), 0x80);

            std::uint64_t writes = inst.write_invoke_count(), erases = inst.erase_invoke_count();
            bool          cut    = false;
            if (failing_erase) {
                inst.set_erase_callback([&](std::uint64_t count) { return !(cut = cut || count - erases == step); });
            } else {
                inst.set_write_callback([&](std::uint64_t count, std::uint32_t) { return !(cut = cut || count - writes == step); });
            }
            wear_leveling_write(0, after.data(), after.size());
            inst.set_erase_callback(nullptr);
            inst.set_write_callback(nullptr);
            if (!cut) {
                break; // every step has been interrupted once
            }

            EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed at step " << step;
            EXPECT_FALSE(wear_leveling_data_lost()) << "Data lost at step " << step;
            auto data = stored();
            for (std::size_t i = 0; i < data.size(); ++i) {
                EXPECT_TRUE(data[i] == before[i] || data[i] == after[i]) << "Byte " << i << " corrupted at step " << step;
            }
            EXPECT_TRUE(copies_equal()) << "Copies differ at step " << step;
            expect_recovered(data);
            EXPECT_EQ(wear_leveling_write(0, after.data(), after.size()) == WEAR_LEVELING_FAILED, false) << "Store stopped working at step " << step;
            expect_recovered(after);
        }
    }
}

#if WEAR_LEVELING_LOGICAL_SIZE > 64
static constexpr std::size_t LOG_START = IMAGE_ELEMENTS + 8 / sizeof(backing_store_int_t);

// First erased log slot of a copy.
static std::size_t log_end(std::size_t copy) {
    std::size_t i = LOG_START;
    while (i < COPY_ELEMENTS && !(MockBackingStore::Instance().storage_begin() + copy * COPY_ELEMENTS + i)->is_erased()) {
        ++i;
    }
    return i;
}

/**
 * This test verifies that a multi-word log entry cut off by the end of a copy, from power loss while it was written,
 * ends the log instead of being completed with whatever follows the copy.
 */
TEST_F(WearLevelingMirror, EntryCutOffAtCopyEnd_Ignored) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    for (std::size_t copy = 0; copy < 2; ++copy) {
        // Fill the log with harmless entries, leaving only the last slot
        for (std::size_t i = log_end(copy); i + 1 < COPY_ELEMENTS; ++i) {
            element(copy, i).set(~LOG_ENTRY_MAKE_OPTIMIZED_64(0, data[0]).raw16[0]);
        }
        // Only the first word of a write to address 100 made it into the last slot
        element(copy, COPY_ELEMENTS - 1).set(~LOG_ENTRY_MAKE_MULTIBYTE(100, 1).raw16[0]);
    }
    // Power was lost after consolidation had erased the first copy
    for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
        element(0, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that a copy which replays completely wins over one whose log stops at an invalid entry.
 */
TEST_F(WearLevelingMirror, TruncatedFirstCopy_LoadsSecond) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    std::size_t slot = log_end(0);
    element(0, slot).set(0x3FFF);                                             // invalid entry type in the first copy
    element(1, slot).set(~LOG_ENTRY_MAKE_OPTIMIZED_64(5, 0x99).raw16[0]); // a real entry in the second
    data[5] = 0x99;
    expect_recovered(data);
}

/**
 * This test verifies that when every copy's log stops at an invalid entry, the entries before it are kept and
 * nothing is reported lost.
 */
TEST_F(WearLevelingMirror, BothCopiesTruncated_PrefixKept) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    std::size_t slot = log_end(0);
    element(0, slot).set(0x3FFF);
    element(1, slot).set(0x3FFF);
    expect_recovered(data);
}
#endif
