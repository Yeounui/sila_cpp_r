// Tests for resolveBinaryParameters/injectBinaryResults: Binary field
// resolution and injection around handler dispatch, including recursion into
// nested messages and the 2 MiB inline threshold.
//
// No generated proto message in this repo carries a Binary field, so the test
// messages (TestWrapper/TestNested) are built at runtime via
// google::protobuf::DescriptorPool + DynamicMessageFactory, with the pool's
// underlay set to the generated pool so ".sila2.org.silastandard.Binary"
// resolves to the real generated type. SetDelegateToGeneratedFactory(true)
// makes the factory hand back genuine sila2::org::silastandard::Binary
// instances for that field (not a DynamicMessage), which the interceptor's
// dynamic_cast requires.
#include <sila/server/binary/BinaryParameterInterceptor.h>

#include <gtest/gtest.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <sila/client/dynamic/AnyCodec.h>
#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/types/BasicTypes.h>
#include <sila/server/binary/InMemoryBinaryStore.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

namespace
{
using sila2::InMemoryBinaryStore;
using sila2::binary::injectBinaryResults;
using sila2::binary::kBinaryInlineThreshold;
using sila2::binary::resolveBinaryParameters;
using sila2::dynamic::AnyCodec;
using sila2::dynamic::DescriptorBuilder;
using sila2::error::FrameworkError;
using sila2::org::silastandard::Any;
using sila2::org::silastandard::Binary;
using sila2::types::AnyValue;
using namespace std::chrono_literals;

// Builds TestWrapper { Binary payload = 1; repeated Binary payloads = 2;
// TestNested nested = 3; Any anyValue = 4; } and TestNested { Binary inner = 1; }
// in a pool underlaid by the generated pool, so field types
// ".sila2.org.silastandard.Binary"/".sila2.org.silastandard.Any" resolve
// against the real SiLAFramework.proto descriptors.
class DynamicWrapperMessages {
public:
    // delegateToGenerated=false makes the factory hand back plain
    // DynamicMessage instances for every field (including "payload"), the
    // shape a Binary decoded out of an Any via AnyCodec::decode always has --
    // used by ResolveDynamicMessageBinaryWithoutCrash to prove the
    // interceptor no longer requires dynamic_cast<Binary*> to succeed.
    explicit DynamicWrapperMessages(bool delegateToGenerated = true)
        : pool_{google::protobuf::DescriptorPool::generated_pool()}, factory_{&pool_} {
        factory_.SetDelegateToGeneratedFactory(delegateToGenerated);

        google::protobuf::FileDescriptorProto fileProto;
        fileProto.set_name("test_binary_parameter_interceptor_wrapper.proto");
        fileProto.set_package("sila2.test.binary");
        fileProto.set_syntax("proto3");
        fileProto.add_dependency("SiLAFramework.proto");

        google::protobuf::DescriptorProto* wrapper = fileProto.add_message_type();
        wrapper->set_name("TestWrapper");
        addBinaryField(wrapper, "payload", 1, google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        addBinaryField(wrapper, "payloads", 2, google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
        auto* nestedField = wrapper->add_field();
        nestedField->set_name("nested");
        nestedField->set_number(3);
        nestedField->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        nestedField->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        nestedField->set_type_name(".sila2.test.binary.TestNested");

        // Field 4, added unset by every existing test: HasField is false for
        // it, so resolveBinaryParameters/injectBinaryResults skip it and the
        // pre-existing tests stay untouched.
        auto* anyField = wrapper->add_field();
        anyField->set_name("anyValue");
        anyField->set_number(4);
        anyField->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        anyField->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        anyField->set_type_name(".sila2.org.silastandard.Any");

        google::protobuf::DescriptorProto* nested = fileProto.add_message_type();
        nested->set_name("TestNested");
        addBinaryField(nested, "inner", 1, google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        // Self-referential field 2, unset by every existing test, lets
        // ResolveDeepPlainNestingIsNotDepthCapped build a chain deeper than
        // 64 levels.
        auto* childField = nested->add_field();
        childField->set_name("child");
        childField->set_number(2);
        childField->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        childField->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        childField->set_type_name(".sila2.test.binary.TestNested");

        const google::protobuf::FileDescriptor* file = pool_.BuildFile(fileProto);
        wrapperDescriptor_ = file->FindMessageTypeByName("TestWrapper");
    }

    [[nodiscard("caller expects the owned wrapper instance")]]
    std::unique_ptr<google::protobuf::Message> newWrapper() {
        return std::unique_ptr<google::protobuf::Message>{factory_.GetPrototype(wrapperDescriptor_)->New()};
    }

private:
    static void addBinaryField(google::protobuf::DescriptorProto* message, const std::string& name, int number,
                                google::protobuf::FieldDescriptorProto::Label label) {
        auto* field = message->add_field();
        field->set_name(name);
        field->set_number(number);
        field->set_label(label);
        field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        field->set_type_name(".sila2.org.silastandard.Binary");
    }

    google::protobuf::DescriptorPool pool_;
    google::protobuf::DynamicMessageFactory factory_;
    const google::protobuf::Descriptor* wrapperDescriptor_ = nullptr;
};

// Returns the generated Binary instance backing a singular message field,
// relying on SetDelegateToGeneratedFactory(true) to make that cast valid.
Binary* mutableBinaryField(google::protobuf::Message* message, const std::string& fieldName) {
    const google::protobuf::Reflection* reflection = message->GetReflection();
    const google::protobuf::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName(fieldName);
    return dynamic_cast<Binary*>(reflection->MutableMessage(message, field));
}

Binary* addRepeatedBinaryField(google::protobuf::Message* message, const std::string& fieldName) {
    const google::protobuf::Reflection* reflection = message->GetReflection();
    const google::protobuf::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName(fieldName);
    return dynamic_cast<Binary*>(reflection->AddMessage(message, field));
}

Binary* mutableNestedField(google::protobuf::Message* message) {
    const google::protobuf::Reflection* reflection = message->GetReflection();
    const google::protobuf::FieldDescriptor* nestedField = message->GetDescriptor()->FindFieldByName("nested");
    google::protobuf::Message* nested = reflection->MutableMessage(message, nestedField);
    return mutableBinaryField(nested, "inner");
}

// Returns the generated Any instance backing a singular message field,
// relying on SetDelegateToGeneratedFactory(true) to make that cast valid
// (mirrors mutableBinaryField above; used only with the default-delegating
// DynamicWrapperMessages()).
Any* mutableAnyField(google::protobuf::Message* message, const std::string& fieldName) {
    const google::protobuf::Reflection* reflection = message->GetReflection();
    const google::protobuf::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName(fieldName);
    return dynamic_cast<Any*>(reflection->MutableMessage(message, field));
}

// Sets a Binary's binaryTransferUUID by reflection instead of dynamic_cast, so
// it also works on a DynamicMessage Binary -- the shape a Binary decoded out
// of an Any payload (AnyCodec::decode) always has, and the shape every field
// of DynamicWrapperMessages(false) has.
void setBinaryUuidByReflection(google::protobuf::Message* binary, const std::string& uuid) {
    const google::protobuf::Reflection* reflection = binary->GetReflection();
    const google::protobuf::FieldDescriptor* uuidField = binary->GetDescriptor()->FindFieldByName("binaryTransferUUID");
    reflection->SetString(binary, uuidField, uuid);
}

std::string binaryValueByReflection(const google::protobuf::Message& binary) {
    const google::protobuf::Reflection* reflection = binary.GetReflection();
    const google::protobuf::FieldDescriptor* valueField = binary.GetDescriptor()->FindFieldByName("value");
    return reflection->GetString(binary, valueField);
}

// Builds the "DataType_Payload" message prototype that buildFromTypeXml()
// derives from `typeXml`, registered into `pool` (which must outlive the
// returned prototype and `factory`). Mirrors anyPayloadPrototype() from
// tests/sila/server/test_command_parameter_validator.cc:180-190 -- a separate
// TU (internal linkage via this anonymous namespace), same encode-side
// pipeline; per repo convention each test file keeps its own copy rather than
// sharing a production-only utility.
const google::protobuf::Message* anyPayloadPrototype(
    const std::string& typeXml, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    DescriptorBuilder builder;
    auto fileProto = builder.buildFromTypeXml(typeXml);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) return nullptr;
    const auto* desc = pool.FindMessageTypeByName(fileProto.package() + ".DataType_Payload");
    if (desc == nullptr) return nullptr;
    return factory.GetPrototype(desc);
}

// Encodes an Any payload for `typeXml` through the same
// DescriptorBuilder/DynamicMessageFactory pipeline anyPayloadPrototype uses:
// builds the DataType_Payload wrapper, hands it plus its Payload field to
// `fill` so the caller sets whatever scalar/nested value the shape needs, then
// serializes -- the bytes the interceptor's visitAnyPayload decodes. Each call
// gets a fresh pool/factory (DescriptorBuilder names the file by a hash of
// typeXml, and a pool refuses a second BuildFile of the same name). Mirrors
// test_command_parameter_validator.cc:200-217.
std::string encodeAnyPayload(
    const std::string& typeXml,
    const std::function<void(google::protobuf::Message&, const google::protobuf::FieldDescriptor&)>&
        fill) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const auto* prototype = anyPayloadPrototype(typeXml, encodePool, encodeFactory);
    if (prototype == nullptr) {
        throw std::runtime_error{"unable to build Any payload prototype for type XML: " + typeXml};
    }
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    if (payloadField == nullptr) {
        throw std::runtime_error{"Any payload prototype is missing its Payload field"};
    }
    fill(*wrapper, *payloadField);
    return wrapper->SerializeAsString();
}

// Decodes an Any(typeXml, payloadBytes) through the production AnyCodec, into
// a pool/factory the caller owns (both must outlive the returned message) --
// the same decode detail::visitAnyPayload performs in
// BinaryParameterInterceptor.h.
std::unique_ptr<google::protobuf::Message> decodeAnyPayload(
    const std::string& typeXml, const std::string& payloadBytes, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    AnyValue anyValue;
    anyValue.typeXml = typeXml;
    anyValue.payload.assign(payloadBytes.begin(), payloadBytes.end());
    return AnyCodec::decode(anyValue, pool, &factory);
}

// ---------------------------------------------------------------------------
// resolveBinaryParameters — True (positive) paths
// ---------------------------------------------------------------------------

TEST(BinaryParameterInterceptor, ResolveSingularFieldReplacesUuidWithAssembledValue) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{1, 2, 3, 4, 5};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);
    mutableBinaryField(wrapper.get(), "payload")->set_binarytransferuuid(uuid);

