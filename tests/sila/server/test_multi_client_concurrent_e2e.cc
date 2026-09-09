// End-to-end test for concurrent RPC clients (architecture.md §2.2a):
// several independent gRPC channels hitting the same SilaServerBase at once
// must all get correctly-served responses. test_interop.cc only drives a
// single, sequential client — this exercises the server under real
// concurrent load instead.
#include <sila/server/SilaServerBase.h>
#include <sila/server/SilaServiceImpl.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <string>
#include <thread>
#include <vector>

namespace {

using sila2::SilaServerBase;
using sila2::silaservice_proto::SiLAService;

constexpr uint16_t kPortMultiClient = 50270;
constexpr int kClientCount = 4;
constexpr int kCallsPerClient = 10;

// SilaServerBase only ever serves TLS (self-signed here); same dial pattern
// as test_sila_server_base_run_shutdown_e2e.cc's dialChannel().
std::shared_ptr<grpc::Channel> dialChannel(const SilaServerBase& server, uint16_t port) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

}  // namespace

TEST(MultiClientConcurrent, FourClientsGetServerName) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPortMultiClient)
                      .build();
    server.run(false);

    const std::string expectedName = server.serverConfig().name();

    std::vector<std::thread> clients;
    std::vector<std::vector<std::string>> resultsPerClient(kClientCount);

    for (int c = 0; c < kClientCount; ++c) {
        clients.emplace_back([&, c] {
            // Each client dials its own channel/stub, not shared across threads.
            auto stub = SiLAService::NewStub(dialChannel(server, kPortMultiClient));
            for (int i = 0; i < kCallsPerClient; ++i) {
                grpc::ClientContext ctx;
                sila2::silaservice_proto::Get_ServerName_Parameters req;
                sila2::silaservice_proto::Get_ServerName_Responses resp;
                auto status = stub->Get_ServerName(&ctx, req, &resp);
                if (status.ok()) {
                    resultsPerClient[c].push_back(resp.servername().value());
                } else {
                    resultsPerClient[c].push_back("");
                }
            }
        });
    }
    for (auto& th : clients) { th.join(); }

    for (int c = 0; c < kClientCount; ++c) {
        ASSERT_EQ(resultsPerClient[c].size(), static_cast<size_t>(kCallsPerClient));
        for (const auto& name : resultsPerClient[c]) {
            EXPECT_EQ(name, expectedName);
        }
    }

    server.shutdown();
}
