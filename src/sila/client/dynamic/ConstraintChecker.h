// ConstraintChecker.h — FQI grammar check and shared size-bound parser for the
// runtime constraint engine (architecture.md §4.2)
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace sila2 {
namespace dynamic {

struct ConstraintValue;

// Constraints.xsd defines Length/MinLength/MaxLength/ElementCount/Min/Max
// ElementCount bounds as unbounded XSD integer text: xs:nonNegativeInteger for
// Length, MinimalLength and the ElementCount trio, xs:positiveInteger for
// MaximalLength, both arbitrary precision. numericValue is a lossy legacy
// double view (0 for text outside double's range), so the
// lexical text (lexicalValue, falling back to stringValue) is authoritative
// when present; a constraint built with only numericValue set (e.g. existing
// hand-built test helpers) falls back to that. Returns std::nullopt if the
// text fails to parse as a size_t.
[[nodiscard("caller must inspect the parse result")]]
std::optional<std::size_t> parseSizeConstraint(const ConstraintValue& constraint);

// Checks that stringValue is a well-formed FullyQualifiedIdentifier of the
// kind the constraint requests (Constraints.xsd enumerates nine kinds).
// Returns std::nullopt if valid, or the validation error message. A non-FQI
// constraint carries FqiKind::Unknown and is rejected. The comparison is
// case-insensitive (Part A p87): only the grammar shape and the 2048-char
// cap are enforced, letter case is not.
[[nodiscard("caller must inspect the validation result")]]
std::optional<std::string> checkFqiConstraint(
    const ConstraintValue& constraint, std::string_view stringValue);

}  // namespace dynamic
}  // namespace sila2
