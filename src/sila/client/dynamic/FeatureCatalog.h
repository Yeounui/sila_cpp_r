// FeatureCatalog.h — Per-server descriptor cache (architecture.md §4.2)
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sila/client/dynamic/ValueValidator.h>

namespace google { namespace protobuf {
class Descriptor;
class DescriptorPool;
class DynamicMessageFactory;
class FileDescriptorProto;
class Message;
class MethodDescriptor;
}}

namespace sila2 {
namespace dynamic {

/// Builds and caches protobuf descriptors for the @ref gl_feature "Features" a
/// @ref gl_sila_client "SiLA Client" discovers at runtime, so it can call a
/// server without generated stubs.
///
/// Constructed by the caller once per server connection; each call to add()
/// parses one @ref gl_feature_definition "Feature Definition" (as returned by
/// SiLAService's GetFeatureDefinition, for example) and registers it under its
/// @ref gl_fully_qualified_identifier "Fully Qualified Identifier". The
/// resulting descriptors drive both message construction (messageFactory())
/// and dynamic gRPC calls (sila2::dynamic::callUnary).
///
/// @code{.cpp}
/// FeatureCatalog catalog{*google::protobuf::DescriptorPool::generated_pool()};
/// catalog.add(fqi, fdlXml);  // fdlXml as returned by GetFeatureDefinition
///
/// const auto* descriptor = catalog.requestDescriptor(fqi, "StartShaking");
/// auto request = std::unique_ptr<google::protobuf::Message>{
///     catalog.messageFactory().GetPrototype(descriptor)->New()};
/// // ... populate request's fields via reflection ...
///
/// grpc::ByteBuffer response;
/// callUnary(channel, catalog, fqi, "StartShaking", *request, &response);
/// @endcode
class FeatureCatalog {
public:
    explicit FeatureCatalog(const google::protobuf::DescriptorPool& frameworkPool);
    ~FeatureCatalog();

    /// Parses fdlXml and registers it under fqi so later lookups on this
    /// catalog can find its @ref gl_command "Commands", @ref gl_property "Properties", and types.
    /// @throws std::invalid_argument if fdlXml fails FDL parsing/validation, or
    /// if the Feature identity it declares (Originator/Category/Identifier/
    /// FeatureVersion) does not match fqi.
    void add(const std::string& fqi, const std::string& fdlXml);

    /// The parameter message descriptor for a Command or the read type
    /// descriptor for a Property's getter RPC.
    [[nodiscard("caller expects the request descriptor")]]
    const google::protobuf::Descriptor* requestDescriptor(
        const std::string& fqi, const std::string& rpcName) const;

    /// The response message descriptor for a Command or an
    /// @ref gl_observable_property "Observable Property" subscription's streamed
    /// message.
    [[nodiscard("caller expects the response descriptor")]]
    const google::protobuf::Descriptor* responseDescriptor(
        const std::string& fqi, const std::string& rpcName) const;

    // A message that is not an RPC input or output -- Metadata_<Identifier>,
    // whose serialized form is what a client attaches to a call.
    /// The descriptor for a message that is not an RPC input or output, such
    /// as a @ref gl_sila_client_metadata "SiLA Client Metadata" wrapper.
    [[nodiscard("caller expects the message descriptor")]]
    const google::protobuf::Descriptor* messageDescriptor(
        const std::string& fqi, const std::string& messageName) const;

    /// The gRPC method path ("/<package>.<Service>/<rpcId>") for one RPC of a
    /// registered Feature, as needed by sila2::dynamic::callUnary() and
    /// callServerStream().
    [[nodiscard("caller expects the gRPC method path")]]
    std::string grpcMethodName(const std::string& fqi, const std::string& rpcId) const;

    /// Checks request against the @ref gl_constraint "Constraints" the
    /// Feature Definition declares for this RPC's parameters.
    /// @return std::nullopt when request is valid, otherwise a diagnostic
    /// describing the first violated constraint.
    [[nodiscard("caller must inspect the validation result")]]
    std::optional<std::string> validateRequest(
        const std::string& fqi, const std::string& rpcName,
        const google::protobuf::Message& request,
        ConstraintResolver constraintResolver = {}) const;

    /// The @ref gl_fully_qualified_identifier "FQIs" registered so far, in
    /// add() order.
    [[nodiscard("caller expects the list of registered FQIs")]]
    const std::vector<std::string>& fqis() const;

    /// The factory that instantiates messages from this catalog's descriptors
    /// (see the usage example above).
    [[nodiscard("caller expects the message factory")]]
    google::protobuf::DynamicMessageFactory& messageFactory();

private:
    const google::protobuf::MethodDescriptor* findMethod(
        const std::string& fqi, const std::string& rpcName) const;

    struct FeatureEntry {
        std::string package;
        std::string serviceName;
        Feature feature;
    };

    const google::protobuf::DescriptorPool& frameworkPool_;
    std::unique_ptr<google::protobuf::DescriptorPool> pool_;
    std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
    std::vector<std::string> fqis_;
    std::map<std::string, FeatureEntry> entries_;
};

}  // namespace dynamic
}  // namespace sila2
