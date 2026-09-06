// ConstraintChecker.cc — FQI grammar check and shared size-bound parser for
// the runtime constraint engine (architecture.md §4.2)
#include <sila/client/dynamic/ConstraintChecker.h>

#include <sila/client/dynamic/FdlIR.h>
#include <sila/common/util/AsciiCase.h>

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace sila2 {
namespace dynamic {

namespace {

bool lowerSegment(std::string_view value) {
    if (value.empty() || value.front() < 'a' || value.front() > 'z') return false;
    for (const char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

bool dottedLowerSegments(std::string_view value) {
    // FeatureDefinition.xsd caps Originator and Category at 255 characters.
    if (value.size() > 255) return false;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto dot = value.find('.', start);
        const auto end = dot == std::string_view::npos ? value.size() : dot;
        if (!lowerSegment(value.substr(start, end - start))) return false;
        if (dot == std::string_view::npos) return true;
        start = dot + 1;
    }
    return false;
}

bool identifierSegment(std::string_view value) {
    // DataTypes.xsd IdentifierType caps every identifier at 255 characters.
    if (value.size() > 255) return false;
    // strictFqiKind ASCII-folds the whole value before calling here (Part A
    // p87: FQIs compared without regard to case), so an Identifier segment
    // is always lower-case by the time it reaches this guard.
    if (value.empty() || value.front() < 'a' || value.front() > 'z') return false;
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string_view> splitFqi(std::string_view value) {
    // The longest valid FQI shape (CommandParameterIdentifier) has 8
    // components; stop splitting once that is exceeded instead of allocating
    // one entry per slash of arbitrarily long input.
    constexpr std::size_t maxComponents = 8;
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto slash = value.find('/', start);
        const auto end = slash == std::string_view::npos ? value.size() : slash;
        if (result.size() == maxComponents) return {};
        result.push_back(value.substr(start, end - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return result;
}

bool featurePrefix(std::string_view value, std::vector<std::string_view>& parts) {
    parts = splitFqi(value);
    if (parts.size() < 4 || !dottedLowerSegments(parts[0]) ||
        !dottedLowerSegments(parts[1]) || !identifierSegment(parts[2]) ||
        parts[3].size() < 2 || parts[3].front() != 'v') {
        return false;
    }
    for (const char character : parts[3].substr(1)) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

bool strictFqiKind(std::string_view value, ConstraintValue::FqiKind kind) {
    // Part A p87: a FQI is at most 2048 characters.
    if (value.size() > 2048) return false;
    // Part A p87: FQIs are compared without regard to letter case. Folding
    // once here (rather than widening every character class) keeps
    // lowerSegment/identifierSegment literally true of their names, and
    // matches the fold FqiMatch.h (SC23) and Constraints.cc's std::regex::icase
    // already apply to the same kind of value.
    const std::string lowered = util::asciiLower(value);

    std::vector<std::string_view> parts;
    // Suffix identifiers share the first four feature-FQI segments. Calling
    // featureFqi() here would reject every valid Command/Property/etc. FQI
    // before the kind-specific suffix is inspected.
    if (!featurePrefix(lowered, parts)) return false;
    if (kind == ConstraintValue::FqiKind::FeatureIdentifier) return parts.size() == 4;

    const auto hasShape = [&parts](std::initializer_list<std::string_view> suffix) {
        if (parts.size() != 4 + suffix.size()) return false;
        std::size_t index = 4;
        for (const auto expected : suffix) {
            if (expected.empty()) {
                if (!identifierSegment(parts[index])) return false;
            } else if (parts[index] != expected) {
                return false;
            }
            ++index;
        }
        return true;
    };
    // Suffix keywords are matched against `lowered`, so they are spelled
    // lower-case here; the canonical FDL spelling is "Command", "Parameter",
    // etc. (Part B p88).
    switch (kind) {
    case ConstraintValue::FqiKind::CommandIdentifier:
        return hasShape({"command", ""});
    case ConstraintValue::FqiKind::CommandParameterIdentifier:
        return hasShape({"command", "", "parameter", ""});
    case ConstraintValue::FqiKind::CommandResponseIdentifier:
        return hasShape({"command", "", "response", ""});
    case ConstraintValue::FqiKind::IntermediateCommandResponseIdentifier:
        return hasShape({"command", "", "intermediateresponse", ""});
    case ConstraintValue::FqiKind::DefinedExecutionErrorIdentifier:
        return hasShape({"definedexecutionerror", ""});
    case ConstraintValue::FqiKind::PropertyIdentifier:
        return hasShape({"property", ""});
    case ConstraintValue::FqiKind::TypeIdentifier:
        return hasShape({"datatype", ""});
    case ConstraintValue::FqiKind::MetadataIdentifier:
        return hasShape({"metadata", ""});
    case ConstraintValue::FqiKind::Unknown:
    case ConstraintValue::FqiKind::FeatureIdentifier:
        return false;
    }
    return false;
}

}  // namespace

std::optional<std::size_t> parseSizeConstraint(const ConstraintValue& constraint) {
    const std::string_view text = constraint.lexicalValue.empty()
                                      ? std::string_view{constraint.stringValue}
                                      : std::string_view{constraint.lexicalValue};
    if (!text.empty()) {
        // Every bound Constraints.xsd declares derives from xs:integer, whose
        // whiteSpace facet is the fixed "collapse" (XML Schema Part 2 4.3.6):
        // the schema accepts "<ElementCount> 0 </ElementCount>" and its value
        // is 0, but FdlRuntimeParser stores the element text verbatim. Trim the
        // padding the facet would have collapsed away before parsing.
        constexpr std::string_view kXmlSpace = " \t\n\r";
        std::string_view digits = text;
        digits.remove_prefix(std::min(digits.find_first_not_of(kXmlSpace), digits.size()));
        const auto lastKept = digits.find_last_not_of(kXmlSpace);
        if (lastKept == std::string_view::npos) return std::nullopt;
        digits.remove_suffix(digits.size() - lastKept - 1);

        // The integer lexical space also carries an optional sign (3.3.13), so
        // "+0", "+7" and "-0" are all valid text for these bounds.
        // std::from_chars' unsigned overload rejects every sign, so strip it
        // too: "+" is a no-op, and "-" is only in range when the magnitude
        // is zero.
        const bool negative = digits.front() == '-';
        if (negative || digits.front() == '+') digits.remove_prefix(1);
        if (digits.empty()) return std::nullopt;

        std::size_t result = 0;
        const auto* begin = digits.data();
        const auto* end = begin + digits.size();
        const auto parsed = std::from_chars(begin, end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
        if (negative && result != 0) return std::nullopt;
        return result;
    }
    if (constraint.numericValue < 0 ||
        constraint.numericValue > static_cast<double>(std::numeric_limits<std::size_t>::max()) ||
        constraint.numericValue !=
            static_cast<double>(static_cast<std::size_t>(constraint.numericValue))) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(constraint.numericValue);
}

std::optional<std::string> checkFqiConstraint(const ConstraintValue& constraint,
                                              std::string_view stringValue) {
    if (!strictFqiKind(stringValue, constraint.fqiKind)) {
        return "String is not a valid FullyQualifiedIdentifier of the requested kind";
    }
    return std::nullopt;
}

}  // namespace dynamic
}  // namespace sila2
