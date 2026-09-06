// JsonSchemaSupport.h — Runtime JSON Schema validation (architecture-v2.md §Schema)
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sila2 {
namespace dynamic {

// Part A p70 Schema (Type Json, Source Inline): validate documentJson against
// the inline JSON Schema schemaJson. Both are parsed as JSON before the schema
// is compiled (draft-7 -- an implementation policy since Part A p70 names no
// draft) and no external/remote $ref is resolved (self-contained): a refusing
// schema_loader blocks every $ref that is not local to the root schema, so no
// network or filesystem IO ever happens on this path. Returns std::nullopt
// when documentJson is schema-valid, or a Validation Error diagnostic (never
// throws) so the server resolver can wrap it. Declared nlohmann-free: this
// header keeps the new vcpkg dependency confined to JsonSchemaSupport.cc, the
// only TU that includes <nlohmann/json.hpp> and <nlohmann/json-schema.hpp> --
// mirrors how FdlRuntimeParser.h/.cc confine libxml2.
std::optional<std::string> validateJsonAgainstInlineSchema(std::string_view schemaJson,
                                                            std::string_view documentJson);

}  // namespace dynamic
}  // namespace sila2