    resolveBinaryParameters(store, wrapper.get());

    Binary* payload = mutableBinaryField(wrapper.get(), "payload");
    ASSERT_EQ(payload->union_case(), Binary::kValue);
    EXPECT_EQ(payload->value(), std::string(content.begin(), content.end()));
}

TEST(BinaryParameterInterceptor, ResolveRecursesIntoNestedMessage) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{9, 8, 7};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);
    mutableNestedField(wrapper.get())->set_binarytransferuuid(uuid);

    resolveBinaryParameters(store, wrapper.get());

    Binary* inner = mutableNestedField(wrapper.get());
    ASSERT_EQ(inner->union_case(), Binary::kValue);
    EXPECT_EQ(inner->value(), std::string(content.begin(), content.end()));
}

// Codex review of 14ebbe5: the depth guard must not stop at 64 plain hops
// (ValueValidator's Any-nesting limit); a plain message chain deeper than that
// still gets its Binary resolved. The cap is detail::kMaxVisitDepth.
TEST(BinaryParameterInterceptor, ResolveDeepPlainNestingIsNotDepthCapped) {
    InMemoryBinaryStore store;
    const std::string content = "deep";
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, {content.begin(), content.end()});

    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    google::protobuf::Message* level = wrapper->GetReflection()->MutableMessage(
        wrapper.get(), wrapper->GetDescriptor()->FindFieldByName("nested"));
    const google::protobuf::FieldDescriptor* childField =
        level->GetDescriptor()->FindFieldByName("child");
    for (int i = 0; i < 80; ++i) {
        level = level->GetReflection()->MutableMessage(level, childField);
    }
    setBinaryUuidByReflection(
        level->GetReflection()->MutableMessage(level, level->GetDescriptor()->FindFieldByName("inner")),
        uuid);

    sila2::binary::resolveBinaryParameters(store, wrapper.get());

    EXPECT_EQ(binaryValueByReflection(
                  level->GetReflection()->GetMessage(*level, level->GetDescriptor()->FindFieldByName("inner"))),
              content);
}

