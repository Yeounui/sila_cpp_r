// Constraints.cc — FDL constraint validation implementation
#include "Constraints.h"

#include <algorithm>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>

namespace sila2 {
namespace types {

namespace {
// FDL <Length>/<MinimalLength>/<MaximalLength> on a String count characters
// (Constraints.xsd:14-24), while std::string::size() counts UTF-8 bytes — a
// multi-byte name would be rejected at roughly a third of its declared limit.
// Every byte that is not a continuation byte (0b10xxxxxx) starts one code point.
// Malformed UTF-8 is counted leniently rather than rejected: these helpers
// validate length, not encoding, and every value arrives from a proto3 string
// field, which gRPC has already validated as UTF-8.
std::size_t characterCount(const std::string& val) {
    return static_cast<std::size_t>(std::count_if(val.begin(), val.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

// FDL <Pattern> is an XML Schema regular expression (Constraints.xsd:37-40),
// which is NOT the ECMAScript grammar std::regex defaults to. Most of the
// grammar is common ground, but four differences change the answer:
//
//   1. XSD has no anchors. '^' and '$' are ordinary characters there, while
//      ECMAScript reads them as position assertions. std::regex_match already
//      supplies XSD's implicit whole-string anchoring, so escaping them
//      restores the XSD reading without losing it.
//   2. Character class subtraction, "[a-z-[aeiou]]", is XSD-only. ECMAScript
//      parses it as a class containing '-' and '[' and then a stray ']', so
//      the pattern silently stops meaning what the FDL author wrote.
//   3. \p{...} and \P{...} select Unicode categories and blocks. libstdc++
//      throws regex_error on them, which today is reported as if the FDL were
//      malformed.
//   4. \i \I \c \C are XSD's XML name-character classes. std::regex accepts
//      them as identity escapes for the letters i and c, so a pattern meant to
//      match an XML name silently matches literal "ic" instead.
//
// (1) is translated. (2)(3)(4) cannot be expressed faithfully without a
// Unicode-aware engine, so they are refused by name rather than answered
// wrongly — a validator that quietly changes the question is worse than one
// that admits it cannot answer.
struct PatternTranslation {
    std::string ecmaScript;
    std::string unsupportedConstruct;  // non-empty: cannot be evaluated faithfully
};

PatternTranslation translateXsdPattern(const std::string& xsdPattern) {
    PatternTranslation translated;
    bool insideCharacterClass = false;
    for (std::size_t i = 0; i < xsdPattern.size(); ++i) {
        const char current = xsdPattern[i];
        if (current == '\\' && i + 1 < xsdPattern.size()) {
            const char escaped = xsdPattern[i + 1];
            if (escaped == 'p' || escaped == 'P' || escaped == 'i' || escaped == 'I' ||
                escaped == 'c' || escaped == 'C') {
                translated.unsupportedConstruct = std::string{"\\"} + escaped;
                return translated;
            }
            // Every other XSD escape (\-, \., \d, \w, \s, \n, ...) has the same
            // spelling in ECMAScript, so it passes through with its backslash.
            translated.ecmaScript += current;
            translated.ecmaScript += escaped;
            ++i;
            continue;
        }
        if (insideCharacterClass && current == '-' && i + 1 < xsdPattern.size() &&
            xsdPattern[i + 1] == '[') {
            translated.unsupportedConstruct = "character class subtraction";
            return translated;
        }
        if (current == '[') {
            insideCharacterClass = true;
        } else if (current == ']') {
            insideCharacterClass = false;
        } else if (!insideCharacterClass && (current == '^' || current == '$')) {
            // Outside a class these are literals in XSD. Inside one, a leading
            // '^' is negation in both grammars and must stay unescaped.
            translated.ecmaScript += '\\';
        }
        translated.ecmaScript += current;
    }
    return translated;
}

struct CompileResult {
    std::shared_ptr<const std::regex> compiled;
    std::string unsupportedConstruct;  // set only when compiled is null and translation failed
};

// Shared by checkPattern(val, regex) and compilePattern() so translateXsdPattern
// runs once per call instead of once for a compile attempt plus once more to
// recover the error text on failure.
CompileResult compileOrTranslateError(const std::string& xsdPattern) {
    const PatternTranslation translated = translateXsdPattern(xsdPattern);
    if (!translated.unsupportedConstruct.empty()) {
        return {nullptr, translated.unsupportedConstruct};
    }
    try {
        return {std::make_shared<const std::regex>(translated.ecmaScript), {}};
    } catch (const std::regex_error&) {
        return {nullptr, {}};
    }
}
}  // namespace

bool isValidUtf8(std::string_view bytes) {
    std::size_t i = 0;
    const std::size_t n = bytes.size();
    while (i < n) {
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        std::size_t extra = 0;
        char32_t codePoint = 0;
        char32_t minimum = 0;  // smallest code point this length may encode (overlong guard)
        if (lead < 0x80) {                   // U+0000..U+007F, one byte
            i += 1;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {  // 110xxxxx, two bytes
            extra = 1; codePoint = lead & 0x1F; minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {  // 1110xxxx, three bytes
            extra = 2; codePoint = lead & 0x0F; minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {  // 11110xxx, four bytes
            extra = 3; codePoint = lead & 0x07; minimum = 0x10000;
        } else {
            return false;  // stray continuation byte (10xxxxxx) or 0xF8..0xFF
        }
        if (i + extra >= n) return false;    // truncated multi-byte sequence
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(bytes[i + k]);
            if ((continuation & 0xC0) != 0x80) return false;  // not a 10xxxxxx byte
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        if (codePoint < minimum) return false;                      // overlong encoding
        if (codePoint > 0x10FFFF) return false;                     // beyond Unicode
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF) return false;  // UTF-16 surrogate
        i += extra + 1;
    }
    return true;
}

std::optional<std::string> checkLength(const std::string& val, size_t exact) {
    const std::size_t length = characterCount(val);
    if (length == exact) {
        return std::nullopt;
    }
    return "String length is " + std::to_string(length) + ", expected exactly " +
           std::to_string(exact);
}

std::optional<std::string> checkMinimalLength(const std::string& val, size_t min) {
    const std::size_t length = characterCount(val);
    if (length >= min) {
        return std::nullopt;
    }
    return "String length is " + std::to_string(length) + ", minimum is " +
           std::to_string(min);
}

std::optional<std::string> checkMaximalLength(const std::string& val, size_t max) {
    const std::size_t length = characterCount(val);
    if (length <= max) {
        return std::nullopt;
    }
    return "String length is " + std::to_string(length) + ", maximum is " +
           std::to_string(max);
}

std::optional<std::string> checkPattern(const std::string& val, const std::string& regex) {
    const CompileResult result = compileOrTranslateError(regex);
    if (result.compiled == nullptr) {
        if (!result.unsupportedConstruct.empty()) {
            return "FDL pattern uses an XML Schema regex construct this runtime cannot "
                   "evaluate (" + result.unsupportedConstruct + "): " + regex;
        }
        return "Invalid pattern in FDL constraint: " + regex;
    }
    return checkPattern(val, *result.compiled, regex);
}

std::shared_ptr<const std::regex> compilePattern(const std::string& regex) {
    return compileOrTranslateError(regex).compiled;
}

std::optional<std::string> checkPattern(const std::string& val, const std::regex& compiled,
                                        const std::string& originalPattern) {
    // regex_match requires the whole subject to match, which is exactly XSD's
    // implicit anchoring.
    if (std::regex_match(val, compiled)) {
        return std::nullopt;
    }
    // Every message echoes the ORIGINAL pattern, never the translated one: the
    // FDL author never wrote the translation.
    return "String does not match pattern: " + originalPattern;
}

std::optional<std::string> checkMinimalElementCount(size_t count, size_t min) {
    if (count >= min) {
        return std::nullopt;
    }
    return "Element count is " + std::to_string(count) + ", minimum is " + std::to_string(min);
}

std::optional<std::string> checkMaximalElementCount(size_t count, size_t max) {
    if (count <= max) {
        return std::nullopt;
    }
    return "Element count is " + std::to_string(count) + ", maximum is " + std::to_string(max);
}

std::optional<std::string> checkFullyQualifiedIdentifier(const std::string& val) {
    // Part A p87: a FQI "MUST be a string of UNICODE characters up to a maximum
    // of 2048 characters in length." Valid FQIs are ASCII (the pattern below),
    // so byte length equals character length here.
    if (val.size() > 2048) {
        return "Fully Qualified Identifier exceeds the maximum length of 2048 characters";
    }
    // Pattern is a fixed literal shared by every call, so a function-local
    // static avoids recompiling it on each invocation.
    // Originator and Category are lower-case dotted segments
    // (FeatureDefinition.xsd:108 Originator, :116 Category); the Feature
    // Identifier is an Identifier, which "MUST ... start with an upper-case
    // letter (A-Z)" (Part B p88) -- that DEFINITION grammar stays as written
    // below, but Part A p87 requires FQI VALUES to be compared "without
    // taking lower and upper case into account", hence std::regex::icase.
    static const std::regex kFqiPattern{
        R"(^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*/[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)*/[A-Z][A-Za-z0-9]*/v[0-9]+$)",
        std::regex::icase};
    if (std::regex_match(val, kFqiPattern)) {
        return std::nullopt;
    }
    return "Not a valid SiLA 2 Fully Qualified Identifier (expected format: "
           "org.example/Category/Name/vN)";
}

}  // namespace types
}  // namespace sila2
