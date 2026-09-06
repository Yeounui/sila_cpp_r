// BinaryParameterInterceptor.h — Binary field resolution around handler dispatch (architecture.md §3.5)
//
// New component, not a port. sila_cpp has no equivalent interceptor pattern.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "SiLAFramework.pb.h"
#include <sila/client/dynamic/AnyCodec.h>
#include <sila/common/types/BasicTypes.h>
#include <sila/server/binary/BinaryStore.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

namespace sila2 {
namespace binary {

// Binary Transfer inlines values up to this size; anything larger must go
// through CreateBinary/UploadChunk (SiLABinaryTransfer.proto) instead of
// being embedded directly in the request/response message.
static constexpr std::size_t kBinaryInlineThreshold = 2 * 1024 * 1024;

namespace detail {

// Field-level access to a SiLAFramework.Binary by reflection, not
// dynamic_cast: a Binary decoded out of an Any payload (AnyCodec::decode,
// Part B p66) is a google::protobuf::DynamicMessage, not the generated class,
// so dynamic_cast<Binary*> on it returns nullptr (crash). Reflection uses the
// same descriptor for generated and dynamic messages, so it handles both.
struct BinaryAccess {
    const google::protobuf::Reflection* reflection;
    const google::protobuf::FieldDescriptor* valueField;  // bytes value = 1
    const google::protobuf::FieldDescriptor* uuidField;   // string binaryTransferUUID = 2
    const google::protobuf::FieldDescriptor* setArm;      // the oneof arm currently set, or nullptr
};

inline BinaryAccess binaryAccess(google::protobuf::Message* binary) {
    const google::protobuf::Descriptor* descriptor = binary->GetDescriptor();
    const google::protobuf::Reflection* reflection = binary->GetReflection();
    const google::protobuf::OneofDescriptor* unionOneof = descriptor->FindOneofByName("union");
    BinaryAccess access;
    access.reflection = reflection;
    access.valueField = descriptor->FindFieldByName("value");
    access.uuidField = descriptor->FindFieldByName("binaryTransferUUID");
    access.setArm = (unionOneof != nullptr) ? reflection->GetOneofFieldDescriptor(*binary, unionOneof) : nullptr;
    return access;
}

// Mutual recursion: visitBinaryFields descends into an Any's decoded payload
// through visitAnyPayload, which re-enters visitBinaryFields on the decoded
// message.
template <typename BinaryFn>
bool visitAnyPayload(google::protobuf::Message* anyMessage, const BinaryFn& fn, std::size_t depth);

// Descend into every SiLAFramework.Binary reachable from message, calling
// fn(Binary*) on each. Recurses through plain sub-messages and, since an Any
// carries its value as opaque bytes (Part B p66), decodes Any payloads and
// recurses into them too (Part B p75-77 / Part A p67: the UUID-resolution
// obligation does not stop at an Any boundary). fn returns true when it
// mutated its Binary; the result is OR-ed up so an enclosing Any whose decoded
// payload changed knows to re-serialize. depth counts every hop (plain
// sub-message and Any descent alike) and stops at kMaxVisitDepth: a wire
// message parses at most 100 levels deep (protobuf's default recursion limit),
// so no valid request reaches the cap, while a recursive descriptor or an
// Any-in-Any chain cannot grow the C++ stack without bound. A deep Any chain
// left untouched here is rejected downstream by ValueValidator's own depth-64
// guard (rejectDisallowedAnyType).
constexpr std::size_t kMaxVisitDepth = 1024;

template <typename BinaryFn>
bool visitBinaryFields(google::protobuf::Message* message, const BinaryFn& fn, std::size_t depth) {
    if (depth > kMaxVisitDepth) {
        return false;
    }
    const google::protobuf::Descriptor* descriptor = message->GetDescriptor();
    const google::protobuf::Reflection* reflection = message->GetReflection();

    bool mutated = false;
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* field = descriptor->field(i);
        if (field->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
            continue;
        }
        // full_name() returns string_view in this vcpkg protobuf (not const
        // std::string&, as elsewhere in this codebase); bind by value here.
        const std::string_view fieldTypeName = field->message_type()->full_name();
        const bool isBinaryField = fieldTypeName == "sila2.org.silastandard.Binary";
        const bool isAnyField = fieldTypeName == "sila2.org.silastandard.Any";

        if (field->is_repeated()) {
            const int fieldSize = reflection->FieldSize(*message, field);
            for (int j = 0; j < fieldSize; ++j) {
                google::protobuf::Message* subMessage = reflection->MutableRepeatedMessage(message, field, j);
                if (isBinaryField) {
                    mutated |= fn(subMessage);
                } else if (isAnyField) {
                    mutated |= visitAnyPayload(subMessage, fn, depth);
                } else {
                    mutated |= visitBinaryFields(subMessage, fn, depth + 1);
                }
            }
        } else {
            if (!reflection->HasField(*message, field)) {
                continue;
            }
            google::protobuf::Message* subMessage = reflection->MutableMessage(message, field);
            if (isBinaryField) {
                mutated |= fn(subMessage);
            } else if (isAnyField) {
                mutated |= visitAnyPayload(subMessage, fn, depth);
            } else {
                mutated |= visitBinaryFields(subMessage, fn, depth + 1);
            }
        }
    }
    return mutated;
}

template <typename BinaryFn>
bool visitAnyPayload(google::protobuf::Message* anyMessage, const BinaryFn& fn, std::size_t depth) {
    std::string typeXml;
    std::string payloadBytes;
    // Not a well-formed SiLAFramework.Any message: leave it untouched.
    if (!sila2::dynamic::AnyCodec::readAnyFields(*anyMessage, typeXml, payloadBytes)) {
        return false;
    }

    sila2::types::AnyValue anyValue;
    anyValue.typeXml = typeXml;
    anyValue.payload.assign(payloadBytes.begin(), payloadBytes.end());

    // Fresh pool per Any (mirrors ValueValidator.cc's decodeWireAnyPayload):
    // DescriptorBuilder names the synthetic file by a hash of the type XML and
    // BuildFile refuses a second file of the same name. The pool underlays the
    // generated pool so ".sila2.org.silastandard.Binary" resolves.
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    std::unique_ptr<google::protobuf::Message> decoded;
    try {
        decoded = sila2::dynamic::AnyCodec::decode(anyValue, pool, &factory);
    } catch (const std::exception&) {
        // Malformed type XML or a payload that does not match it: leave the Any
        // untouched so the adapter's validator (ValueValidator::validateAnyValue)
        // produces the Validation Error -- the interceptor must not raise a
        // different error class here.
        return false;
    }

    // A FrameworkError from an unknown/incomplete UUID inside the payload
    // propagates out (the recursion is outside the try above), so an
    // Any{Binary} with a bad UUID fails exactly like a top-level Binary.
    const bool changed = visitBinaryFields(decoded.get(), fn, depth + 1);
    if (changed) {
        const google::protobuf::FieldDescriptor* payloadField =
            anyMessage->GetDescriptor()->FindFieldByName("payload");
        anyMessage->GetReflection()->SetString(anyMessage, payloadField, decoded->SerializeAsString());
    }
    return changed;
}

}  // namespace detail

/// Replace every Binary field carrying a binaryTransferUUID with its
/// assembled bytes, so handler code only ever sees resolved values.
/// Recurses into nested message fields and Any payloads at any depth.
/// @throws error::FrameworkError{CommandExecutionNotAccepted} if a referenced UUID is unknown or its
/// upload is still incomplete.
inline void resolveBinaryParameters(BinaryStore& store, google::protobuf::Message* message) {
    detail::visitBinaryFields(message, [&store](google::protobuf::Message* binaryMessage) -> bool {
        const detail::BinaryAccess access = detail::binaryAccess(binaryMessage);
        if (access.setArm != access.uuidField) {
            return false;  // no binaryTransferUUID arm set; nothing to resolve
        }
        const std::string uuid = access.reflection->GetString(*binaryMessage, access.uuidField);
        std::vector<uint8_t> assembled;
        // CommandExecutionNotAccepted, not ValidationError: ValidationError's
        // first field is the fully qualified PARAMETER identifier
        // (SiLAFramework.proto:95-98) and dispatchToHandler is handed only
        // the Feature FQI, never the Command/Parameter segments.
        // SiLABinaryTransfer's INVALID_BINARY_TRANSFER_UUID is not a
        // SiLAError oneof arm (SiLABinaryTransfer.proto:73-81), so of the
        // five values this is the only one that says "the server will not
        // run this call".
        try {
            assembled = store.assemble(uuid);
        } catch (const std::out_of_range&) {
            throw error::FrameworkError{error::FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted,
                                         "Binary transfer UUID not found: " + uuid};
        } catch (const std::logic_error&) {
            // assemble() throws logic_error only for an incomplete slot
            // (InMemoryBinaryStore.cc:92, FileSpoolBinaryStore.cc:131).
            // Reporting that as "not found" sends the client hunting for a
            // lifetime expiry that never happened.
            throw error::FrameworkError{error::FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted,
                                         "Binary transfer incomplete: " + uuid};
        }
        access.reflection->SetString(binaryMessage, access.valueField,
                                     std::string{assembled.begin(), assembled.end()});
        return true;
    }, 0);
}

/// Replace every inline Binary value larger than 2 MiB with a
/// binaryTransferUUID, storing the payload in store for later retrieval via
/// GetBinary/DownloadChunk. Recurses into nested message fields and Any
/// payloads at any depth.
inline void injectBinaryResults(BinaryStore& store, google::protobuf::Message* message,
                                 std::chrono::seconds lifetime) {
    detail::visitBinaryFields(message, [&store, lifetime](google::protobuf::Message* binaryMessage) -> bool {
        const detail::BinaryAccess access = detail::binaryAccess(binaryMessage);
        if (access.setArm != access.valueField) {
            return false;  // no inline value set; nothing to inject
        }
        // Copy the value out before SetString on the uuid arm clears it (oneof).
        const std::string value = access.reflection->GetString(*binaryMessage, access.valueField);
        if (value.size() <= kBinaryInlineThreshold) {
            return false;
        }
        const std::string uuid = store.createSlot(value.size(), 1, lifetime);
        store.storeChunk(uuid, 0, {value.begin(), value.end()});
        access.reflection->SetString(binaryMessage, access.uuidField, uuid);
        return true;
    }, 0);
}

}  // namespace binary
}  // namespace sila2
