// AsciiCase.h -- case-insensitive helpers for the SiLA identifiers the standard
// compares without regard to case: Fully Qualified Identifiers (Part A p87) and
// UUIDs (Part A p90). Both are ASCII by construction (the FQI grammar and the
// RFC 4122 hex string), so ASCII tolower is exact; no locale or UTF-8 folding.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace sila2::util {

/// Returns `value` with every ASCII A-Z lowered, other bytes unchanged. Used to
/// normalise one operand of an FQI/UUID compare that is not a map key.
inline std::string asciiLower(std::string_view value) {
    std::string lowered{value};
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

/// Transparent std::map comparator ordering keys by their ASCII-lowered form, so
/// a map keyed by FQI or UUID matches a lookup whatever case the peer sent while
/// its stored keys keep the canonical case (what ListImplementedFeatures
/// advertises). Compares char-by-char rather than lowering both sides, so a
/// lookup allocates nothing. is_transparent enables string_view lookups.
struct CaseInsensitiveLess {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const {
        const std::size_t common = std::min(lhs.size(), rhs.size());
        for (std::size_t i = 0; i < common; ++i) {
            const unsigned char a =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(lhs[i])));
            const unsigned char b =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(rhs[i])));
            if (a != b) { return a < b; }
        }
        return lhs.size() < rhs.size();
    }
};

}  // namespace sila2::util
