// fuzz_cloud_router.cc — libfuzzer target for cloud envelope parsing (audit §3.9)
//
// Fuzzes SiLAClientMessage deserialization and the field-access patterns used
// by CloudEnvelopeRouter::route() — the switch on message_case() and nested
// field reads that process untrusted wire bytes from the cloud bidi stream.
//
// build:
//   conda run -n sica clang++ -std=c++20 -fsanitize=fuzzer,address \
//       -I src -I build/src/sila -I build/vcpkg_installed/x64-linux/include \
//       tests/fuzz/fuzz_cloud_router.cc \
//       build/src/sila/SiLACloudConnector.pb.cc \
//       build/src/sila/SiLAFramework.pb.cc \
//       build/src/sila/SiLABinaryTransfer.pb.cc \
//       -L build/vcpkg_installed/x64-linux/lib \
//       -lprotobuf -labsl_log_internal_check_op -labsl_log_internal_message \
//       -labsl_status -labsl_cord -labsl_strings -labsl_str_format_internal \
//       -labsl_string_view -labsl_raw_logging_internal -labsl_spinlock_wait \
//       -labsl_base -lutf8_validity \
//       -o build/fuzz_cloud_router
//
// Run:
//   ./build/fuzz_cloud_router -max_total_time=300

#include "SiLACloudConnector.pb.h"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace cloud = sila2::org::silastandard;

// Exercises every field-access path that CloudEnvelopeRouter::route() takes
// after parsing the protobuf. This is the same switch/read pattern, without
// the dispatch machinery (handler maps, writer, etc.) that would need the
// full server dependency tree.
static void exerciseRouteFields(const cloud::SiLAClientMessage& msg) {
    // requestUUID is read on every path.
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
            // These only read requestuuid (already accessed above).
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

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    cloud::SiLAClientMessage msg;
    if (msg.ParseFromArray(data, static_cast<int>(size))) {
        exerciseRouteFields(msg);
    }
    return 0;
}
