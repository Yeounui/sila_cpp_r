// GenericDynamicTestServer.h — shared in-process test server for
// GenericStub-based dynamic dispatch (architecture.md §4.2, §5).
//
// DynamicCall::callUnary/callServerStream, ExecutionInfoSubscriber, and
// ObservableCommandRunner all talk to arbitrary gRPC method paths through
// raw ByteBuffers rather than a generated .proto service. gRPC's wire
// protocol doesn't distinguish streaming shapes at the transport level, so
// a single grpc::AsyncGenericService accepts calls of every RPC type
// (unary, server-streaming) uniformly as "read N requests, write M
// responses, finish" — this harness exposes that as a per-method-path
// script so tests can inject exact response sequences and error timing
// without standing up a real generated service or full SiLAServerBase.
#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/byte_buffer.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sila2 {
namespace test {

// One scripted call: the messages to write back and the final status.
// `request` holds whatever single message the client wrote (all call
// sites in this codebase write exactly one request message before
// WritesDone/half-close).
struct ScriptedResponse {
    std::vector<grpc::ByteBuffer> messages;
    grpc::Status status = grpc::Status::OK;
};

using MethodHandler = std::function<ScriptedResponse(const grpc::ByteBuffer& request)>;

// In-process server that accepts any RPC via the generic (untyped) service
// API and replies according to a per-method-path script registered with
// on(). Backs DynamicCall, ObservableCommandRunner, and
// ExecutionInfoSubscriber integration tests alike.
class GenericDynamicTestServer {
public:
    // Defaulted to insecure so the 7 raw-harness callers (which reach the
    // server only through channel(), also insecure below) stay unchanged;
    // the one test routed through ClientConfig (S74, Part B p74: no
    // plaintext) passes SslServerCredentials instead.
    explicit GenericDynamicTestServer(
        std::shared_ptr<grpc::ServerCredentials> credentials = grpc::InsecureServerCredentials()) {
        grpc::ServerBuilder builder;
        builder.AddListeningPort("127.0.0.1:0", std::move(credentials), &port_);
        builder.RegisterAsyncGenericService(&service_);
        notificationCq_ = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();
        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    ~GenericDynamicTestServer() {
        server_->Shutdown();
        notificationCq_->Shutdown();
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        std::lock_guard<std::mutex> lock(threadsMu_);
        for (auto& t : handlerThreads_) {
            if (t.joinable()) t.join();
        }
    }

    GenericDynamicTestServer(const GenericDynamicTestServer&) = delete;
    GenericDynamicTestServer& operator=(const GenericDynamicTestServer&) = delete;

    int port() const { return port_; }

    std::shared_ptr<grpc::Channel> channel() const {
        return grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials());
    }

    // Registers the handler invoked for `method` (full gRPC path, e.g.
    // "/pkg.Service/Rpc"). A call to a path with no registered handler
    // is answered with UNIMPLEMENTED.
    void on(const std::string& method, MethodHandler handler) {
        std::lock_guard<std::mutex> lock(handlersMu_);
        handlers_[method] = std::move(handler);
    }

private:
    // Leapfrogging accept loop (standard async-server pattern): queue the
    // next RequestCall, wait for one to land, hand it off to a worker
    // thread, repeat.
    void acceptLoop() {
        while (true) {
            auto ctx = std::make_unique<grpc::GenericServerContext>();
            auto stream = std::make_unique<grpc::GenericServerAsyncReaderWriter>(ctx.get());
            auto callCq = std::make_unique<grpc::CompletionQueue>();
            void* acceptTag = ctx.get();
            service_.RequestCall(ctx.get(), stream.get(), callCq.get(), notificationCq_.get(), acceptTag);

            void* tag = nullptr;
            bool ok = false;
            if (!notificationCq_->Next(&tag, &ok) || !ok) {
                break;  // server shutting down
            }

            std::lock_guard<std::mutex> lock(threadsMu_);
            handlerThreads_.emplace_back(
                [this, ctx = std::move(ctx), stream = std::move(stream), callCq = std::move(callCq)]() mutable {
                    handleCall(*ctx, *stream, *callCq);
                });
        }
    }

    void handleCall(
        grpc::GenericServerContext& ctx,
        grpc::GenericServerAsyncReaderWriter& stream,
        grpc::CompletionQueue& cq) {
        grpc::ByteBuffer request;
        void* tag = &request;
        stream.Read(&request, tag);
        void* t = nullptr;
        bool ok = false;
        if (!cq.Next(&t, &ok) || !ok) {
            cq.Shutdown();
            return;
        }

        MethodHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlersMu_);
            auto it = handlers_.find(ctx.method());
            if (it != handlers_.end()) handler = it->second;
        }

        const ScriptedResponse response = handler
            ? handler(request)
            : ScriptedResponse{{}, grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "no handler for " + ctx.method())};

        for (const auto& message : response.messages) {
            // Copy: Write() takes a non-const ref to a mutable ByteBuffer tag
            // pairing below, but the message itself is only read from.
            grpc::ByteBuffer copy = message;
            stream.Write(copy, tag);
            if (!cq.Next(&t, &ok) || !ok) {
                cq.Shutdown();
                return;
            }
        }

        stream.Finish(response.status, tag);
        cq.Next(&t, &ok);
        cq.Shutdown();
    }

    int port_ = 0;
    grpc::AsyncGenericService service_;
    std::unique_ptr<grpc::ServerCompletionQueue> notificationCq_;
    std::unique_ptr<grpc::Server> server_;
    std::thread acceptThread_;

    std::mutex threadsMu_;
    std::vector<std::thread> handlerThreads_;

    std::mutex handlersMu_;
    std::map<std::string, MethodHandler> handlers_;
};

// Serializes a protobuf message into a single-slice ByteBuffer, matching
// how DynamicCall's callers hand requests to callUnary/callServerStream.
template <typename Message>
grpc::ByteBuffer toByteBuffer(const Message& message) {
    const std::string serialized = message.SerializeAsString();
    grpc::Slice slice(serialized);
    return grpc::ByteBuffer(&slice, 1);
}

// Concatenates a ByteBuffer's slices back into a plain string, for
// asserting on/parsing raw response bytes in tests.
inline std::string fromByteBuffer(const grpc::ByteBuffer& buffer) {
    std::string result;
    std::vector<grpc::Slice> slices;
    buffer.Dump(&slices);
    for (const auto& slice : slices) {
        result.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    }
    return result;
}

}  // namespace test
}  // namespace sila2