TEST(BinaryParameterInterceptor, ResolveRepeatedFieldReplacesEachEntry) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> first{1};
    const std::vector<uint8_t> second{2, 2};
    const std::string firstUuid = store.createSlot(first.size(), 1, 60s);
    store.storeChunk(firstUuid, 0, first);
    const std::string secondUuid = store.createSlot(second.size(), 1, 60s);
    store.storeChunk(secondUuid, 0, second);
    addRepeatedBinaryField(wrapper.get(), "payloads")->set_binarytransferuuid(firstUuid);
    addRepeatedBinaryField(wrapper.get(), "payloads")->set_binarytransferuuid(secondUuid);

    resolveBinaryParameters(store, wrapper.get());

    const google::protobuf::Reflection* reflection = wrapper->GetReflection();
    const google::protobuf::FieldDescriptor* field = wrapper->GetDescriptor()->FindFieldByName("payloads");
    auto* entry0 = dynamic_cast<Binary*>(reflection->MutableRepeatedMessage(wrapper.get(), field, 0));
    auto* entry1 = dynamic_cast<Binary*>(reflection->MutableRepeatedMessage(wrapper.get(), field, 1));
    EXPECT_EQ(entry0->value(), std::string(first.begin(), first.end()));
    EXPECT_EQ(entry1->value(), std::string(second.begin(), second.end()));
}

