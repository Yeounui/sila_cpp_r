// FeatureCatalog.cc — Per-server descriptor cache (architecture.md §4.2)
#include <sila/client/dynamic/FeatureCatalog.h>

#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/client/dynamic/FdlRuntimeParser.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

namespace gpb = google::protobuf;

namespace sila2 {
namespace dynamic {

namespace {

// The FQI a Feature's own contents imply: <Originator>/<Category>/<Identifier>/v<major>.
// Mirrors the two normalisations DescriptorBuilder's packageName() already
// applies to these same four fields (DescriptorBuilder.cc:23-30) -- the
// "none" stand-in for an omitted Category and truncation of FeatureVersion to
// its major part -- so a Feature's FQI and its proto package can never
// disagree about which Feature they name.
// ponytail: the 'none'/major-version rule is spelled twice (here and
// DescriptorBuilder.cc:23-30 packageName()); extract a shared fqiOf() into
// FdlRuntimeParser.h once its live edits settle.
std::string derivedFqi(const Feature& feature) {
    const std::string category = feature.category.empty() ? "none" : feature.category;
    const std::string major = feature.featureVersion.substr(0, feature.featureVersion.find('.'));
    return feature.originator + "/" + category + "/" + feature.identifier + "/v" + major;
}

}  // namespace

FeatureCatalog::FeatureCatalog(const gpb::DescriptorPool& frameworkPool)
    : frameworkPool_{frameworkPool},
      // Overlaying frameworkPool as the underlay lets types defined there
      // (e.g. common data types) resolve without duplicating them here.
      pool_{std::make_unique<gpb::DescriptorPool>(&frameworkPool)},
      factory_{std::make_unique<gpb::DynamicMessageFactory>(pool_.get())} {
}

FeatureCatalog::~FeatureCatalog() = default;

void FeatureCatalog::add(const std::string& fqi, const std::string& fdlXml) {
    auto ir = parseFdl(fdlXml);

    // The caller's FQI is used as a lookup key and (for a server) is what
    // ListImplementedFeatures advertises; the FDL carries its own identity.
    // Reject a disagreement here, before DescriptorBuilder runs, so a
    // mismatched Feature never reaches pool_/entries_/fqis_ and surfaces as a
    // confusing "RPC not found" later instead of failing at registration.
    if (const std::string derived = derivedFqi(ir); derived != fqi) {
        throw std::invalid_argument{
            "FDL identity " + derived + " does not match the registered FQI " + fqi};
    }

    DescriptorBuilder builder;
    auto fileProto = builder.build(ir);

    const auto* file = pool_->BuildFile(fileProto);
    if (file == nullptr) {
        throw std::invalid_argument{"Failed to build FileDescriptorProto for FQI: " + fqi};
    }

    entries_[fqi] = {fileProto.package(), ir.identifier, std::move(ir)};
    fqis_.push_back(fqi);
}

const gpb::MethodDescriptor* FeatureCatalog::findMethod(
    const std::string& fqi, const std::string& rpcName) const {
    const auto& entry = entries_.at(fqi);
    std::string fullServiceName = entry.package + "." + entry.serviceName;
    const auto* service = pool_->FindServiceByName(fullServiceName);
    if (service == nullptr) {
        throw std::invalid_argument{"Service not found: " + fullServiceName};
    }
    const auto* method = service->FindMethodByName(rpcName);
    if (method == nullptr) {
        throw std::invalid_argument{"RPC not found: " + rpcName};
    }
    return method;
}

const gpb::Descriptor* FeatureCatalog::requestDescriptor(
    const std::string& fqi, const std::string& rpcName) const {
    return findMethod(fqi, rpcName)->input_type();
}

const gpb::Descriptor* FeatureCatalog::responseDescriptor(
    const std::string& fqi, const std::string& rpcName) const {
    return findMethod(fqi, rpcName)->output_type();
}

const gpb::Descriptor* FeatureCatalog::messageDescriptor(
    const std::string& fqi, const std::string& messageName) const {
    const auto& entry = entries_.at(fqi);
    const std::string fullName = entry.package + "." + messageName;
    const auto* descriptor = pool_->FindMessageTypeByName(fullName);
    if (descriptor == nullptr) {
        throw std::invalid_argument{"Message not found: " + fullName};
    }
    return descriptor;
}

std::string FeatureCatalog::grpcMethodName(const std::string& fqi, const std::string& rpcId) const {
    const auto& entry = entries_.at(fqi);
    return "/" + entry.package + "." + entry.serviceName + "/" + rpcId;
}

std::optional<std::string> FeatureCatalog::validateRequest(
    const std::string& fqi, const std::string& rpcName,
    const gpb::Message& request, ConstraintResolver constraintResolver) const {
    const auto& entry = entries_.at(fqi);
    const auto* requestDescriptor = findMethod(fqi, rpcName)->input_type();
    if (request.GetDescriptor() != requestDescriptor) {
        return "request message does not match RPC input type";
    }

    const auto command = std::find_if(
        entry.feature.commands.begin(), entry.feature.commands.end(),
        [&](const Command& candidate) { return candidate.identifier == rpcName; });
    if (command == entry.feature.commands.end()) return std::nullopt;

    const DataTypeResolver resolver = [&](std::string_view typeId) -> const DataType* {
        const auto definition = std::find_if(
            entry.feature.dataTypeDefinitions.begin(), entry.feature.dataTypeDefinitions.end(),
            [&](const DataTypeDefinition& candidate) { return candidate.identifier == typeId; });
        return definition == entry.feature.dataTypeDefinitions.end() ? nullptr : &definition->dataType;
    };
    for (const auto& parameter : command->parameters) {
        const auto* field = requestDescriptor->FindFieldByName(parameter.identifier);
        if (field == nullptr) {
            return "request message has no field for parameter: " + parameter.identifier;
        }
        if (const auto error = ValueValidator::validate(
                parameter.dataType, request, *field, resolver, constraintResolver)) {
            return "parameter " + parameter.identifier + ": " + *error;
        }
    }
    return std::nullopt;
}

const std::vector<std::string>& FeatureCatalog::fqis() const {
    return fqis_;
}

gpb::DynamicMessageFactory& FeatureCatalog::messageFactory() {
    return *factory_;
}

}  // namespace dynamic
}  // namespace sila2
