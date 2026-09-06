// Tests for the FDL constraint validators in Constraints.h: length, pattern,
// element count, set membership, inclusive/exclusive bounds, and Fully
// Qualified Identifier format — each checked for both its valid (nullopt) and
// its rejecting (error message) path.
#include <sila/common/types/Constraints.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
using namespace sila2::types;

TEST(Constraints, CheckLengthAcceptsExactMatch) {
    EXPECT_EQ(checkLength("abc", 3), std::nullopt);
}

TEST(Constraints, CheckLengthRejectsMismatch) {
    const auto result = checkLength("ab", 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String length is 2, expected exactly 3");
}

TEST(Constraints, CheckMinimalLengthAcceptsAtBoundary) {
    EXPECT_EQ(checkMinimalLength("abc", 3), std::nullopt);
}

TEST(Constraints, CheckMinimalLengthRejectsBelowMinimum) {
    const auto result = checkMinimalLength("ab", 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String length is 2, minimum is 3");
}

TEST(Constraints, CheckMaximalLengthAcceptsAtBoundary) {
    EXPECT_EQ(checkMaximalLength("abc", 3), std::nullopt);
}

TEST(Constraints, CheckMaximalLengthRejectsAboveMaximum) {
    const auto result = checkMaximalLength("abcd", 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String length is 4, maximum is 3");
}

// ---------------------------------------------------------------------------
// Length is counted in UTF-8 code points, not bytes (Constraints.xsd:14-24
// declares Length/MinimalLength/MaximalLength as applying to a String's
// character count).
// ---------------------------------------------------------------------------

TEST(Constraints, MaximalLengthCountsCharactersNotBytes) {
    // "한글이름" is 4 Korean characters, 12 UTF-8 bytes.
    EXPECT_EQ(checkMaximalLength("한글이름", 4), std::nullopt);
    EXPECT_EQ(checkLength("한글이름", 4), std::nullopt);
}

TEST(Constraints, MaximalLengthRejectsAboveMaximumInCharacters) {
    // 7 Korean characters exceeds a MaximalLength of 4 even though the
    // 21-byte encoding is well under a byte-based limit of 21.
    const auto result = checkMaximalLength("한글이름다섯여", 4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String length is 7, maximum is 4");
}

TEST(Constraints, CheckMinimalLengthRejectsBelowMinimumInCharacters) {
    // "한" is 1 character (3 bytes); a byte-based count would satisfy min=2.
    const auto result = checkMinimalLength("한", 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String length is 1, minimum is 2");
}

TEST(Constraints, CheckPatternAcceptsMatch) {
    EXPECT_EQ(checkPattern("hello", "h.*o"), std::nullopt);
}

TEST(Constraints, CheckPatternRejectsNonMatch) {
    const auto result = checkPattern("hello", "^xyz$");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String does not match pattern: ^xyz$");
}

// ---------------------------------------------------------------------------
// FDL <Pattern> is an XML Schema regular expression (Constraints.xsd:37-40),
// not ECMAScript. checkPattern translates '^'/'$' to their XSD (literal)
// reading and refuses the constructs it cannot express faithfully: \p{...},
// \P{...}, \i, \I, \c, \C, and character class subtraction.
// ---------------------------------------------------------------------------

TEST(Constraints, CheckPatternTreatsCaretAndDollarAsLiteralsPerXsd) {
    // Uncaught before: ECMAScript read this as anchors around "abc" and
    // accepted the value; XSD reads '^'/'$' as ordinary characters.
    EXPECT_EQ(checkPattern("^abc$", "^abc$"), std::nullopt);
}

TEST(Constraints, CheckPatternAcceptsTheUuidPatternUsedByFourteenInTreeFdls) {
    // SiLAService-v1_0.sila.xml:146 and 13 more in-tree FDLs depend on the
    // escaped hyphen still matching after translation.
    EXPECT_EQ(checkPattern("01234567-89ab-cdef-0123-456789abcdef",
                            R"([0-9a-f]{8}\-[0-9a-f]{4}\-[0-9a-f]{4}\-[0-9a-f]{4}\-[0-9a-f]{12})"),
              std::nullopt);
}

TEST(Constraints, CheckPatternKeepsCaretAsNegationInsideACharacterClass) {
    // Proves the '^' escape only applies outside a character class: inside
    // one, '^' stays negation in both XSD and ECMAScript.
    EXPECT_EQ(checkPattern("x", "[^abc]"), std::nullopt);
    ASSERT_TRUE(checkPattern("a", "[^abc]").has_value());
}

TEST(Constraints, CheckPatternAcceptsTheSemanticVersionPatternFromSiLAService) {
    // Verbatim from SiLAService-v1_0.sila.xml:177.
    EXPECT_EQ(checkPattern("1.2.3",
                            R"((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*))?(_[_a-zA-Z0-9]+)?)"),
              std::nullopt);
}

TEST(Constraints, CheckPatternIsWholeStringAnchoredPerXsd) {
    // XSD anchors implicitly; removing the ECMAScript anchors must not make
    // matching partial.
    ASSERT_TRUE(checkPattern("hello world", "hello").has_value());
}

TEST(Constraints, CheckPatternAcceptsADollarInsideACharacterClass) {
    // Pins the insideCharacterClass guard on the '$' branch.
    EXPECT_EQ(checkPattern("$", "[$]"), std::nullopt);
}

TEST(Constraints, CheckPatternRejectsAnchorSyntaxAgainstAnUnanchoredSubject) {
    // Headline behaviour change: "^abc$" is a five-character literal under
    // XSD, so a 3-character subject no longer matches it.
    const auto result = checkPattern("abc", "^abc$");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "String does not match pattern: ^abc$");
}

TEST(Constraints, CheckPatternReportsUnicodeCategoryEscapesAsUnsupported) {
    // Caught before, but with the wrong message ("Invalid pattern in FDL
    // constraint"), which blamed the FDL for a limitation of this runtime.
    const auto result = checkPattern("abc", R"(\p{L}+)");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "FDL pattern uses an XML Schema regex construct this runtime cannot "
              "evaluate (\\p): \\p{L}+");
}

TEST(Constraints, CheckPatternReportsUnicodeBlockEscapesAsUnsupported) {
    const auto result = checkPattern("abc", R"(\p{IsBasicLatin}+)");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "FDL pattern uses an XML Schema regex construct this runtime cannot "
              "evaluate (\\p): \\p{IsBasicLatin}+");
}

TEST(Constraints, CheckPatternReportsXmlNameCharacterEscapesAsUnsupported) {
    // Uncaught before: std::regex read \i\c* as literal "ic" and answered a
    // different question with no error at all.
    const auto result = checkPattern("abc", R"(\i\c*)");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "FDL pattern uses an XML Schema regex construct this runtime cannot "
              "evaluate (\\i): \\i\\c*");
}

TEST(Constraints, CheckPatternReportsCharacterClassSubtractionAsUnsupported) {
    // Uncaught before: ECMAScript mis-parsed the class and rejected every
    // single-character subject, including the 'b' this pattern is meant to
    // accept.
    const auto result = checkPattern("b", "[a-z-[aeiou]]");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "FDL pattern uses an XML Schema regex construct this runtime cannot "
              "evaluate (character class subtraction): [a-z-[aeiou]]");
}

