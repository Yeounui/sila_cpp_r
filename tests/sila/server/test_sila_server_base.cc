// Checks for SiLAServerBase::Builder: Feature registration delegates to
// FeatureRegistry (including its duplicate-FQI throw), WithSelfSignedCertificate
// produces PEM material while WithCertificate stores caller-supplied PEM
// verbatim without parsing it, and Build() refuses to run without either.
#include <sila/server/SiLAServerBase.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

TEST(SiLAServerBaseBuilder, RegistersFeaturesIntoTheAssembledRegistry)
{
    const auto server = sila2::SiLAServerBase::Builder()
                             .WithSelfSignedCertificate("SiLA2", "127.0.0.1")
                             .AddFeature("org.silastandard/core/SiLAService/v1", "<Feature/>")
                             .Build();

    EXPECT_EQ(server.featureRegistry().featureDefinition("org.silastandard/core/SiLAService/v1"),
              "<Feature/>");
}

TEST(SiLAServerBaseBuilder, RejectsDuplicateFeatureRegistration)
{
    sila2::SiLAServerBase::Builder builder;
    builder.AddFeature("org.silastandard/core/SiLAService/v1", "<Feature/>");

    EXPECT_THROW(builder.AddFeature("org.silastandard/core/SiLAService/v1", "<Feature/>"),
                 std::invalid_argument);
}

TEST(SiLAServerBaseBuilder, GeneratesASelfSignedCertificate)
{
    const auto server = sila2::SiLAServerBase::Builder()
                             .WithSelfSignedCertificate("SiLA2", "127.0.0.1")
                             .Build();

    // The certificate/key content itself is TlsConfig's responsibility and is
    // already covered by test_tls_config.cc; this only checks that the
    // Builder wired generation through to PEM output.
    EXPECT_EQ(server.certificatePem().rfind("-----BEGIN CERTIFICATE-----", 0), 0u);
    EXPECT_EQ(server.privateKeyPem().rfind("-----BEGIN PRIVATE KEY-----", 0), 0u);
}

TEST(SiLAServerBaseBuilder, UsesSuppliedCertificateVerbatim)
{
    const auto server = sila2::SiLAServerBase::Builder()
                             .WithCertificate("fake-cert-pem", "fake-key-pem")
                             .Build();

    EXPECT_EQ(server.certificatePem(), "fake-cert-pem");
    EXPECT_EQ(server.privateKeyPem(), "fake-key-pem");
}

TEST(SiLAServerBaseBuilder, RequiresTlsBeforeBuild)
{
    sila2::SiLAServerBase::Builder builder;

    EXPECT_THROW(builder.Build(), std::logic_error);
}
