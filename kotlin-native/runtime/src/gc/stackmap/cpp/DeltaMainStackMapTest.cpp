/*
 * Copyright 2010-2026 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license
 * that can be found in the LICENSE file.
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "DeltaMainStackMap.hpp"
#include "DeltaMath.hpp"
#include "VarIntCodec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "CompilerConstants.hpp"
#include "Utils.hpp"

using namespace kotlin::stackMap;

namespace {

std::vector<uint8_t> uleb128Bytes(uint64_t value) {
    std::vector<uint8_t> bytes;
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if (value != 0) byte |= 0x80;
        bytes.push_back(byte);
    } while (value != 0);
    return bytes;
}

std::vector<uint8_t> sleb128Bytes(int64_t value) {
    std::vector<uint8_t> bytes;
    bool more = true;
    while (more) {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        if ((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40))) {
            more = false;
        } else {
            byte |= 0x80;
        }
        bytes.push_back(byte);
    }
    return bytes;
}

} // namespace

TEST(DeltaMainVarIntCodecTest, ULEB128Roundtrip) {
    std::vector<uint64_t> values = {0, 1, 127, 128, 300, UINT64_MAX / 2, UINT64_MAX};
    for (uint64_t value : values) {
        std::vector<uint8_t> bytes = uleb128Bytes(value);
        ByteReader reader(bytes.data());
        EXPECT_EQ(reader.nextULEB128(), value) << "value=" << value;
    }
}

TEST(DeltaMainVarIntCodecTest, SLEB128Roundtrip) {
    std::vector<int64_t> values = {0, 1, -1, 63, -64, 64, -65, INT64_MIN, INT64_MAX};
    for (int64_t value : values) {
        std::vector<uint8_t> bytes = sleb128Bytes(value);
        ByteReader reader(bytes.data());
        EXPECT_EQ(reader.nextSLEB128(), value) << "value=" << value;
    }
}

TEST(DeltaMainVarIntCodecTest, NextIntSignExtends) {
    // A single byte 0xFF, read as a 1-byte signed integer, should sign-extend to -1.
    std::vector<uint8_t> bytes = {0xFF};
    ByteReader reader(bytes.data());
    EXPECT_EQ(reader.nextInt(1), -1);
}

TEST(DeltaMainDeltaTest, XorIsSelfInverse) {
    // D ^ D should decode to no live locations at all: this is the
    // property DeltaMainStackMapBuilder::Reader::getRootsInfo relies on
    // when a callsite's Delta equals its function's base Delta.
    Delta d;
    d.slots = {0b0110};

    Delta selfXor = d ^ d;
    EXPECT_TRUE(selfXor.slots.empty() || selfXor.slots[0] == 0);
}

TEST(DeltaMainDeltaTest, XorRecoversOriginalViaSecondXor) {
    // base ^ (base ^ other) should recover `other`'s live-location set --
    // this is exactly how a callsite's Delta is reconstructed from its
    // function's base Delta plus the stored per-callsite Delta.
    Delta base;
    base.slots = {0b1100};

    Delta other;
    other.slots = {0b1100};

    Delta diff = base ^ other;
    Delta recovered = base ^ diff;

    EXPECT_EQ(recovered.slots, other.slots);
}

TEST(DeltaMainDeltaTest, ToRootInfoDecodesStackSlotBits) {
    Delta d;
    d.slots = {0b101}; // bits 0 and 2 set: two stack slots.

    RootsInfo info = d.toRootInfo(/*baseOffset=*/64);

    // Two live base locations (bit 0 and bit 2 of the slot bit vector).
    EXPECT_EQ(info.totalCount(), 2u);
}

// --- The section as the compiler writes it ------------------------------------------------------
//
// The byte sequences below are what the delta-main emitter of the LLVM fork produces; the same
// layout is checked on the compiler side by CodeGen/AArch64/kn-delta-main-stackmap-format.ll:
//
//   magic            1 byte, DELTA_MAIN_VERSION << 4
//   function count   ULEB128
//   per function:
//     start          4 bytes, little endian, function address - section address
//     base           SLEB128, offset of slot 0 from the frame pointer
//     frame size     ULEB128
//     live set       ULEB128 bit vector, bit i is the slot at (base - 8 * i)
//     call count     ULEB128
//     calls          (return address - function address, delta index) as two ULEB128
//     deltas         one ULEB128 bit vector each; the live set of a call is (live set ^ delta)

