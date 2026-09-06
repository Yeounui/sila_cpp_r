// Internal shared libxml2 support for embedded SiLA FDL schemas.
// Included by the runtime parser and the Any type-fragment adapter; this is
// deliberately not a public validator API.

constexpr char kSilaNamespace[] = "http://www.sila-standard.org";

struct EmbeddedSchemaResource {
    const char* name;
    const char* content;
    std::size_t size;
};

using EmbeddedXmlDocument = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

bool isSchemaInclude(xmlNodePtr node) {
    return node != nullptr && node->type == XML_ELEMENT_NODE && node->ns != nullptr &&
           xmlStrEqual(node->ns->href, BAD_CAST "http://www.w3.org/2001/XMLSchema") &&
           xmlStrEqual(node->name, BAD_CAST "include");
}

std::string compositeSchema(std::string_view rootXml, std::string_view firstIncludedXml,
                            std::string_view secondIncludedXml = {}) {
    auto parse = [](std::string_view xml) -> EmbeddedXmlDocument {
        if (xml.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return {nullptr, &xmlFreeDoc};
        }
        return {xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                              XML_PARSE_NONET),
                &xmlFreeDoc};
    };

    auto rootDocument = parse(rootXml);
    auto firstIncludedDocument = parse(firstIncludedXml);
    auto secondIncludedDocument = secondIncludedXml.empty()
                                      ? EmbeddedXmlDocument{nullptr, &xmlFreeDoc}
                                      : parse(secondIncludedXml);
    auto* root = rootDocument ? xmlDocGetRootElement(rootDocument.get()) : nullptr;
    if (root == nullptr || firstIncludedDocument == nullptr ||
        (!secondIncludedXml.empty() && secondIncludedDocument == nullptr)) {
        return {};
    }

    for (auto* child = root->children; child != nullptr;) {
        auto* next = child->next;
        if (isSchemaInclude(child)) {
            xmlUnlinkNode(child);
            xmlFreeNode(child);
        }
        child = next;
    }

    auto appendTopLevelElements = [&](xmlDocPtr sourceDocument) {
        auto* sourceRoot = xmlDocGetRootElement(sourceDocument);
        for (auto* child = sourceRoot == nullptr ? nullptr : sourceRoot->children; child != nullptr;
             child = child->next) {
            if (child->type != XML_ELEMENT_NODE || isSchemaInclude(child)) continue;
            auto* copy = xmlDocCopyNode(child, rootDocument.get(), 1);
            if (copy == nullptr) return false;
            xmlAddChild(root, copy);
        }
        return true;
    };

    if (!appendTopLevelElements(firstIncludedDocument.get()) ||
        (!secondIncludedXml.empty() && !appendTopLevelElements(secondIncludedDocument.get()))) {
        return {};
    }

    xmlChar* serialized = nullptr;
    int serializedSize = 0;
    xmlDocDumpMemoryEnc(rootDocument.get(), &serialized, &serializedSize, "UTF-8");
    if (serialized == nullptr || serializedSize <= 0) {
        if (serialized != nullptr) xmlFree(serialized);
        return {};
    }
    std::string result{reinterpret_cast<const char*>(serialized),
                       static_cast<std::size_t>(serializedSize)};
    xmlFree(serialized);
    return result;
}

const EmbeddedSchemaResource* embeddedSchemaForUri(std::string_view uri) {
    static const std::string featureDefinitionComposite = compositeSchema(
        {sila2::generated::kFeatureDefinitionXsd, sizeof(sila2::generated::kFeatureDefinitionXsd) - 1},
        {sila2::generated::kDataTypesXsd, sizeof(sila2::generated::kDataTypesXsd) - 1},
        {sila2::generated::kConstraintsXsd, sizeof(sila2::generated::kConstraintsXsd) - 1});
    static const std::string dataTypesComposite = compositeSchema(
        {sila2::generated::kDataTypesXsd, sizeof(sila2::generated::kDataTypesXsd) - 1},
        {sila2::generated::kConstraintsXsd, sizeof(sila2::generated::kConstraintsXsd) - 1});
    static const std::string constraintsComposite = compositeSchema(
        {sila2::generated::kConstraintsXsd, sizeof(sila2::generated::kConstraintsXsd) - 1},
        {sila2::generated::kDataTypesXsd, sizeof(sila2::generated::kDataTypesXsd) - 1});
    static const std::string anyTypeDataTypeComposite = compositeSchema(
        {sila2::generated::kAnyTypeDataTypeXsd, sizeof(sila2::generated::kAnyTypeDataTypeXsd) - 1},
        {sila2::generated::kDataTypesXsd, sizeof(sila2::generated::kDataTypesXsd) - 1},
        {sila2::generated::kConstraintsXsd, sizeof(sila2::generated::kConstraintsXsd) - 1});

    static const EmbeddedSchemaResource featureDefinition{
        "FeatureDefinition.xsd",
        featureDefinitionComposite.empty() ? sila2::generated::kFeatureDefinitionXsd
                                             : featureDefinitionComposite.c_str(),
        featureDefinitionComposite.empty() ? sizeof(sila2::generated::kFeatureDefinitionXsd) - 1
                                            : featureDefinitionComposite.size()};
    static const EmbeddedSchemaResource dataTypes{
        "DataTypes.xsd", dataTypesComposite.empty() ? sila2::generated::kDataTypesXsd
                                                      : dataTypesComposite.c_str(),
        dataTypesComposite.empty() ? sizeof(sila2::generated::kDataTypesXsd) - 1
                                    : dataTypesComposite.size()};
    static const EmbeddedSchemaResource constraints{
        "Constraints.xsd", constraintsComposite.empty() ? sila2::generated::kConstraintsXsd
                                                          : constraintsComposite.c_str(),
        constraintsComposite.empty() ? sizeof(sila2::generated::kConstraintsXsd) - 1
                                      : constraintsComposite.size()};
    static const EmbeddedSchemaResource anyTypeDataType{
        "AnyTypeDataType.xsd",
        anyTypeDataTypeComposite.empty() ? sila2::generated::kAnyTypeDataTypeXsd
                                          : anyTypeDataTypeComposite.c_str(),
        anyTypeDataTypeComposite.empty() ? sizeof(sila2::generated::kAnyTypeDataTypeXsd) - 1
                                         : anyTypeDataTypeComposite.size()};

    // Do not normalize to a basename here. The schemaLocations in the pinned
    // documents are these exact relative resource names; every other URI is
    // refused instead of falling through to libxml2's filesystem/network IO.
    if (uri == featureDefinition.name) return &featureDefinition;
    if (uri == dataTypes.name) return &dataTypes;
    if (uri == constraints.name) return &constraints;
    if (uri == anyTypeDataType.name) return &anyTypeDataType;
    return nullptr;
}

[[maybe_unused]] xmlParserErrors loadEmbeddedSchemaResource(void*, const char* url, const char*,
                                                            xmlResourceType, xmlParserInputFlags,
                                                            xmlParserInput** output) {
    if (output == nullptr) return XML_ERR_INTERNAL_ERROR;
    *output = nullptr;

    const auto* resource = embeddedSchemaForUri(url == nullptr ? std::string_view{} : url);
    if (resource == nullptr) return XML_IO_ENOENT;

    *output = xmlNewInputFromMemory(resource->name, resource->content, resource->size,
                                    XML_INPUT_BUF_STATIC);
    return *output == nullptr ? XML_ERR_NO_MEMORY : XML_ERR_OK;
}

int secureXmlParseOptions() {
    int options = XML_PARSE_NONET;
#if defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300
    options |= XML_PARSE_NO_XXE;
#endif
    return options;
}
