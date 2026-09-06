// Tests for MetadataInjector: headerKey derivation and set/remove/clear/apply
// against a real grpc::ClientContext/ServerContext round trip, so the -bin
// binary-header encode/decode grpc performs on the wire is exercised too.
#include <sila/client/MetadataInjector.h>

#include <sila/server/features/AuthenticationServiceImpl.h>  // auth_proto::AuthenticationService, generated Service/Stub

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace
{
using sila2::MetadataInjector;
namespace auth_proto = sila2::auth_proto;

// Captures the client metadata a Login RPC arrived with; request/response
// contents are irrelevant, only context->client_metadata() is inspected.
class MetadataCapturingService final : public auth_proto::AuthenticationService::Service {
public:
    grpc::Status Login(grpc::ServerContext* context, const auth_proto::Login_Parameters*,
                        auth_proto::Login_Responses*) override {
        std::lock_guard<std::mutex> lock(mu_);
        receivedMetadata_.clear();
        for (const auto& [key, value] : context->client_metadata()) {
            receivedMetadata_.emplace(std::string(key.data(), key.length()), std::string(value.data(), value.length()));
        }
        return grpc::Status::OK;
    }

    grpc::Status Logout(grpc::ServerContext*, const auth_proto::Logout_Parameters*,
                         auth_proto::Logout_Responses*) override {
        return grpc::Status::OK;
    }

    std::multimap<std::string, std::string> receivedMetadata() const {
        std::lock_guard<std::mutex> lock(mu_);
        return receivedMetadata_;
    }

private:
    mutable std::mutex mu_;
    std::multimap<std::string, std::string> receivedMetadata_;
};

// Real local server+channel: the only way to observe what ClientContext::
// AddMetadata actually put on the wire, since ClientContext exposes no
// public getter for outgoing metadata before the call is issued.
class LocalAuthServer {
public:
    LocalAuthServer() {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
        stub_ = auth_proto::AuthenticationService::NewStub(channel_);
    }

    ~LocalAuthServer() { server_->Shutdown(); }

    std::multimap<std::string, std::string> callWith(const MetadataInjector& injector) {
        grpc::ClientContext ctx;
        injector.apply(ctx);
        auth_proto::Login_Parameters request;
        auth_proto::Login_Responses response;
        stub_->Login(&ctx, request, &response);
        return service_.receivedMetadata();
    }

private:
    MetadataCapturingService service_;
    std::unique_ptr<grpc::Server> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<auth_proto::AuthenticationService::Stub> stub_;
};

const std::string kLockIdentifierFqi = "org.silastandard/core/LockController/v1/Metadata/LockIdentifier";
const std::string kOtherFqi = "org.silastandard/core/OtherFeature/v1/Metadata/Other";

// ---------------------------------------------------------------------------
// True (positive) paths
// ---------------------------------------------------------------------------

TEST(MetadataInjector, SetAndApplyAttachesBinaryHeaderKeyAndValue) {
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    LocalAuthServer server;

    const auto received = server.callWith(injector);

    const auto it = received.find(MetadataInjector::headerKey(kLockIdentifierFqi));
    ASSERT_NE(it, received.end());
    EXPECT_EQ(it->second, "lock-value");
}

TEST(MetadataInjector, HeaderKeyDerivesFromMultiLevelFqi) {
    const std::string key = MetadataInjector::headerKey(kLockIdentifierFqi);

    // Only '/' folds to '-'; the originator's dots stay, matching the FQI
    // grammar a normative server restores the key against (see
    // MetadataHeaderKey.h). This assertion flipped from the dashed form the
    // fork used to emit -- that dialect had no real peer.
    EXPECT_EQ(key, "sila-org.silastandard-core-lockcontroller-v1-metadata-lockidentifier-bin");
}

TEST(MetadataInjector, HeaderKeyLowercasesAndKeepsDotsForAccessToken) {
    const std::string key = MetadataInjector::headerKey(
        "org.silastandard/core/AuthorizationService/v1/Metadata/AccessToken");

    EXPECT_EQ(key, "sila-org.silastandard-core-authorizationservice-v1-metadata-accesstoken-bin");
}

TEST(MetadataInjector, MultipleSetCallsAttachAllEntries) {
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kOtherFqi, "other-value");
    LocalAuthServer server;

    const auto received = server.callWith(injector);

    EXPECT_EQ(received.count(MetadataInjector::headerKey(kLockIdentifierFqi)), 1u);
    EXPECT_EQ(received.count(MetadataInjector::headerKey(kOtherFqi)), 1u);
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockIdentifierFqi))->second, "lock-value");
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kOtherFqi))->second, "other-value");
}

TEST(MetadataInjector, SetSameFqiTwiceOverwritesPreviousValue) {
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "first-value");
    injector.set(kLockIdentifierFqi, "second-value");
    LocalAuthServer server;

    const auto received = server.callWith(injector);

    EXPECT_EQ(received.count(MetadataInjector::headerKey(kLockIdentifierFqi)), 1u);
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockIdentifierFqi))->second, "second-value");
}

// ---------------------------------------------------------------------------
// False (negative/edge) paths
// ---------------------------------------------------------------------------

TEST(MetadataInjector, HeaderKeyNoLongerFoldsOriginatorDots) {
    // Pins that the dashed dialect (folding '.' alongside '/') can never
    // silently come back: it has no real peer (see MetadataHeaderKey.h).
    EXPECT_NE(MetadataInjector::headerKey(kLockIdentifierFqi),
              "sila-org-silastandard-core-lockcontroller-v1-metadata-lockidentifier-bin");
    EXPECT_EQ(MetadataInjector::headerKey(kLockIdentifierFqi).find("org.silastandard"), 5u);
}

TEST(MetadataInjector, ClearRemovesAllEntriesApplyAttachesNothing) {
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kOtherFqi, "other-value");

    injector.clear();

    LocalAuthServer server;
    // grpc itself always sends a "user-agent" header, so this checks that no
    // sila-*-bin metadata header arrives rather than that received is empty.
    const auto received = server.callWith(injector);
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockIdentifierFqi)), received.end());
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kOtherFqi)), received.end());
}

TEST(MetadataInjector, RemoveExistingEntryStopsFutureApply) {
    MetadataInjector injector;
    injector.set(kLockIdentifierFqi, "lock-value");
    injector.set(kOtherFqi, "other-value");

    injector.remove(kLockIdentifierFqi);

    LocalAuthServer server;
    const auto received = server.callWith(injector);
    EXPECT_EQ(received.find(MetadataInjector::headerKey(kLockIdentifierFqi)), received.end());
    EXPECT_NE(received.find(MetadataInjector::headerKey(kOtherFqi)), received.end());
}

}  // namespace