TEST(Constraints, CheckPatternStillRejectsAMalformedPattern) {
    // Caught before and after: the translator must not swallow genuinely
    // broken input into the unsupported-construct branch.
    const auto result = checkPattern("abc", "[");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Invalid pattern in FDL constraint: [");
}

TEST(Constraints, CheckMinimalElementCountAcceptsAtBoundary) {
    EXPECT_EQ(checkMinimalElementCount(2, 2), std::nullopt);
}

TEST(Constraints, CheckMinimalElementCountRejectsBelowMinimum) {
    const auto result = checkMinimalElementCount(1, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Element count is 1, minimum is 2");
}

TEST(Constraints, CheckMaximalElementCountAcceptsAtBoundary) {
    EXPECT_EQ(checkMaximalElementCount(2, 2), std::nullopt);
}

TEST(Constraints, CheckMaximalElementCountRejectsAboveMaximum) {
    const auto result = checkMaximalElementCount(3, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Element count is 3, maximum is 2");
}

TEST(Constraints, CheckFullyQualifiedIdentifierAcceptsMixedCaseCategory) {
    // Part A p87: FQIs MUST be compared "without taking lower and upper case
    // into account" -- "Sensor" (upper-case start) folds to a valid Category.
    EXPECT_EQ(checkFullyQualifiedIdentifier("com.example/Sensor/TemperatureController/v2"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierAcceptsLowercaseCategory) {
    // Real SiLA FQIs use lowercase category names like "core".
    EXPECT_EQ(checkFullyQualifiedIdentifier("org.silastandard/core/SimulationController/v1"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierAcceptsDottedCategory) {
    // FeatureDefinition.xsd:116 -- Category is a dotted lower-case hierarchy;
    // real FDLs use it (e.g. tests/examples/fdl cetoni Category="pumps.contiflowpumps").
    EXPECT_EQ(checkFullyQualifiedIdentifier(
                  "de.cetoni/pumps.contiflowpumps/ContinuousFlowConfigurationService/v1"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierAcceptsLowerCaseFeatureIdentifier) {
    // Part B p88 describes an Identifier as starting upper-case, but Part A
    // p87 compares FQI VALUES without regard to case, so a lower-first
    // Feature Identifier segment ("temperatureController") is accepted too.
    EXPECT_EQ(checkFullyQualifiedIdentifier("org.example/sensor/temperatureController/v2"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierAcceptsCaseVariant) {
    // Part A p87: every segment upper-cased still validates.
    EXPECT_EQ(checkFullyQualifiedIdentifier("ORG.SILASTANDARD/CORE/SimulationController/V1"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierRejectsOverLength) {
    // Part A p87 -- a FQI MUST be at most 2048 characters. The name segment
    // itself is pattern-valid, so only the length gate can reject this.
    const std::string longName(2100, 'A');
    EXPECT_NE(checkFullyQualifiedIdentifier("org.example/sensor/" + longName + "/v1"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierRejectsMinorVersionSegment) {
    // Part A p87 folds case, not structure -- "v1.0" is still not the
    // required "v" + digits shape.
    EXPECT_NE(checkFullyQualifiedIdentifier("org.silastandard/core/SimulationController/v1.0"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierRejectsTrailingSlash) {
    EXPECT_NE(checkFullyQualifiedIdentifier("org.silastandard/core/SimulationController/v1/"),
              std::nullopt);
}

TEST(Constraints, CheckFullyQualifiedIdentifierRejectsMalformed) {
    const auto result = checkFullyQualifiedIdentifier("not-an-fqi");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              "Not a valid SiLA 2 Fully Qualified Identifier (expected format: "
              "org.example/Category/Name/vN)");
}

TEST(Constraints, CheckSetAcceptsMember) {
    EXPECT_EQ(checkSet(5, std::vector<int>{1, 3, 5}), std::nullopt);
}

TEST(Constraints, CheckSetRejectsNonMember) {
    const auto result = checkSet(7, std::vector<int>{1, 3, 5});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Value is not in the allowed set");
}

TEST(Constraints, CheckMinimalInclusiveAcceptsAtBoundary) {
    EXPECT_EQ(checkMinimalInclusive(10, 10), std::nullopt);
}

TEST(Constraints, CheckMinimalInclusiveRejectsBelowMinimum) {
    const auto result = checkMinimalInclusive(9, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Value is below the minimum (inclusive)");
}

TEST(Constraints, CheckMaximalInclusiveAcceptsAtBoundary) {
    EXPECT_EQ(checkMaximalInclusive(10, 10), std::nullopt);
}

TEST(Constraints, CheckMaximalInclusiveRejectsAboveMaximum) {
    const auto result = checkMaximalInclusive(11, 10);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Value exceeds the maximum (inclusive)");
}

TEST(Constraints, CheckMinimalExclusiveAcceptsAboveMinimum) {
    EXPECT_EQ(checkMinimalExclusive(6, 5), std::nullopt);
}

TEST(Constraints, CheckMinimalExclusiveRejectsAtBoundary) {
    const auto result = checkMinimalExclusive(5, 5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Value is at or below the minimum (exclusive)");
}

TEST(Constraints, CheckMaximalExclusiveAcceptsBelowMaximum) {
    EXPECT_EQ(checkMaximalExclusive(4, 5), std::nullopt);
}

TEST(Constraints, CheckMaximalExclusiveRejectsAtBoundary) {
    const auto result = checkMaximalExclusive(5, 5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Value is at or above the maximum (exclusive)");
}

// ---------------------------------------------------------------------------
// R10-9e: isValidUtf8 is the strict RFC 3629 well-formedness check used by
// ValueValidator::validateContentType (Part A p70/p63: character data in a
// SiLA Binary MUST be UTF-8). It is deliberately separate from the lenient
// characterCount above, which is for length constraints and must not be
// reused here.
// ---------------------------------------------------------------------------

TEST(Constraints, Utf8AcceptsEmpty) {
    EXPECT_TRUE(isValidUtf8(""));
}

TEST(Constraints, Utf8AcceptsAscii) {
    EXPECT_TRUE(isValidUtf8("{\"k\":\"v\"}"));
}

TEST(Constraints, Utf8AcceptsKoreanMultibyte) {
    // "안녕" -- two three-byte UTF-8 code points.
    EXPECT_TRUE(isValidUtf8(std::string{"\xEC\x95\x88\xEB\x85\x95", 6}));
}

TEST(Constraints, Utf8RejectsOverlong) {
    // 0xC0 0x80 is an overlong two-byte encoding of NUL (U+0000), which a
    // one-byte encoding already covers -- RFC 3629 requires the shortest form.
    EXPECT_FALSE(isValidUtf8(std::string{"\xC0\x80", 2}));
}

TEST(Constraints, Utf8RejectsSurrogate) {
    // 0xED 0xA0 0x80 encodes U+D800, a UTF-16 surrogate half that RFC 3629
    // excludes from well-formed UTF-8.
    EXPECT_FALSE(isValidUtf8(std::string{"\xED\xA0\x80", 3}));
}

TEST(Constraints, Utf8RejectsTruncated) {
    // 0xE2 0x82 starts a three-byte sequence but is missing its third byte.
    EXPECT_FALSE(isValidUtf8(std::string{"\xE2\x82", 2}));
}

TEST(Constraints, Utf8RejectsStrayContinuation) {
    // 0x80 is a continuation byte (10xxxxxx) with no preceding lead byte.
    EXPECT_FALSE(isValidUtf8(std::string{"\x80", 1}));
}

TEST(Constraints, Utf8RejectsAboveMax) {
    // 0xF4 0x90 0x80 0x80 encodes U+110000, one past the U+10FFFF Unicode
    // ceiling.
    EXPECT_FALSE(isValidUtf8(std::string{"\xF4\x90\x80\x80", 4}));
}

}  // namespace
