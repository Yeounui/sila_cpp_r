// Publish↔browse round-trip for mDNS discovery (SiLA 2 §5.4 Discovery):
// MdnsPublisher::publish() announces a _sila._tcp service, MdnsBrowser::browse()
// must resolve it into a ResolveEvent carrying the same uuid/port/name, and
// MdnsPublisher::shutdown() must be observed as a goodbye for that uuid.
#include <sila/client/discovery/MdnsBrowser.h>
#include <sila/common/discovery/MdnsSocketPair.h>
#include <sila/server/discovery/MdnsPublisher.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using sila2::discovery::MdnsBrowser;
using sila2::discovery::MdnsPublisher;
using sila2::discovery::MdnsSocketPair;
using sila2::discovery::ResolveEvent;

namespace {

// MDNS_PORT (5353) may already be bound by avahi/systemd-resolved, and
// multicast loopback may not work (e.g. WSL2).  Skip rather than fail.
bool mdnsPortAvailable() {
    // WSL2 multicast loopback is unreliable; skip early.
    std::ifstream uts("/proc/version");
    std::string ver;
    if (uts && std::getline(uts, ver) && ver.find("microsoft") != std::string::npos)
        return false;

    MdnsSocketPair probe;
    probe.open();
    return probe.ipv4 >= 0;
}

constexpr auto kEventTimeout = std::chrono::seconds{5};

}  // namespace

TEST(MdnsBrowserLoopback, ResolvesPublishedServiceAndObservesGoodbye)
{
    if (!mdnsPortAvailable()) {
        GTEST_SKIP() << "MDNS_PORT (5353) unavailable on this host";
    }

    MdnsPublisher publisher{"test-uuid-1234", "TestServer", "", "", 50051,
                            std::chrono::seconds{60}, std::chrono::seconds{120},
                            std::chrono::milliseconds{250}};

    std::promise<ResolveEvent> resolvedPromise;
    std::future<ResolveEvent> resolvedFuture = resolvedPromise.get_future();
    std::atomic<bool> resolved{false};

    std::promise<std::string> goodbyePromise;
    std::future<std::string> goodbyeFuture = goodbyePromise.get_future();

    MdnsBrowser browser;
    browser.browse(
        [&](const ResolveEvent& event) {
            // The readvertise timer may resend the same announcement; only the
            // first resolve should satisfy the promise.
            if (!resolved.exchange(true)) {
                resolvedPromise.set_value(event);
            }
        },
        [&](const std::string& uuid) { goodbyePromise.set_value(uuid); });

    publisher.publish();

    ASSERT_EQ(resolvedFuture.wait_for(kEventTimeout), std::future_status::ready);
    ResolveEvent event = resolvedFuture.get();
    EXPECT_EQ(event.uuid, "test-uuid-1234");
    EXPECT_EQ(event.port, 50051);
    // Part B p76 MUST: the mDNS Service Instance Name is the SiLA Server UUID,
    // not the human-readable ServerName -- so the resolved instance (event.name,
    // MdnsBrowser.h:17-18's own naming) equals the UUID, with no conflict-retry
    // suffix (the UUID is globally unique, S67).
    EXPECT_EQ(event.name, "test-uuid-1234");
    // The publisher advertises one A record per non-loopback IPv4 address;
    // when the host has any, the browser must surface it as a dotted literal
    // (the value ClientConfig's private-range TLS rule keys on).
    if (!event.address.empty()) {
        struct in_addr parsed{};
        EXPECT_EQ(inet_pton(AF_INET, event.address.c_str(), &parsed), 1) << event.address;
    }

    publisher.shutdown();

    ASSERT_EQ(goodbyeFuture.wait_for(kEventTimeout), std::future_status::ready);
    EXPECT_EQ(goodbyeFuture.get(), "test-uuid-1234");

    browser.stop();
}

TEST(MdnsSocketPair, OpenReturnsFailureWhenDescriptorsAreExhausted) {
    const pid_t child = fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        struct rlimit limit{32, 32};
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) _exit(2);
        while (open("/dev/null", O_RDONLY) >= 0) {}
        MdnsSocketPair sockets;
        sockets.open();
        _exit(sockets.ipv4 < 0 && sockets.ipv6 < 0 ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(MdnsBrowser, BrowseCalledTwiceDoesNotTerminate) {
    MdnsBrowser browser;
    browser.browse({}, {});
    browser.browse({}, {});
    browser.stop();
}

TEST(MdnsPublisher, PublishReportsExhaustedDescriptors) {
    const pid_t child = fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        struct rlimit limit{32, 32};
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) _exit(2);
        while (open("/dev/null", O_RDONLY) >= 0) {}
        try {
            MdnsPublisher publisher{"uuid", "test", "", "", 12345,
                                    std::chrono::seconds{60}, std::chrono::seconds{120},
                                    std::chrono::milliseconds{50}};
            publisher.publish();
        } catch (const std::runtime_error&) {
            _exit(0);
        }
        _exit(1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
