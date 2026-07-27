// EdidPatch.h - pure EDID parsing and Microsoft-VSDB patching logic.
//
// Header-only and free of Windows/registry dependencies so the self-test can
// exercise exactly the code that `apply` runs, without writing anything.
//
// References for the byte layout (see README.md for URLs):
//   * CTA-861 data block: byte 0 = (tag << 5) | payload_length.
//   * Microsoft VSDB, tag 3 (Vendor Specific), payload length 21 -> header 0x75:
//       [0]     0x75
//       [1..3]  IEEE OUI, raw bytes 5C 12 CA in that order
//       [4]     version (0x03 for non-HMD specialized displays)
//       [5]     bit6 = desktop usage, bit5 = third-party usage,
//               bits4-0 = primary use case
//       [6..21] Container ID (16-byte UUID)
//     Total 22 bytes. Windows treats version 3 with desktop-usage CLEAR as
//     "non-desktop", which is what makes the display specialized.
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace edid {

using Bytes = std::vector<uint8_t>;

constexpr size_t kBlockSize = 128;
constexpr size_t kVsdbSize = 22;
constexpr uint8_t kVsdbHeader = 0x75;          // tag 3, payload length 21
constexpr uint8_t kOuiByte0 = 0x5C;
constexpr uint8_t kOuiByte1 = 0x12;
constexpr uint8_t kOuiByte2 = 0xCA;
constexpr uint8_t kExtTagCta = 0x02;

// ---------------------------------------------------------------------------
// Basic block helpers
// ---------------------------------------------------------------------------

inline size_t BlockCount(const Bytes& e) { return e.size() / kBlockSize; }

inline bool HasValidHeader(const Bytes& e) {
    static const uint8_t kMagic[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (e.size() < kBlockSize) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (e[i] != kMagic[i]) return false;
    }
    return true;
}

// The value byte 127 must hold so the block sums to 0 mod 256.
inline uint8_t ComputeChecksum(const uint8_t* block) {
    unsigned sum = 0;
    for (size_t i = 0; i < kBlockSize - 1; ++i) sum += block[i];
    return static_cast<uint8_t>((256u - (sum & 0xFFu)) & 0xFFu);
}

inline bool BlockChecksumOk(const uint8_t* block) {
    unsigned sum = 0;
    for (size_t i = 0; i < kBlockSize; ++i) sum += block[i];
    return (sum & 0xFFu) == 0u;
}

inline void FixBlockChecksum(uint8_t* block) {
    block[kBlockSize - 1] = ComputeChecksum(block);
}

inline bool AllChecksumsOk(const Bytes& e) {
    if (e.empty() || (e.size() % kBlockSize) != 0) return false;
    for (size_t b = 0; b < BlockCount(e); ++b) {
        if (!BlockChecksumOk(e.data() + b * kBlockSize)) return false;
    }
    return true;
}

// Byte 126 of the base block: number of extension blocks that follow.
inline uint8_t DeclaredExtensionCount(const Bytes& e) {
    return (e.size() >= kBlockSize) ? e[126] : 0;
}

// ---------------------------------------------------------------------------
// CTA-861 extension parsing
// ---------------------------------------------------------------------------

struct CtaBlockInfo {
    bool   found = false;
    size_t blockIndex = 0;     // 1-based extension index within the EDID
    size_t offset = 0;         // byte offset of the block start
    uint8_t revision = 0;
    uint8_t dtdStart = 0;      // byte 2: offset of first DTD within the block
    size_t dataBlockBytes = 0; // bytes 4 .. dtdStart-1
    size_t dtdCount = 0;
    size_t paddingStart = 0;   // first unused byte within the block
    size_t freeBytes = 0;      // paddingStart .. 126
};

// A DTD is 18 bytes; the DTD list ends at a zero pixel-clock or the block end.
inline CtaBlockInfo FindFirstCtaBlock(const Bytes& e) {
    CtaBlockInfo info;
    for (size_t b = 1; b < BlockCount(e); ++b) {
        const size_t off = b * kBlockSize;
        if (e[off] != kExtTagCta) continue;

        info.found = true;
        info.blockIndex = b;
        info.offset = off;
        info.revision = e[off + 1];
        info.dtdStart = e[off + 2];

        // dtdStart == 0 means "no DTDs and no data block collection".
        const size_t dtdStart = (info.dtdStart == 0) ? kBlockSize - 1 : info.dtdStart;
        info.dataBlockBytes = (dtdStart > 4) ? (dtdStart - 4) : 0;

        size_t p = dtdStart;
        while (p + 18 <= kBlockSize - 1) {
            if (e[off + p] == 0 && e[off + p + 1] == 0) break;   // zero pixel clock
            ++info.dtdCount;
            p += 18;
        }
        info.paddingStart = p;
        info.freeBytes = (kBlockSize - 1 > p) ? (kBlockSize - 1 - p) : 0;
        return info;
    }
    return info;
}

// Walks the CTA data block collection, returning each block as its own byte run.
inline bool ExtractCtaDataBlocks(const Bytes& e, const CtaBlockInfo& cta,
                                 std::vector<Bytes>& out) {
    out.clear();
    if (!cta.found || cta.dtdStart == 0) return true;   // nothing to walk
    if (cta.dtdStart < 4 || cta.dtdStart > kBlockSize - 1) return false;

    size_t p = 4;
    while (p < cta.dtdStart) {
        const uint8_t header = e[cta.offset + p];
        const size_t len = static_cast<size_t>(header & 0x1F);
        if (p + 1 + len > cta.dtdStart) return false;    // runs past the collection
        out.emplace_back(e.begin() + static_cast<ptrdiff_t>(cta.offset + p),
                         e.begin() + static_cast<ptrdiff_t>(cta.offset + p + 1 + len));
        p += 1 + len;
    }
    return p == cta.dtdStart;
}

inline void ExtractCtaDtds(const Bytes& e, const CtaBlockInfo& cta,
                           std::vector<Bytes>& out) {
    out.clear();
    if (!cta.found) return;
    const size_t start = (cta.dtdStart == 0) ? kBlockSize - 1 : cta.dtdStart;
    for (size_t i = 0; i < cta.dtdCount; ++i) {
        const size_t p = cta.offset + start + i * 18;
        out.emplace_back(e.begin() + static_cast<ptrdiff_t>(p),
                         e.begin() + static_cast<ptrdiff_t>(p + 18));
    }
}

// ---------------------------------------------------------------------------
// Microsoft VSDB
// ---------------------------------------------------------------------------

struct VsdbLocation {
    bool   found = false;
    size_t offset = 0;      // offset of the data block header byte
    uint8_t version = 0;
    uint8_t flags = 0;
    bool   desktopUsage = false;
};

// Scans every CTA extension's data block collection for the Microsoft OUI.
inline VsdbLocation FindMicrosoftVsdb(const Bytes& e) {
    VsdbLocation loc;
    for (size_t b = 1; b < BlockCount(e); ++b) {
        const size_t off = b * kBlockSize;
        if (e[off] != kExtTagCta) continue;
        const uint8_t dtdStart = e[off + 2];
        const size_t end = (dtdStart == 0) ? 4 : dtdStart;
        if (end < 4 || end > kBlockSize - 1) continue;

        size_t p = 4;
        while (p < end) {
            const uint8_t header = e[off + p];
            const size_t len = static_cast<size_t>(header & 0x1F);
            const uint8_t tag = static_cast<uint8_t>((header >> 5) & 0x07);
            if (p + 1 + len > end) break;
            if (tag == 3 && len >= 5 && e[off + p + 1] == kOuiByte0 &&
                e[off + p + 2] == kOuiByte1 && e[off + p + 3] == kOuiByte2) {
                loc.found = true;
                loc.offset = off + p;
                loc.version = e[off + p + 4];
                loc.flags = e[off + p + 5];
                loc.desktopUsage = (loc.flags & 0x40) != 0;
                return loc;
            }
            p += 1 + len;
        }
    }
    return loc;
}

struct VsdbOptions {
    uint8_t version = 0x03;          // 3 = specialized, non-HMD
    bool    desktopUsage = false;    // MUST be false to mark the display non-desktop
    bool    thirdPartyUsage = false; // bit 5
    uint8_t primaryUseCase = 0x02;   // 2 = generic display
    std::array<uint8_t, 16> containerId{};
};

inline Bytes BuildMicrosoftVsdb(const VsdbOptions& o) {
    Bytes v(kVsdbSize, 0);
    v[0] = kVsdbHeader;
    v[1] = kOuiByte0;
    v[2] = kOuiByte1;
    v[3] = kOuiByte2;
    v[4] = o.version;
    uint8_t flags = static_cast<uint8_t>(o.primaryUseCase & 0x1F);
    if (o.desktopUsage)    flags |= 0x40;
    if (o.thirdPartyUsage) flags |= 0x20;
    v[5] = flags;
    for (size_t i = 0; i < 16; ++i) v[6 + i] = o.containerId[i];
    return v;
}

// Derives a stable Container ID from a device identifier so re-running `apply`
// produces byte-identical output (FNV-1a expanded, stamped as UUID v4).
inline std::array<uint8_t, 16> DeriveContainerId(const std::string& seed) {
    std::array<uint8_t, 16> id{};
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : seed) {
        h ^= c;
        h *= 1099511628211ull;
    }
    for (size_t i = 0; i < 16; ++i) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        id[i] = static_cast<uint8_t>(h >> 56);
    }
    id[6] = static_cast<uint8_t>((id[6] & 0x0F) | 0x40);   // version 4
    id[8] = static_cast<uint8_t>((id[8] & 0x3F) | 0x80);   // RFC 4122 variant
    return id;
}

