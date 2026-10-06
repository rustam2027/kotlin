#include "DeltaMath.hpp"

#include "Logging.hpp"

#include <algorithm>
#include <sstream>

namespace kotlin::stackMap {

namespace {

/// Builds a RootLocation from an (unsigned) bit-vector index -- used for
/// the stack-slot bit vector.
RootLocation locationFromBitIndex(RootLocation::RootLocationType type, int32_t idx, uint64_t baseOffset) {
    switch (type) {
        case RootLocation::Indirect:
            return RootLocation::ConstructIndirect(static_cast<int32_t>(baseOffset - idx * 8));
        case RootLocation::Direct:
            return RootLocation::ConstructDirect(static_cast<int32_t>(baseOffset - idx * 8));
        default:
            RuntimeFail("locationFromBitIndex: Location wrong location type");
    }
}

std::vector<uint64_t> xorWords(const std::vector<uint64_t>& lhs, const std::vector<uint64_t>& rhs) {
    std::vector<uint64_t> result;
    size_t common = std::min(lhs.size(), rhs.size());

    for (size_t i = 0; i < common; ++i) {
        result.push_back(lhs[i] ^ rhs[i]);
    }
    for (size_t i = common; i < lhs.size(); ++i) {
        result.push_back(lhs[i]);
    }
    for (size_t i = common; i < rhs.size(); ++i) {
        result.push_back(rhs[i]);
    }
    return result;
}

} // namespace

Delta operator^(const Delta& lhs, const Delta& rhs) {
    Delta result;
    result.indirectSlots = xorWords(lhs.indirectSlots, rhs.indirectSlots);
    result.directSlots = xorWords(lhs.directSlots, rhs.directSlots);
    return result;
}

void Delta::log(std::vector<uint64_t>& vec) const {
    std::ostringstream oss;
    oss << "    [ ";
    for (auto elem : vec) {
        oss << elem << " ";
    }
    oss << "]";
    RuntimeLogDebug({kTagGC}, "%s", oss.str().c_str());
}

void Delta::logSlots() const {
    RuntimeLogDebug({kTagGC}, "    Delta.indirectSlots");
    log(const_cast<std::vector<uint64_t>&>(indirectSlots));
    RuntimeLogDebug({kTagGC}, "    Delta.directSlots");
    log(const_cast<std::vector<uint64_t>&>(directSlots));
}

RootsInfo Delta::toRootInfo(uint64_t baseOffset) const {
    RootsInfo result;

    auto traverseBitVector = [&result, baseOffset](const std::vector<uint64_t>& words, RootLocation::RootLocationType type) {
        for (size_t wordIdx = 0; wordIdx < words.size(); ++wordIdx) {
            uint64_t word = words[wordIdx];
            int32_t bitIdx = static_cast<int32_t>(wordIdx * 64);

            while (word != 0) {
                if (word & 1) {
                    result.addBase(locationFromBitIndex(type, bitIdx, baseOffset));
                }
                bitIdx++;
                word >>= 1;
            }
        }
    };

    traverseBitVector(indirectSlots, RootLocation::RootLocationType::Indirect);
    traverseBitVector(directSlots, RootLocation::RootLocationType::Direct);

    return result;
}

} // namespace kotlin::stackMap
