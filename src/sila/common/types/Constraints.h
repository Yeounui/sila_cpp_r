// Constraints.h — FDL constraint validation (architecture.md §3.4)
//
// Each check function returns std::nullopt when the value is valid, or a
// human-readable error message string when it fails.
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace sila2 {
namespace types {

/// Checks the FDL @ref gl_constraint "Constraint" `<Length>`: val has exactly
/// `exact` characters. Returns std::nullopt when it does, otherwise an
/// error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkLength(const std::string& val, size_t exact);

/// Checks the FDL @ref gl_constraint "Constraint" `<MinimalLength>`: val has
/// at least `min` characters. Returns std::nullopt when it does, otherwise an
/// error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMinimalLength(const std::string& val, size_t min);

/// Checks the FDL @ref gl_constraint "Constraint" `<MaximalLength>`: val has
/// at most `max` characters. Returns std::nullopt when it does, otherwise an
/// error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMaximalLength(const std::string& val, size_t max);

/// Strict UTF-8 well-formedness check (RFC 3629): every code point uses its
/// shortest form (no overlong encodings), no UTF-16 surrogates
/// (U+D800..U+DFFF), nothing above U+10FFFF, no truncated sequences and no
/// stray continuation bytes. The empty string is valid. Deliberately NOT the
/// lenient characterCount used for length constraints -- Part A p70/p63 make
/// UTF-8 a MUST for character data in a textual Binary.
[[nodiscard("caller must inspect the validation result")]]
bool isValidUtf8(std::string_view bytes);

/// Checks the FDL @ref gl_constraint "Constraint" `<Pattern>`: val matches
/// the given XML Schema regular expression (Constraints.xsd:37-40). Returns
/// std::nullopt on a match, otherwise an error message describing the
/// violation (or the untranslatable construct, see below). Constructs with
/// no faithful std::regex equivalent (\p{...}, \P{...}, \i, \I, \c, \C, and
/// character class subtraction) are reported as errors rather than evaluated
/// under ECMAScript rules.
// ponytail: \d, \w and \s are ASCII-ranged here, where XSD defines them over
// Unicode (\d is \p{Nd}, \w excludes \p{P}\p{Z}\p{C}). No in-tree FDL uses
// them, so this narrows rather than breaks; the upgrade path is a Unicode
// character-class table, not a bigger translator.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkPattern(const std::string& val, const std::string& regex);

/// Translates and compiles an FDL <Pattern> once, so a caller validating the
/// same pattern against many values (e.g. every element of a repeated field)
/// does not recompile the regex per value. Returns nullptr when the pattern
/// cannot be translated to std::regex's grammar or fails to compile; the
/// caller must then fall back to checkPattern(val, regex) above, which
/// reproduces the exact same diagnostic for either failure.
[[nodiscard("caller must inspect the compiled pattern")]]
std::shared_ptr<const std::regex> compilePattern(const std::string& regex);

/// Checks the FDL @ref gl_constraint "Constraint" `<Pattern>` against an
/// already-compiled pattern from compilePattern(). Returns std::nullopt on a
/// match, otherwise an error message. originalPattern is echoed in the error
/// message, exactly like the string-regex overload above (the FDL author
/// never wrote the translation).
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkPattern(const std::string& val, const std::regex& compiled,
                                        const std::string& originalPattern);

/// Checks the FDL @ref gl_constraint "Constraint" `<MinimalElementCount>`:
/// a repeated field has at least `min` elements. Returns std::nullopt when it
/// does, otherwise an error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMinimalElementCount(size_t count, size_t min);

/// Checks the FDL @ref gl_constraint "Constraint" `<MaximalElementCount>`:
/// a repeated field has at most `max` elements. Returns std::nullopt when it
/// does, otherwise an error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMaximalElementCount(size_t count, size_t max);

/// Checks that val is a well-formed @ref gl_fully_qualified_identifier "Fully Qualified Identifier"
/// . Returns std::nullopt when it is, otherwise
/// an error message describing the violation.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkFullyQualifiedIdentifier(const std::string& val);

/// Checks the FDL @ref gl_constraint "Constraint" `<Set>`: val is one of the
/// elements in `allowed`. Returns std::nullopt when it is, otherwise an
/// error message describing the violation.
template <typename T>
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkSet(const T& val, const std::vector<T>& allowed) {
    // std::any_of over std::find: no need to keep the matched iterator.
    bool isMember = std::any_of(allowed.begin(), allowed.end(), [&val](const T& candidate) {
        return candidate == val;
    });
    if (!isMember) {
        return "Value is not in the allowed set";
    }
    return std::nullopt;
}

/// Checks the FDL @ref gl_constraint "Constraint" `<MinimalInclusive>`: val
/// is greater than or equal to `min`. Returns std::nullopt when it is,
/// otherwise an error message describing the violation.
template <typename T>
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMinimalInclusive(const T& val, const T& min) {
    if (val < min) {
        return "Value is below the minimum (inclusive)";
    }
    return std::nullopt;
}

/// Checks the FDL @ref gl_constraint "Constraint" `<MaximalInclusive>`: val
/// is less than or equal to `max`. Returns std::nullopt when it is,
/// otherwise an error message describing the violation.
template <typename T>
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMaximalInclusive(const T& val, const T& max) {
    if (val > max) {
        return "Value exceeds the maximum (inclusive)";
    }
    return std::nullopt;
}

/// Checks the FDL @ref gl_constraint "Constraint" `<MinimalExclusive>`: val
/// is strictly greater than `min`. Returns std::nullopt when it is,
/// otherwise an error message describing the violation.
template <typename T>
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMinimalExclusive(const T& val, const T& min) {
    if (val <= min) {
        return "Value is at or below the minimum (exclusive)";
    }
    return std::nullopt;
}

/// Checks the FDL @ref gl_constraint "Constraint" `<MaximalExclusive>`: val
/// is strictly less than `max`. Returns std::nullopt when it is, otherwise
/// an error message describing the violation.
template <typename T>
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkMaximalExclusive(const T& val, const T& max) {
    if (val >= max) {
        return "Value is at or above the maximum (exclusive)";
    }
    return std::nullopt;
}
}  // namespace types
}  // namespace sila2
