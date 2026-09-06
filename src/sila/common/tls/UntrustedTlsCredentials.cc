// UntrustedTlsCredentials.cc
#include "UntrustedTlsCredentials.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_certificate_verifier.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sila2::tls {

std::shared_ptr<grpc::ChannelCredentials> untrustedTlsChannelCredentials(
    std::string_view certificatePem, std::string_view privateKeyPem) {
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_verify_server_certs(false);
    options.set_check_call_host(false);
    options.set_certificate_verifier(
        std::make_shared<grpc::experimental::NoOpCertificateVerifier>());
    // Present an identity if configured (mTLS to an untrusted peer).
    // set_identity_certificate_provider is the non-deprecated setter replacing
    // set_certificate_provider()+watch_identity_key_cert_pairs(); it attaches
    // only identity certs, leaving root verification off as set above.
    if (!privateKeyPem.empty()) {
        std::vector<grpc::experimental::IdentityKeyCertPair> identity;
        identity.push_back({std::string{privateKeyPem}, std::string{certificatePem}});
        options.set_identity_certificate_provider(
            std::make_shared<grpc::experimental::StaticDataCertificateProvider>(identity));
    }
    return grpc::experimental::TlsCredentials(options);
}

namespace {

// RFC1918 private IPv4 ranges (Part B p75), tested in host byte order.
bool isPrivateIpv4(std::uint32_t address) {
    if ((address & 0xff000000u) == 0x0a000000u) {  // 10.0.0.0/8
        return true;
    }
    if ((address & 0xfff00000u) == 0xac100000u) {  // 172.16.0.0/12
        return true;
    }
    if ((address & 0xffff0000u) == 0xc0a80000u) {  // 192.168.0.0/16
        return true;
    }
    return false;
}

}  // namespace

bool isPrivateAddress(std::string_view host) {
    // inet_pton needs a NUL-terminated string; IPv6 connection targets are
    // bracketed ("[fd00::1]"), which inet_pton rejects, so strip the brackets.
    // Only the private-IP rule of Part B p75 is honoured here: loopback
    // (127/8, ::1), link-local (169.254/16, fe80::/10) and hostnames (anything
    // inet_pton rejects, "localhost" included) are NOT private, and NO DNS
    // resolution is done — gRPC resolves the name again and could differ.
    // Part B p74: outside this rule a peer MUST NOT implicitly accept an
    // untrusted certificate.
    std::string text{host};
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    in_addr ipv4{};
    if (inet_pton(AF_INET, text.c_str(), &ipv4) == 1) {
        return isPrivateIpv4(ntohl(ipv4.s_addr));
    }
    in6_addr ipv6{};
    if (inet_pton(AF_INET6, text.c_str(), &ipv6) == 1) {
        // An IPv4-mapped address (::ffff:a.b.c.d) IS the embedded IPv4, not a
        // distinct range — classify it by those four octets.
        if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
            std::uint32_t mapped = (static_cast<std::uint32_t>(ipv6.s6_addr[12]) << 24)
                                 | (static_cast<std::uint32_t>(ipv6.s6_addr[13]) << 16)
                                 | (static_cast<std::uint32_t>(ipv6.s6_addr[14]) << 8)
                                 | static_cast<std::uint32_t>(ipv6.s6_addr[15]);
            return isPrivateIpv4(mapped);
        }
        // RFC4193 ULA fc00::/7: first byte masked 0xfe equals 0xfc (fc00::-fdff:).
        return (ipv6.s6_addr[0] & 0xfe) == 0xfc;
    }
    return false;  // not an IP literal -> not private (no DNS).
}

}  // namespace sila2::tls
