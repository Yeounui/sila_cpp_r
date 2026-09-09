// BinaryChunkLimit.h — shared Binary Chunk size ceiling (Part B p56)
#pragma once

#include <cstddef>

namespace sila2 {
namespace binary {

// Part B p56, Definition: Binary Chunk: "A Binary Chunk MUST not be larger
// than 2 MiB in size." Both server entry points (BinaryUploadService,
// BinaryDownloadService, CloudEnvelopeRouter) and the client uploader
// (BinaryUploader.cc) share this one constant.
//
// This is distinct in *meaning* from two other 2 MiB constants that happen to
// carry the same value: kBinaryInlineThreshold
// (src/sila/server/binary/BinaryParameterInterceptor.h) decides whether a
// whole parameter value is sent inline vs. as a Binary Transfer, and
// binarySpoolThreshold (src/sila/server/config/ServerConfig.h) decides
// whether a binary is held in RAM vs. spooled to disk. Neither of those is
// this per-chunk wire ceiling, and per the binding constraint neither is
// retargeted to reuse this constant.
/// The largest single chunk a @ref gl_binary_transfer "Binary Transfer" may
/// carry on the wire, in bytes. Callers that split or reassemble a large
/// binary must stay within this size per chunk.
static constexpr std::size_t kMaxBinaryChunkSize = 2 * 1024 * 1024;

}  // namespace binary
}  // namespace sila2