TEST(BinaryParameterInterceptor, ResolveBinaryInsideAnyPayload) {
    // Exercises the dynamic_cast fix directly: a Binary decoded out of an Any
    // payload (AnyCodec::decode) is a DynamicMessage, never the generated
    // sila2::org::silastandard::Binary class.
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{11, 22, 33};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);

    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Binary</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [&uuid](google::protobuf::Message& wrapperMsg, const google::protobuf::FieldDescriptor& payloadField) {
            auto* binaryMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            setBinaryUuidByReflection(binaryMessage, uuid);
        });
    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type(typeXml);
    anyValue->set_payload(payload);

    resolveBinaryParameters(store, wrapper.get());

    Any* resolvedAny = mutableAnyField(wrapper.get(), "anyValue");
    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = decodeAnyPayload(resolvedAny->type(), resolvedAny->payload(), decodePool, decodeFactory);
    const auto* payloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    auto* binaryMessage = decoded->GetReflection()->MutableMessage(decoded.get(), payloadField);
    EXPECT_EQ(binaryValueByReflection(*binaryMessage), std::string(content.begin(), content.end()));
}

TEST(BinaryParameterInterceptor, ResolveBinaryInsideAnyStructure) {
    // Proves recursion inside a decoded Any payload finds a non-top-level
    // Binary (Structure element "Blob"), not just an Any whose payload is a
    // bare Binary.
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{44, 55};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);

    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Structure>"
        "<Element><Identifier>Blob</Identifier><DisplayName>Blob</DisplayName>"
        "<Description>Blob</Description><DataType><Basic>Binary</Basic></DataType></Element>"
        "<Element><Identifier>Count</Identifier><DisplayName>Count</DisplayName>"
        "<Description>Count</Description><DataType><Basic>Integer</Basic></DataType></Element>"
        "</Structure></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [&uuid](google::protobuf::Message& wrapperMsg, const google::protobuf::FieldDescriptor& payloadField) {
            auto* structMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            const auto* blobField = structMessage->GetDescriptor()->FindFieldByName("Blob");
            auto* blobMessage = structMessage->GetReflection()->MutableMessage(structMessage, blobField);
            setBinaryUuidByReflection(blobMessage, uuid);
        });
    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type(typeXml);
    anyValue->set_payload(payload);

    resolveBinaryParameters(store, wrapper.get());

    Any* resolvedAny = mutableAnyField(wrapper.get(), "anyValue");
    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = decodeAnyPayload(resolvedAny->type(), resolvedAny->payload(), decodePool, decodeFactory);
    const auto* payloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    auto* structMessage = decoded->GetReflection()->MutableMessage(decoded.get(), payloadField);
    const auto* blobField = structMessage->GetDescriptor()->FindFieldByName("Blob");
    auto* blobMessage = structMessage->GetReflection()->MutableMessage(structMessage, blobField);
    EXPECT_EQ(binaryValueByReflection(*blobMessage), std::string(content.begin(), content.end()));
}