// ---------------------------------------------------------------------------
// Patching
// ---------------------------------------------------------------------------

enum class PatchStrategy {
    None,
    InsertIntoExistingCta,   // room in the first CTA block: shift DTDs down
    AppendNewCtaBlock,       // no room: add a whole new CTA extension block
};

inline const char* StrategyName(PatchStrategy s) {
    switch (s) {
    case PatchStrategy::InsertIntoExistingCta: return "insert into existing CTA block";
    case PatchStrategy::AppendNewCtaBlock:     return "append new CTA extension block";
    default:                                   return "none";
    }
}

struct PatchResult {
    bool          ok = false;
    std::string   error;
    PatchStrategy strategy = PatchStrategy::None;
    Bytes         output;
};

inline PatchResult AddMicrosoftVsdb(const Bytes& input, const VsdbOptions& opts) {
    PatchResult r;
    if (input.empty() || (input.size() % kBlockSize) != 0) {
        r.error = "EDID length is not a multiple of 128";
        return r;
    }
    if (!HasValidHeader(input)) {
        r.error = "EDID base block header is not 00 FF FF FF FF FF FF 00";
        return r;
    }
    if (FindMicrosoftVsdb(input).found) {
        r.error = "a Microsoft VSDB is already present";
        return r;
    }
    if (BlockCount(input) >= 255) {
        r.error = "EDID already has the maximum number of extension blocks";
        return r;
    }

    const Bytes vsdb = BuildMicrosoftVsdb(opts);
    const CtaBlockInfo cta = FindFirstCtaBlock(input);

    // Strategy A: enough unused space after the DTDs of the existing CTA block.
    if (cta.found && cta.dtdStart >= 4 && cta.freeBytes >= kVsdbSize) {
        r.output = input;
        uint8_t* blk = r.output.data() + cta.offset;
        const size_t dtdStart = cta.dtdStart;
        const size_t tailBytes = cta.paddingStart - dtdStart;   // the DTD run

        // Shift the DTD run down to make room, then drop the VSDB in.
        for (size_t i = tailBytes; i > 0; --i) {
            blk[dtdStart + kVsdbSize + i - 1] = blk[dtdStart + i - 1];
        }
        for (size_t i = 0; i < kVsdbSize; ++i) blk[dtdStart + i] = vsdb[i];
        for (size_t i = cta.paddingStart + kVsdbSize; i < kBlockSize - 1; ++i) {
            blk[i] = 0;
        }
        blk[2] = static_cast<uint8_t>(dtdStart + kVsdbSize);
        FixBlockChecksum(blk);
        r.ok = true;
        r.strategy = PatchStrategy::InsertIntoExistingCta;
        return r;
    }

    // Strategy B: append a fresh CTA-861 rev 3 extension block holding only the
    // VSDB. Only byte 126 (extension count) and byte 127 (checksum) of the base
    // block change; every existing extension block is untouched.
    r.output = input;
    r.output.resize(input.size() + kBlockSize, 0);
    uint8_t* blk = r.output.data() + input.size();
    blk[0] = kExtTagCta;
    blk[1] = 0x03;                                            // CTA revision 3
    blk[2] = static_cast<uint8_t>(4 + kVsdbSize);              // no DTDs follow
    blk[3] = 0x00;                                            // no native formats
    for (size_t i = 0; i < kVsdbSize; ++i) blk[4 + i] = vsdb[i];
    FixBlockChecksum(blk);

    r.output[126] = static_cast<uint8_t>(r.output[126] + 1);
    FixBlockChecksum(r.output.data());

    r.ok = true;
    r.strategy = PatchStrategy::AppendNewCtaBlock;
    return r;
}

