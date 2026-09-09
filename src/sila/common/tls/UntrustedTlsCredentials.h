// UntrustedTlsCredentials.h — TLS channel credentials that encrypt and
// present an identity but verify no peer certificate.
//
// Extracted from client/ClientConfig.cc's anonymous-namespace
// untrustedTlsCredentials() (Part B p75 private-IP zero-config path) once a
// second caller needed the identical shape: SilaServerBase's default
// server-initiated outbound credentials (Part A p32 SHALL support), used
// when the Builder was not given an explicit outbound credential via
// withConnectionConfiguration.
#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace grpc {
class ChannelCredentials;
}  // namespace grpc

namespace sila2::tls {

/// Encrypts the connection and, when privateKeyPem is non-empty, presents
/// (certificatePem, privateKeyPem) as this side's identity — but performs NO
/// verification of the peer's certificate (NoOpCertificateVerifier,
/// set_verify_server_certs(false), set_check_call_host(false)). Suitable only
/// where the caller has an independent reason to trust the peer (a
/// private-range IP literal per Part B p75, or a server presenting its own
/// certificate to a client that binds the connection to a known UUID).
/// @param privateKeyPem Empty means no identity is presented; non-empty must
///        pair with a non-empty certificatePem.
std::shared_ptr<grpc::ChannelCredentials> untrustedTlsChannelCredentials(
    std::string_view certificatePem, std::string_view privateKeyPem);

/// Part B p75: true for an RFC1918 IPv4 literal (10/8, 172.16/12,
/// 192.168/16, also when IPv4-mapped in IPv6) or an RFC4193 ULA (fc00::/7),
/// optionally bracketed. Loopback, link-local and hostnames are NOT private
/// and no DNS resolution is done. Shared by ClientConfig (client-initiated
/// connections) and SilaServerBase's default outbound credentials
/// (server-initiated connections): both legs accept an untrusted peer
/// certificate only inside a private-range network.
bool isPrivateAddress(std::string_view host);

/// Chooses the channel credentials for one outbound target. Returns null when
/// the target may not be connected to under the current trust configuration
/// (a non-private host without a trusted CA, Part B p74/p75).
using OutboundCredentialsProvider =
    std::function<std::shared_ptr<grpc::ChannelCredentials>(std::string_view host)>;

}  // namespace sila2::tls