TEST(BinaryParameterInterceptor, ResolveBinaryInsideNestedAny) {
    // Proves re-serialize propagation through Any-in-Any: resolving the
    // innermost Binary must mark the inner Any changed, which must mark the
    // outer Any changed, or the rewritten bytes never reach the wire field.
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{66, 77, 88, 99};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);

    const std::string binaryTypeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Binary</Basic></DataType>";
    const std::string innerPayload = encodeAnyPayload(
        binaryTypeXml,
        [&uuid](google::protobuf::Message& wrapperMsg, const google::protobuf::FieldDescriptor& payloadField) {
            auto* binaryMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            setBinaryUuidByReflection(binaryMessage, uuid);
        });

    const std::string outerTypeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Any</Basic></DataType>";
    const std::string outerPayload = encodeAnyPayload(
        outerTypeXml,
        [&binaryTypeXml, &innerPayload](google::protobuf::Message& wrapperMsg,
                                        const google::protobuf::FieldDescriptor& payloadField) {
            auto* innerAnyMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            const auto* typeField = innerAnyMessage->GetDescriptor()->FindFieldByName("type");
            const auto* innerPayloadField = innerAnyMessage->GetDescriptor()->FindFieldByName("payload");
            innerAnyMessage->GetReflection()->SetString(innerAnyMessage, typeField, binaryTypeXml);
            innerAnyMessage->GetReflection()->SetString(innerAnyMessage, innerPayloadField, innerPayload);
        });

    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type(outerTypeXml);
    anyValue->set_payload(outerPayload);

    resolveBinaryParameters(store, wrapper.get());

    Any* resolvedAny = mutableAnyField(wrapper.get(), "anyValue");
    google::protobuf::DescriptorPool outerPool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory outerFactory{&outerPool};
    auto outerDecoded = decodeAnyPayload(resolvedAny->type(), resolvedAny->payload(), outerPool, outerFactory);
    const auto* outerPayloadField = outerDecoded->GetDescriptor()->FindFieldByName("Payload");
    auto* decodedInnerAny = outerDecoded->GetReflection()->MutableMessage(outerDecoded.get(), outerPayloadField);

    std::string decodedInnerTypeXml;
    std::string decodedInnerPayloadBytes;
    ASSERT_TRUE(AnyCodec::readAnyFields(*decodedInnerAny, decodedInnerTypeXml, decodedInnerPayloadBytes));
    google::protobuf::DescriptorPool innerPool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory innerFactory{&innerPool};
    auto innermostDecoded =
        decodeAnyPayload(decodedInnerTypeXml, decodedInnerPayloadBytes, innerPool, innerFactory);
    const auto* innermostPayloadField = innermostDecoded->GetDescriptor()->FindFieldByName("Payload");
    auto* innermostBinary =
        innermostDecoded->GetReflection()->MutableMessage(innermostDecoded.get(), innermostPayloadField);
    EXPECT_EQ(binaryValueByReflection(*innermostBinary), std::string(content.begin(), content.end()));
}

