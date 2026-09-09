// test_stress_connections_e2e.cc — stress/soak tests for many concurrent gRPC
// connections (audit 2.1b) and fd/memory stability under sustained
// connect/disconnect churn (audit 2.1c, 2.1e, 2.1k) over the real SiLA TLS
// stack (audit 2.1f-b) rather than a bare insecure gRPC service.
#include <sila/server/SilaServerBase.h>
#include <sila/server/SilaServiceImpl.h>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <fstream>
#endif

namespace {

using sila2::SilaServerBase;
using sila2::silaservice_proto::SiLAService;

constexpr uint16_t kPortManyParallelChannels = 50290;
constexpr uint16_t kPortRapidConnectDisconnect = 50291;
constexpr uint16_t kPortFdStableUnderLoad = 50292;

// Same dial pattern as test_multi_client_concurrent_e2e.cc's dialChannel():
// SilaServerBase only ever serves TLS, so every stub in this file dials
// against the server's own self-signed certificate.
std::shared_ptr<grpc::Channel> dialChannel(const SilaServerBase& server, uint16_t port) {
    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = server.certificatePem();
    return grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(opts));
}

// Runs one connect/RPC/disconnect cycle; channel and stub are destroyed on return.
bool runOneConnectRpcDisconnectCycle(const SilaServerBase& server, uint16_t port) {
    auto stub = SiLAService::NewStub(dialChannel(server, port));
    grpc::ClientContext ctx;
    sila2::silaservice_proto::Get_ServerUUID_Parameters req;
    sila2::silaservice_proto::Get_ServerUUID_Responses resp;
    return stub->Get_ServerUUID(&ctx, req, &resp).ok();
}

#ifdef __linux__
int countOpenFds() {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) return -1;
    int count = 0;
    while (readdir(dir) != nullptr) ++count;
    closedir(dir);
    return count;
}

long readVmRssKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            return std::stol(line.substr(6));
        }
    }
    return -1;
}
#endif

}  // namespace

TEST(StressConnections, ManyParallelChannels) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPortManyParallelChannels)
                      .build();
    server.run(false);

    constexpr int kThreadCount = 64;
    constexpr int kRpcsPerThread = 100;

    // Per-thread result slots avoid a shared counter needing synchronization.
    // vector<char>, not vector<bool>: the latter is bit-packed, so concurrent
    // writes to different indices from different threads would race on the
    // same underlying word.
    std::vector<char> threadAllOk(kThreadCount, false);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([&server, &threadAllOk, t]() {
            auto stub = SiLAService::NewStub(dialChannel(server, kPortManyParallelChannels));
            bool allOk = true;
            for (int i = 0; i < kRpcsPerThread; ++i) {
                grpc::ClientContext ctx;
                sila2::silaservice_proto::Get_ServerUUID_Parameters req;
                sila2::silaservice_proto::Get_ServerUUID_Responses resp;
                allOk = allOk && stub->Get_ServerUUID(&ctx, req, &resp).ok();
            }
            threadAllOk[t] = allOk;
        });
    }
    for (auto& thread : threads) thread.join();

    for (int t = 0; t < kThreadCount; ++t) EXPECT_TRUE(threadAllOk[t]);

    server.shutdown();
}

TEST(StressConnections, RapidConnectDisconnect) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPortRapidConnectDisconnect)
                      .build();
    server.run(false);

    constexpr int kIterations = 200;
    int okCount = 0;

    for (int i = 0; i < kIterations; ++i) {
        if (runOneConnectRpcDisconnectCycle(server, kPortRapidConnectDisconnect)) ++okCount;
    }

    EXPECT_EQ(okCount, kIterations);

    server.shutdown();
}

TEST(StressConnections, FdStableUnderLoad) {
    auto server = SilaServerBase::Builder()
                      .withSelfSignedCertificate("localhost", "127.0.0.1")
                      .withConfig(std::make_unique<sila2::InMemoryServerConfig>("SiLA Server"))
                      .withDiscovery(kPortFdStableUnderLoad)
                      .build();
    server.run(false);

    for (int i = 0; i < 50; ++i) runOneConnectRpcDisconnectCycle(server, kPortFdStableUnderLoad);

#ifdef __linux__
    int fdCountBefore = countOpenFds();
    long rssBeforeKb = readVmRssKb();
#endif

    for (int i = 0; i < 200; ++i) runOneConnectRpcDisconnectCycle(server, kPortFdStableUnderLoad);

#ifdef __linux__
    int fdCountAfter = countOpenFds();
    // Slack of 5 absorbs timing jitter from gRPC's own background threads
    // closing connections asynchronously, not a leak tolerance.
    EXPECT_LE(fdCountAfter - fdCountBefore, 5);

    long rssAfterKb = readVmRssKb();
    // 4 MB slack absorbs TLS session buffers and allocator fragmentation
    // from the churn, not a leak tolerance.
    EXPECT_LE(rssAfterKb - rssBeforeKb, 4096);
#endif

    server.shutdown();
}
