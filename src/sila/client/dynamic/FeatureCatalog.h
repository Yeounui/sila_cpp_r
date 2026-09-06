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

class FeatureCatalog {
public:
    explicit FeatureCatalog(const google::protobuf::DescriptorPool& frameworkPool);
    ~FeatureCatalog();

    void add(const std::string& fqi, const std::string& fdlXml);

    [[nodiscard("caller expects the request descriptor")]]
    const google::protobuf::Descriptor* requestDescriptor(
        const std::string& fqi, const std::string& rpcName) const;

    [[nodiscard("caller expects the response descriptor")]]
    const google::protobuf::Descriptor* responseDescriptor(
        const std::string& fqi, const std::string& rpcName) const;

    // A message that is not an RPC input or output -- Metadata_<Identifier>,
    // whose serialized form is what a client attaches to a call.
    [[nodiscard("caller expects the message descriptor")]]
    const google::protobuf::Descriptor* messageDescriptor(
        const std::string& fqi, const std::string& messageName) const;

    [[nodiscard("caller expects the gRPC method path")]]
    std::string grpcMethodName(const std::string& fqi, const std::string& rpcId) const;

    [[nodiscard("caller must inspect the validation result")]]
    std::optional<std::string> validateRequest(
        const std::string& fqi, const std::string& rpcName,
        const google::protobuf::Message& request,
        ConstraintResolver constraintResolver = {}) const;

    [[nodiscard("caller expects the list of registered FQIs")]]
    const std::vector<std::string>& fqis() const;

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