TEST(BinaryParameterInterceptor, ResolveDynamicMessageBinaryWithoutCrash) {
    // Isolates the dynamic_cast->reflection fix at the top level: with
    // delegateToGenerated=false, "payload" itself is a DynamicMessage (the
    // exact null-cast shape a Binary decoded out of an Any also has), so this
    // would previously crash before ever reaching an Any.
    DynamicWrapperMessages messages{false};
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::vector<uint8_t> content{1, 2, 3, 4};
    const std::string uuid = store.createSlot(content.size(), 1, 60s);
    store.storeChunk(uuid, 0, content);

    const google::protobuf::Reflection* reflection = wrapper->GetReflection();
    const google::protobuf::FieldDescriptor* payloadField = wrapper->GetDescriptor()->FindFieldByName("payload");
    google::protobuf::Message* payloadBinary = reflection->MutableMessage(wrapper.get(), payloadField);
    setBinaryUuidByReflection(payloadBinary, uuid);

    resolveBinaryParameters(store, wrapper.get());

    EXPECT_EQ(binaryValueByReflection(*payloadBinary), std::string(content.begin(), content.end()));
}

// ---------------------------------------------------------------------------
// resolveBinaryParameters — False (negative/rejection) paths — CAUGHT
// ---------------------------------------------------------------------------

TEST(BinaryParameterInterceptor, ResolveUnknownUuidThrowsFrameworkError) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    mutableBinaryField(wrapper.get(), "payload")->set_binarytransferuuid("not-a-known-uuid");

    // Distinct from the incomplete-upload arm below: this exercises the
    // std::out_of_range catch, that one std::logic_error. Both must report
    // CommandExecutionNotAccepted -- the wire value that used to be an
    // unrouted "Invalid" sentinel serializing as proto 0 (SC2) -- but the
    // message must say "not found" here and NOT "incomplete": an unknown
    // UUID and a short upload are different client-actionable situations
    // (S11).
    try {
        resolveBinaryParameters(store, wrapper.get());
        FAIL() << "expected FrameworkError";
    } catch (const FrameworkError& e) {
        EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted);
        const std::string what = e.what();
        EXPECT_NE(what.find("Binary transfer UUID not found"), std::string::npos);
        EXPECT_NE(what.find("not-a-known-uuid"), std::string::npos);
    }
}

TEST(BinaryParameterInterceptor, ResolveIncompleteUploadThrowsFrameworkError) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    // chunkCount = 2, but only index 0 is ever stored.
    const std::string uuid = store.createSlot(10, 2, 60s);
    store.storeChunk(uuid, 0, {1, 2, 3});
    mutableBinaryField(wrapper.get(), "payload")->set_binarytransferuuid(uuid);

    try {
        resolveBinaryParameters(store, wrapper.get());
        FAIL() << "expected FrameworkError";
    } catch (const FrameworkError& e) {
        EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted);
        const std::string what = e.what();
        EXPECT_NE(what.find("Binary transfer incomplete"), std::string::npos);
        EXPECT_NE(what.find(uuid), std::string::npos);
        EXPECT_EQ(what.find("not found"), std::string::npos);
    }
}

TEST(BinaryParameterInterceptor, ResolveUnknownUuidInsideAnyThrowsFrameworkError) {
    // Proves the FrameworkError propagates out of the Any recursion
    // identically to ResolveUnknownUuidThrowsFrameworkError's top-level case:
    // the try/catch around AnyCodec::decode inside visitAnyPayload only
    // guards the decode step, not the recursive resolve that follows it.
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Binary</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapperMsg, const google::protobuf::FieldDescriptor& payloadField) {
            auto* binaryMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            setBinaryUuidByReflection(binaryMessage, "not-a-known-uuid");
        });
    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type(typeXml);
    anyValue->set_payload(payload);

    try {
        resolveBinaryParameters(store, wrapper.get());
        FAIL() << "expected FrameworkError";
    } catch (const FrameworkError& e) {
        EXPECT_EQ(e.frameworkErrorType(), FrameworkError::FrameworkErrorType::CommandExecutionNotAccepted);
        const std::string what = e.what();
        EXPECT_NE(what.find("Binary transfer UUID not found"), std::string::npos);
        EXPECT_NE(what.find("not-a-known-uuid"), std::string::npos);
    }
}

