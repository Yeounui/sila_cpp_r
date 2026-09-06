// Regression companion to tests/fuzz/fuzz_cloud_router.cc (audit 4.1l):
// pins the "parse, then never crash while reading every field route() would
// read" contract for hand-picked SiLAClientMessage wire-byte inputs. The
// exploratory search over the same input space lives in the libfuzzer
// target; this file fixes specific inputs it has (or could plausibly have)
// flagged to a known outcome.
//
// exerciseRouteFields() below is copied verbatim from
// tests/fuzz/fuzz_cloud_router.cc rather than included from it -- the fuzz
// harness is a .cc file with its own LLVMFuzzerTestOneInput entry point, not
// a library target, and it exercises the same field-access pattern
// CloudEnvelopeRouter::route() takes after ParseFromArray without the full
// dispatch machinery (handler maps, writer, FeatureRegistry) that would
// require booting a server.
#include "SiLACloudConnector.pb.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace {

namespace cloud = sila2::org::silastandard;

// Exercises every field-access path that CloudEnvelopeRouter::route() takes
// after parsing the protobuf. Copied from fuzz_cloud_router.cc's
// exerciseRouteFields -- see file header.
void exerciseRouteFields(const cloud::SiLAClientMessage& msg) {
    (void)msg.requestuuid();

    switch (msg.message_case()) {
        case cloud::SiLAClientMessage::kUnobservableCommandExecution: {
            const auto& exec = msg.unobservablecommandexecution();
            (void)exec.fullyqualifiedcommandid();
            (void)exec.commandparameter().parameters();
            for (const auto& md : exec.commandparameter().metadata()) {
                (void)md.fullyqualifiedmetadataid();
                (void)md.value();
            }
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandInitiation: {
            const auto& init = msg.observablecommandinitiation();
            (void)init.fullyqualifiedcommandid();
            (void)init.commandparameter().parameters();
            for (const auto& md : init.commandparameter().metadata()) {
                (void)md.fullyqualifiedmetadataid();
                (void)md.value();
            }
            break;
        }
        case cloud::SiLAClientMessage::kUnobservablePropertyRead: {
            const auto& read = msg.unobservablepropertyread();
            (void)read.fullyqualifiedpropertyid();
            for (const auto& md : read.metadata()) {
                (void)md.fullyqualifiedmetadataid();
                (void)md.value();
            }
            break;
        }
        case cloud::SiLAClientMessage::kObservablePropertySubscription: {
            const auto& sub = msg.observablepropertysubscription();
            (void)sub.fullyqualifiedpropertyid();
            for (const auto& md : sub.metadata()) {
                (void)md.fullyqualifiedmetadataid();
                (void)md.value();
            }
            break;
        }
        case cloud::SiLAClientMessage::kCancelObservableCommandExecutionInfoSubscription:
        case cloud::SiLAClientMessage::kCancelObservableCommandIntermediateResponseSubscription:
        case cloud::SiLAClientMessage::kCancelObservablePropertySubscription:
            break;
        case cloud::SiLAClientMessage::kObservableCommandExecutionInfoSubscription: {
            const auto& sub = msg.observablecommandexecutioninfosubscription();
            (void)sub.commandexecutionuuid().value();
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandIntermediateResponseSubscription: {
            const auto& sub = msg.observablecommandintermediateresponsesubscription();
            (void)sub.commandexecutionuuid().value();
            break;
        }
        case cloud::SiLAClientMessage::kObservableCommandGetResponse: {
            const auto& resp = msg.observablecommandgetresponse();
            (void)resp.commandexecutionuuid().value();
            break;
        }
        case cloud::SiLAClientMessage::kCreateBinaryUploadRequest: {
            const auto& uploadReq = msg.createbinaryuploadrequest();
            for (const auto& md : uploadReq.metadata()) {
                (void)md.fullyqualifiedmetadataid();
                (void)md.value();
            }
            const auto& req = uploadReq.createbinaryrequest();
            (void)req.binarysize();
            (void)req.chunkcount();
            (void)req.parameteridentifier();
            break;
        }
        case cloud::SiLAClientMessage::kUploadChunkRequest: {
            const auto& req = msg.uploadchunkrequest();
            (void)req.binarytransferuuid();
            (void)req.chunkindex();
            (void)req.payload();
            break;
        }
        case cloud::SiLAClientMessage::kDeleteUploadedBinaryRequest: {
            (void)msg.deleteuploadedbinaryrequest().binarytransferuuid();
            break;
        }
        case cloud::SiLAClientMessage::kGetBinaryInfoRequest: {
            (void)msg.getbinaryinforequest().binarytransferuuid();
            break;
        }
        case cloud::SiLAClientMessage::kGetChunkRequest: {
            const auto& req = msg.getchunkrequest();
            (void)req.binarytransferuuid();
            (void)req.offset();
            (void)req.length();
            break;
        }
        case cloud::SiLAClientMessage::kDeleteDownloadedBinaryRequest: {
            (void)msg.deletedownloadedbinaryrequest().binarytransferuuid();
            break;
        }
        case cloud::SiLAClientMessage::kMetadataRequest: {
            (void)msg.metadatarequest().fullyqualifiedmetadataid();
            break;
        }
        case cloud::SiLAClientMessage::MESSAGE_NOT_SET:
            break;
    }
}

// Mirrors fuzz_cloud_router.cc's LLVMFuzzerTestOneInput: parses `bytes` as a
// SiLAClientMessage and, only on a successful parse, exercises every field
// read route() would perform -- matching CloudTransport, which never hands
// route() a message that failed to parse. Returns whether the parse
// succeeded.
bool parseAndExercise(const std::string& bytes) {
    cloud::SiLAClientMessage msg;
    if (!msg.ParseFromString(bytes)) return false;
    exerciseRouteFields(msg);
    return true;
}

}  // namespace

// --- True (positive) paths --------------------------------------------------

TEST(CloudRouterFuzzRegression, UnobservableCommandExecutionWithEmbeddedNulBytesParses) {
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-1");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/v1/Feature/Command/RunTask/1");
    // Metadata.value and CommandParameter.parameters are `bytes`, not
    // `string`, so an embedded NUL is a legal payload byte, unlike in the
    // UTF-8-validated string fields exercised by the false paths below.
    const std::string withNul = std::string("a") + '\0' + "b";
    exec->mutable_commandparameter()->set_parameters(withNul);
    auto* md = exec->mutable_commandparameter()->add_metadata();
    md->set_fullyqualifiedmetadataid("org.test/v1/Feature/Metadata/Tag/1");
    md->set_value(withNul);

    ASSERT_TRUE(parseAndExercise(msg.SerializeAsString()));
}

