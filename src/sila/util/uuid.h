// uuid.h — RFC 4122 v4 UUID generation (config·command·binary·recovery 공용)
#pragma once

#include <string>

namespace sila2::util {

/// Generate a random UUID v4 string in 8-4-4-4-12 lowercase hex format.
[[nodiscard("caller expects the generated UUID")]]
std::string generateUuid();

}  // namespace sila2::util