// The decoder reads the section through the linker symbol `__LLVM_StackMaps`. A test defines it
// itself and puts the bytes of the current test into it.
extern "C" uint8_t gTestStackMapsSection[1024] __asm__("__LLVM_StackMaps");
uint8_t gTestStackMapsSection[1024];

namespace {

// Makes the runtime believe that it was compiled with the delta-main scheme.
class ScopedDeltaMainScheme : private kotlin::Pinned {
public:
    ScopedDeltaMainScheme() : saved_(Kotlin_gcStackMapScheme) { Kotlin_gcStackMapScheme = kotlin::compiler::DELTA_MAIN; }
    ~ScopedDeltaMainScheme() { Kotlin_gcStackMapScheme = saved_; }

private:
    const char* saved_;
};

std::vector<uint8_t> le32(uint32_t value) {
    return {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

// Installs the section, decodes it, and gives access to the decoded call sites.
class DecodedSection {
public:
    explicit DecodedSection(const std::vector<uint8_t>& bytes) {
        std::memset(gTestStackMapsSection, 0, sizeof(gTestStackMapsSection));
        std::copy(bytes.begin(), bytes.end(), gTestStackMapsSection);
        builder_.emplace();
        builder_->collect();
    }

    static uint64_t* address(uint64_t offsetInSection) {
        return reinterpret_cast<uint64_t*>(gTestStackMapsSection + offsetInSection);
    }

    bool has(uint64_t offsetInSection) { return builder_->hasMapForPC(address(offsetInSection)); }

    std::vector<int32_t> offsets(uint64_t offsetInSection) {
        std::vector<int32_t> result;
        for (const auto& root : builder_->getRootsInfoForPC(address(offsetInSection)).bases()) {
            EXPECT_EQ(root.Type, RootLocation::Indirect);
            result.push_back(root.Offset);
        }
        return result;
    }

    size_t callCount() { return builder_->pc2RootsInfo().size(); }

private:
    ScopedDeltaMainScheme scheme_;
    // The constructor of the builder already reads the magic byte of the section (and dies on a
    // wrong one), so it must run after the bytes are installed: a plain member would be
    // constructed before the body of the constructor of this class.
    std::optional<DeltaMainStackMapBuilder> builder_;
};

} // namespace

TEST(DeltaMainDecoderTest, DecodesTwoCallsWithShrinkingLiveSet) {
    // Output of llc for a function that has two slots live at the first call and one at the second.
    std::vector<uint8_t> section = concat({
            {0x40},      // magic: version 4
            {1},         // one function
            le32(0x100), // starts at section + 0x100
            {0x78},      // base: SLEB128 0x78 = -8
            {32},        // frame size
            {0b11},      // live set: slots 0 and 1
            {2},         // two calls
            {12, 0},     // pc +12, delta 0
            {20, 1},     // pc +20, delta 1
            {0},         // delta 0: empty
            {0b01},      // delta 1: slot 0 is dead
    });

    DecodedSection decoded(section);

    EXPECT_EQ(decoded.callCount(), 2u);
    ASSERT_TRUE(decoded.has(0x100 + 12));
    ASSERT_TRUE(decoded.has(0x100 + 20));
    EXPECT_THAT(decoded.offsets(0x100 + 12), testing::ElementsAre(-8, -16));
    EXPECT_THAT(decoded.offsets(0x100 + 20), testing::ElementsAre(-16));
    EXPECT_FALSE(decoded.has(0x100 + 16));
}

TEST(DeltaMainDecoderTest, CallWithoutRootsIsStillAKnownFrame) {
    // The walker steps over frames that have no entry, so a Kotlin frame with no live
    // references must have an (empty) entry.
    std::vector<uint8_t> section = concat({
            {0x40}, {1}, le32(0x80), {0}, {16}, {0}, // no slots at all
            {1},                                      // one call
            {10, 0},
            {0},
    });

    DecodedSection decoded(section);

    ASSERT_TRUE(decoded.has(0x80 + 10));
    EXPECT_THAT(decoded.offsets(0x80 + 10), testing::IsEmpty());
}

TEST(DeltaMainDecoderTest, LiveSetLongerThanOneByte) {
    // Eight slots: the bit vector is 7 bits per byte, 0xFF 0x01 = bits 0..7.
    std::vector<uint8_t> section = concat({
            {0x40}, {1}, le32(0x100), {0x78}, {0x90, 0x01}, // frame size 144 (two bytes)
            {0xFF, 0x01},                                   // eight live slots
            {1},
            {8, 0},
            {0},
    });

    DecodedSection decoded(section);

    ASSERT_TRUE(decoded.has(0x100 + 8));
    EXPECT_THAT(decoded.offsets(0x100 + 8), testing::ElementsAre(-8, -16, -24, -32, -40, -48, -56, -64));
}

TEST(DeltaMainDecoderTest, MoreThanSixtyFourSlots) {
    // Slots 0 and 70: the bit vector is more than one 64-bit word.
    // 70 = 7 * 10 + 0: bit 70 is bit 0 of the 11th ULEB128 byte.
    std::vector<uint8_t> liveSet(11, 0x80);
    liveSet[0] = 0x81; // slot 0, continues
    liveSet[10] = 0x01; // slot 70, last byte (no continuation flag)
    std::vector<uint8_t> section = concat({
            {0x40}, {1}, le32(0x100), {0x78}, {0xC0, 0x04}, // frame size 576
            liveSet,
            {1},
            {8, 0},
            {0},
    });

    DecodedSection decoded(section);

    ASSERT_TRUE(decoded.has(0x100 + 8));
    EXPECT_THAT(decoded.offsets(0x100 + 8), testing::ElementsAre(-8, -8 - 8 * 70));
}

TEST(DeltaMainDecoderTest, BaseOffsetOfSeveralBytes) {
    // -200 as SLEB128 is 0xB8 0x7E.
    std::vector<uint8_t> section = concat({
            {0x40}, {1}, le32(0x100), {0xB8, 0x7E}, {0xF0, 0x01}, {0b101}, {1}, {8, 0}, {0},
    });

    DecodedSection decoded(section);

    EXPECT_THAT(decoded.offsets(0x100 + 8), testing::ElementsAre(-200, -216));
}

TEST(DeltaMainDecoderTest, SeveralFunctions) {
    std::vector<uint8_t> section = concat({
            {0x40},
            {2},
            // function 1
            le32(0x100), {0x78}, {32}, {0b1}, {1}, {8, 0}, {0},
            // function 2
            le32(0x200), {0x70}, {48}, {0b11}, {2}, {4, 0}, {12, 1}, {0}, {0b10},
    });

    DecodedSection decoded(section);

    EXPECT_EQ(decoded.callCount(), 3u);
    EXPECT_THAT(decoded.offsets(0x100 + 8), testing::ElementsAre(-8));
    EXPECT_THAT(decoded.offsets(0x200 + 4), testing::ElementsAre(-16, -24));
    // Function 2: base 0x70 as SLEB128 is -16, so slot i is at -16 - 8 * i. The live set of the
    // second call is 0b11 ^ 0b10 = 0b01: slot 1 (-24) is dead, slot 0 (-16) stays.
    EXPECT_THAT(decoded.offsets(0x200 + 12), testing::ElementsAre(-16));
}

TEST(DeltaMainDecoderTest, WrongVersionIsFatal) {
    // The compiler and the runtime must agree on the format version: a runtime that reads
    // another format must not guess.
    std::vector<uint8_t> section = concat({{0x50}, {0}});

    EXPECT_DEATH(DecodedSection decoded(section), "magic mismatch");
}

TEST(DeltaMainDecoderTest, LazyBitInTheMagicIsFatal) {
    // The lazy mode is not implemented: its bit must not be set.
    std::vector<uint8_t> section = concat({{0x42}, {0}});

    EXPECT_DEATH(DecodedSection decoded(section), "magic mismatch");
}