// ---------------------------------------------------------------------------
// Round-trip verification (used by `selftest`)
// ---------------------------------------------------------------------------

struct RoundTripReport {
    bool        ok = false;
    std::string error;
    size_t      differingBytes = 0;
    size_t      preservedDataBlocks = 0;
    size_t      preservedDtds = 0;
};

// Asserts the patched EDID differs from the original ONLY by the added VSDB,
// the bookkeeping bytes that adding it requires, and the checksums.
inline RoundTripReport VerifyRoundTrip(const Bytes& before, const Bytes& after,
                                       PatchStrategy strategy) {
    RoundTripReport rep;

    if (!AllChecksumsOk(after)) {
        rep.error = "patched EDID has at least one bad block checksum";
        return rep;
    }
    if (!HasValidHeader(after)) {
        rep.error = "patched EDID lost its base block header";
        return rep;
    }
    const VsdbLocation loc = FindMicrosoftVsdb(after);
    if (!loc.found) {
        rep.error = "patched EDID does not contain a findable Microsoft VSDB";
        return rep;
    }
    if (loc.desktopUsage) {
        rep.error = "patched VSDB still has the desktop-usage bit set";
        return rep;
    }
    if (DeclaredExtensionCount(after) + 1u != BlockCount(after)) {
        rep.error = "extension count does not match the number of blocks present";
        return rep;
    }

    // Every data block and DTD that existed before must still exist after.
    const CtaBlockInfo ctaBefore = FindFirstCtaBlock(before);
    std::vector<Bytes> dbBefore, dbAfter, dtdBefore, dtdAfter;
    if (ctaBefore.found) {
        if (!ExtractCtaDataBlocks(before, ctaBefore, dbBefore)) {
            rep.error = "original CTA data block collection is malformed";
            return rep;
        }
        ExtractCtaDtds(before, ctaBefore, dtdBefore);
    }
    const CtaBlockInfo ctaAfter = FindFirstCtaBlock(after);
    if (ctaAfter.found) {
        if (!ExtractCtaDataBlocks(after, ctaAfter, dbAfter)) {
            rep.error = "patched CTA data block collection is malformed";
            return rep;
        }
        ExtractCtaDtds(after, ctaAfter, dtdAfter);
    }

    for (const Bytes& original : dbBefore) {
        bool present = false;
        for (const Bytes& candidate : dbAfter) {
            if (candidate == original) { present = true; break; }
        }
        if (!present) {
            rep.error = "a CTA data block from the original EDID was lost";
            return rep;
        }
        ++rep.preservedDataBlocks;
    }
    if (dtdBefore.size() != dtdAfter.size()) {
        rep.error = "the number of detailed timing descriptors changed";
        return rep;
    }
    for (size_t i = 0; i < dtdBefore.size(); ++i) {
        if (dtdBefore[i] != dtdAfter[i]) {
            rep.error = "a detailed timing descriptor changed";
            return rep;
        }
        ++rep.preservedDtds;
    }

    // Byte-level accounting of what moved.
    const size_t common = (before.size() < after.size()) ? before.size() : after.size();
    for (size_t i = 0; i < common; ++i) {
        if (before[i] != after[i]) ++rep.differingBytes;
    }

    if (strategy == PatchStrategy::AppendNewCtaBlock) {
        if (after.size() != before.size() + kBlockSize) {
            rep.error = "append strategy did not grow the EDID by exactly one block";
            return rep;
        }
        // Only byte 126 (extension count) and 127 (checksum) may differ.
        if (rep.differingBytes != 2 || before[126] + 1 != after[126]) {
            rep.error = "append strategy touched bytes outside the base block "
                        "extension count and checksum";
            return rep;
        }
    } else if (strategy == PatchStrategy::InsertIntoExistingCta) {
        if (after.size() != before.size()) {
            rep.error = "insert strategy changed the EDID length";
            return rep;
        }
        // Every changed byte must live inside the CTA block that was patched.
        for (size_t i = 0; i < common; ++i) {
            if (before[i] == after[i]) continue;
            if (i < ctaBefore.offset || i >= ctaBefore.offset + kBlockSize) {
                rep.error = "insert strategy modified a byte outside the CTA block";
                return rep;
            }
        }
    }

    rep.ok = true;
    return rep;
}

} // namespace edid