TEST(CloudRouterFuzzRegression, ObservableCommandExecutionInfoSubscriptionParses) {
    // A distinct oneof branch from the command-execution one above: only the
    // nested CommandExecutionUUID is read, no metadata loop.
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-2");
    msg.mutable_observablecommandexecutioninfosubscription()
        ->mutable_commandexecutionuuid()
        ->set_value("11111111-2222-3333-4444-555555555555");

    ASSERT_TRUE(parseAndExercise(msg.SerializeAsString()));
}

TEST(CloudRouterFuzzRegression, CreateBinaryUploadRequestWithMaximalNumericFieldsParses) {
    // Exercises the deepest-nested branch plus maximum-value numeric fields
    // (uint64/uint32 wire varints at their widest encoding).
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-3");
    auto* uploadReq = msg.mutable_createbinaryuploadrequest();
    auto* req = uploadReq->mutable_createbinaryrequest();
    req->set_binarysize(std::numeric_limits<uint64_t>::max());
    req->set_chunkcount(std::numeric_limits<uint32_t>::max());
    req->set_parameteridentifier(std::string(4096, 'x'));

    ASSERT_TRUE(parseAndExercise(msg.SerializeAsString()));
}

TEST(CloudRouterFuzzRegression, GetChunkRequestParses) {
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-4");
    auto* req = msg.mutable_getchunkrequest();
    req->set_binarytransferuuid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    req->set_offset(std::numeric_limits<uint64_t>::max());
    req->set_length(std::numeric_limits<uint32_t>::max());

    ASSERT_TRUE(parseAndExercise(msg.SerializeAsString()));
}

TEST(CloudRouterFuzzRegression, EmptyInputParsesAsMessageNotSet) {
    // A zero-length byte string is a valid encoding of an all-default proto3
    // message (spec-defined, not a parse failure): every field, including
    // the oneof, reads back as unset. This is the MESSAGE_NOT_SET branch,
    // distinct from every branch above.
    ASSERT_TRUE(parseAndExercise(""));
}

// --- False (negative) paths -------------------------------------------------
// All CAUGHT: ParseFromString returns false and exerciseRouteFields is never
// reached, matching production (CloudTransport never routes a message that
// failed to parse).

TEST(CloudRouterFuzzRegression, SingleZeroByteRejected) {
    // Tag byte 0x00 decodes to field number 0, which is not a legal field
    // number in any protobuf message.
    const std::string oneZeroByte{'\x00'};
    EXPECT_FALSE(parseAndExercise(oneZeroByte));
}

TEST(CloudRouterFuzzRegression, SingleFFByteRejected) {
    // 0xFF's continuation bit is set, so the tag varint is truncated with no
    // more bytes to continue into.
    const std::string oneFFByte{'\xFF'};
    EXPECT_FALSE(parseAndExercise(oneFFByte));
}

TEST(CloudRouterFuzzRegression, RandomGarbageBytesRejected) {
    const std::string garbage = "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE\x7F\x1B";
    EXPECT_FALSE(parseAndExercise(garbage));
}

TEST(CloudRouterFuzzRegression, TruncatedValidMessageRejected) {
    // Serializes a valid, complete message, then cuts it off mid-field: the
    // final length-delimited field's declared length no longer matches the
    // bytes actually present.
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("req-5");
    auto* exec = msg.mutable_unobservablecommandexecution();
    exec->set_fullyqualifiedcommandid("org.test/v1/Feature/Command/RunTask/1");
    exec->mutable_commandparameter()->set_parameters(std::string(64, 'p'));

    const std::string full = msg.SerializeAsString();
    ASSERT_GT(full.size(), 10u);
    const std::string truncated = full.substr(0, full.size() - 10);

    EXPECT_FALSE(parseAndExercise(truncated));
}

TEST(CloudRouterFuzzRegression, InvalidUtf8InStringFieldRejected) {
    // proto3 string fields (as opposed to bytes fields) are UTF-8-validated
    // on parse. requestUUID is field 1 of SiLAClientMessage with wire type 2
    // (length-delimited): tag byte 0x0A, length 2, followed by a lone
    // continuation byte pair that is not valid UTF-8.
    const std::string wireBytes{'\x0A', '\x02', '\xFF', '\xFE'};
    EXPECT_FALSE(parseAndExercise(wireBytes));
}