TEST(BinaryParameterInterceptor, ResolveMalformedAnyLeftUntouched) {
    // A malformed Any is the validator's job to reject as a Validation Error
    // (ValueValidator::validateAnyValue); the interceptor must not raise a
    // different error class, so a type XML that fails to parse leaves the
    // Any's wire fields untouched (visitAnyPayload's catch returns false, no
    // re-serialize).
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type("not-valid-type-xml");
    anyValue->set_payload("arbitrary-bytes");

    EXPECT_NO_THROW(resolveBinaryParameters(store, wrapper.get()));

    Any* unchangedAny = mutableAnyField(wrapper.get(), "anyValue");
    EXPECT_EQ(unchangedAny->type(), "not-valid-type-xml");
    EXPECT_EQ(unchangedAny->payload(), "arbitrary-bytes");
}

// ---------------------------------------------------------------------------
// injectBinaryResults — True (positive) path
// ---------------------------------------------------------------------------

TEST(BinaryParameterInterceptor, InjectAboveThresholdReplacesValueWithStoredUuid) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::string oversized(kBinaryInlineThreshold + 1, 'x');
    mutableBinaryField(wrapper.get(), "payload")->set_value(oversized);

    injectBinaryResults(store, wrapper.get(), 60s);

    Binary* payload = mutableBinaryField(wrapper.get(), "payload");
    ASSERT_EQ(payload->union_case(), Binary::kBinaryTransferUUID);
    const std::vector<uint8_t> assembled = store.assemble(payload->binarytransferuuid());
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), oversized);
}

TEST(BinaryParameterInterceptor, InjectBinaryInsideAnyPayload) {
    // Injection twin of ResolveBinaryInsideAnyPayload: injectBinaryResults
    // gets Any-descent for free by routing through the same
    // detail::visitBinaryFields, so this locks that shared-visitor fix rather
    // than a separately implemented one.
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::string oversized(kBinaryInlineThreshold + 1, 'z');
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Binary</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml,
        [&oversized](google::protobuf::Message& wrapperMsg, const google::protobuf::FieldDescriptor& payloadField) {
            auto* binaryMessage = wrapperMsg.GetReflection()->MutableMessage(&wrapperMsg, &payloadField);
            const auto* valueField = binaryMessage->GetDescriptor()->FindFieldByName("value");
            binaryMessage->GetReflection()->SetString(binaryMessage, valueField, oversized);
        });
    Any* anyValue = mutableAnyField(wrapper.get(), "anyValue");
    anyValue->set_type(typeXml);
    anyValue->set_payload(payload);

    injectBinaryResults(store, wrapper.get(), 60s);

    Any* resolvedAny = mutableAnyField(wrapper.get(), "anyValue");
    google::protobuf::DescriptorPool decodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory decodeFactory{&decodePool};
    auto decoded = decodeAnyPayload(resolvedAny->type(), resolvedAny->payload(), decodePool, decodeFactory);
    const auto* payloadField = decoded->GetDescriptor()->FindFieldByName("Payload");
    auto* binaryMessage = decoded->GetReflection()->MutableMessage(decoded.get(), payloadField);
    const auto* uuidField = binaryMessage->GetDescriptor()->FindFieldByName("binaryTransferUUID");
    const std::string uuid = binaryMessage->GetReflection()->GetString(*binaryMessage, uuidField);
    EXPECT_FALSE(uuid.empty());
    const std::vector<uint8_t> assembled = store.assemble(uuid);
    EXPECT_EQ(std::string(assembled.begin(), assembled.end()), oversized);
}

// ---------------------------------------------------------------------------
// injectBinaryResults — False (negative) path — no-op below threshold
// ---------------------------------------------------------------------------

TEST(BinaryParameterInterceptor, InjectAtThresholdLeavesValueInline) {
    DynamicWrapperMessages messages;
    auto wrapper = messages.newWrapper();
    InMemoryBinaryStore store;

    const std::string atThreshold(kBinaryInlineThreshold, 'y');
    mutableBinaryField(wrapper.get(), "payload")->set_value(atThreshold);

    injectBinaryResults(store, wrapper.get(), 60s);

    Binary* payload = mutableBinaryField(wrapper.get(), "payload");
    EXPECT_EQ(payload->union_case(), Binary::kValue);
    EXPECT_EQ(payload->value(), atThreshold);
    EXPECT_EQ(store.size(), 0u);
}

}  // namespace
