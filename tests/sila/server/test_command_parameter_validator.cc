// Tests for CommandParameterValidator's resolver of value-dependent
// Constraints (Unit, ContentType, Schema, AllowedTypes): S65 fixed the
// resolver from an unconditional rejection to std::nullopt (accept), because
// Part A p67 / Part B require rejecting only on an actual violation. This
// exercises the real ParameterConstraintsTest FDL fixture end to end through
// CommandParameterValidator::validate, and confirms the value-independent
// Pattern constraint is still enforced afterward.
//
// No generated proto message in this repo carries the ParameterConstraintsTest
// request shape, so the request message is built at runtime via
// google::protobuf::DescriptorPool + DynamicMessageFactory, mirroring
// tests/sila/server/binary/test_binary_parameter_interceptor.cc: the pool's
// underlay is the generated pool so ".sila2.org.silastandard.String" resolves
// to the real generated type, and SetDelegateToGeneratedFactory(true) makes
// the factory hand back a genuine sila2::org::silastandard::String instance.
#include <sila/server/CommandParameterValidator.h>

#include <gtest/gtest.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sila/client/dynamic/DescriptorBuilder.h>
#include <sila/common/error/SiLAErrorSubtypes.h>

#include "SiLAFramework.pb.h"

namespace {
using sila2::CommandParameterValidator;
using sila2::ProvisionedSchema;
using sila2::error::ValidationError;
using sila2::dynamic::DescriptorBuilder;
using sila2::org::silastandard::Any;
using sila2::org::silastandard::Binary;
using sila2::org::silastandard::String;

#ifndef SILA2_SOURCE_ROOT
#error "SILA2_SOURCE_ROOT must be supplied by tests/CMakeLists.txt"
#endif

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"unable to open FDL fixture: " + path.string()};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string parameterConstraintsTestFdl() {
    return readFile(std::filesystem::path{SILA2_SOURCE_ROOT} / "third_party" / "sila_base" /
                     "feature_definitions" / "org" / "silastandard" / "test" /
                     "ParameterConstraintsTest-v1_0.sila.xml");
}

std::string anyTypeTestFdl() {
    return readFile(std::filesystem::path{SILA2_SOURCE_ROOT} / "third_party" / "sila_base" /
                     "feature_definitions" / "org" / "silastandard" / "test" /
                     "AnyTypeTest-v1_0.sila.xml");
}

// Builds ConstrainedParameterRequest { sila2.org.silastandard.String
// ConstrainedParameter = 1; } in a pool underlaid by the generated pool, the
// same shape codegen emits for a Command's *_Parameters message (see
// tests/codegen/golden/Constrained.proto).
class DynamicConstrainedRequest {
public:
    DynamicConstrainedRequest()
        : pool_{google::protobuf::DescriptorPool::generated_pool()}, factory_{&pool_} {
        factory_.SetDelegateToGeneratedFactory(true);

        google::protobuf::FileDescriptorProto fileProto;
        fileProto.set_name("test_command_parameter_validator_request.proto");
        fileProto.set_package("sila2.test.commandparametervalidator");
        fileProto.set_syntax("proto3");
        fileProto.add_dependency("SiLAFramework.proto");

        google::protobuf::DescriptorProto* request = fileProto.add_message_type();
        request->set_name("ConstrainedParameterRequest");
        auto* field = request->add_field();
        field->set_name("ConstrainedParameter");
        field->set_number(1);
        field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        field->set_type_name(".sila2.org.silastandard.String");

        const google::protobuf::FileDescriptor* file = pool_.BuildFile(fileProto);
        requestDescriptor_ = file->FindMessageTypeByName("ConstrainedParameterRequest");
    }

    [[nodiscard("caller expects the owned request instance")]]
    std::unique_ptr<google::protobuf::Message> newRequest(const std::string& value) {
        std::unique_ptr<google::protobuf::Message> request{
            factory_.GetPrototype(requestDescriptor_)->New()};
        const google::protobuf::Reflection* reflection = request->GetReflection();
        const google::protobuf::FieldDescriptor* field =
            requestDescriptor_->FindFieldByName("ConstrainedParameter");
        auto* stringMessage = dynamic_cast<String*>(reflection->MutableMessage(request.get(), field));
        stringMessage->set_value(value);
        return request;
    }

private:
    google::protobuf::DescriptorPool pool_;
    google::protobuf::DynamicMessageFactory factory_;
    const google::protobuf::Descriptor* requestDescriptor_ = nullptr;
};

// Builds ContentTypeParameterRequest { sila2.org.silastandard.Binary
// ContentTypeParameter = 1; } -- mirrors DynamicConstrainedRequest above but
// with a Binary field, so newRequest can set raw bytes (including embedded
// NULs and high bytes) via Binary::set_value instead of a String's UTF-8
// text value.
class DynamicBinaryRequest {
public:
    DynamicBinaryRequest()
        : pool_{google::protobuf::DescriptorPool::generated_pool()}, factory_{&pool_} {
        factory_.SetDelegateToGeneratedFactory(true);

        google::protobuf::FileDescriptorProto fileProto;
        fileProto.set_name("test_command_parameter_validator_binary_request.proto");
        fileProto.set_package("sila2.test.commandparametervalidator.binary");
        fileProto.set_syntax("proto3");
        fileProto.add_dependency("SiLAFramework.proto");

        google::protobuf::DescriptorProto* request = fileProto.add_message_type();
        request->set_name("ContentTypeParameterRequest");
        auto* field = request->add_field();
        field->set_name("ContentTypeParameter");
        field->set_number(1);
        field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        field->set_type_name(".sila2.org.silastandard.Binary");

        const google::protobuf::FileDescriptor* file = pool_.BuildFile(fileProto);
        requestDescriptor_ = file->FindMessageTypeByName("ContentTypeParameterRequest");
    }

    // `bytes` becomes the Binary's inline `value` field verbatim -- passed
    // through std::string so embedded NULs and high bytes (the malformed
    // UTF-8 fixtures below) survive rather than being read as a C string.
    [[nodiscard("caller expects the owned request instance")]]
    std::unique_ptr<google::protobuf::Message> newRequest(const std::string& bytes) {
        std::unique_ptr<google::protobuf::Message> request{
            factory_.GetPrototype(requestDescriptor_)->New()};
        const google::protobuf::Reflection* reflection = request->GetReflection();
        const google::protobuf::FieldDescriptor* field =
            requestDescriptor_->FindFieldByName("ContentTypeParameter");
        auto* binaryMessage = dynamic_cast<Binary*>(reflection->MutableMessage(request.get(), field));
        binaryMessage->set_value(bytes);
        return request;
    }

private:
    google::protobuf::DescriptorPool pool_;
    google::protobuf::DynamicMessageFactory factory_;
    const google::protobuf::Descriptor* requestDescriptor_ = nullptr;
};

// Builds a one-field request { .sila2.org.silastandard.Any <fieldName> = 1; }
// in a pool underlaid by the generated pool. `fieldName` defaults to
// "AnyTypeValue", the shape codegen emits for the SetAnyTypeValue Command's
// Parameters message (AnyTypeTest-v1_0.sila.xml) -- the two R10-9b tests
// below use that default untouched. R10-9c's AllowedTypes tests instead pass
// "ConstrainedParameter", the Any parameter name both ParameterConstraintsTest
// and the inline AllowedTypes fixture below declare. Mirrors
// DynamicConstrainedRequest above.
class DynamicAnyRequest {
public:
    explicit DynamicAnyRequest(std::string fieldName = "AnyTypeValue")
        : pool_{google::protobuf::DescriptorPool::generated_pool()},
          factory_{&pool_},
          fieldName_{std::move(fieldName)} {
        factory_.SetDelegateToGeneratedFactory(true);

        google::protobuf::FileDescriptorProto fileProto;
        fileProto.set_name("test_command_parameter_validator_any_request.proto");
        fileProto.set_package("sila2.test.commandparametervalidator.any");
        fileProto.set_syntax("proto3");
        fileProto.add_dependency("SiLAFramework.proto");

        google::protobuf::DescriptorProto* request = fileProto.add_message_type();
        request->set_name("SetAnyTypeValue_Parameters");
        auto* field = request->add_field();
        field->set_name(fieldName_);
        field->set_number(1);
        field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
        field->set_type_name(".sila2.org.silastandard.Any");

        const google::protobuf::FileDescriptor* file = pool_.BuildFile(fileProto);
        requestDescriptor_ = file->FindMessageTypeByName("SetAnyTypeValue_Parameters");
    }

    // `typeXml` and `payloadBytes` become the wire Any's `type`/`payload`
    // fields verbatim -- ValueValidator::validateAnyValue (R10-9b) parses and
    // decodes them, so a malformed typeXml or a payload that does not match
    // it surfaces as a ValidationError rather than an uncaught throw.
    [[nodiscard("caller expects the owned request instance")]]
    std::unique_ptr<google::protobuf::Message> newRequest(const std::string& typeXml,
                                                            const std::string& payloadBytes) {
        std::unique_ptr<google::protobuf::Message> request{
            factory_.GetPrototype(requestDescriptor_)->New()};
        const google::protobuf::Reflection* reflection = request->GetReflection();
        const google::protobuf::FieldDescriptor* field =
            requestDescriptor_->FindFieldByName(fieldName_);
        auto* anyMessage = dynamic_cast<Any*>(reflection->MutableMessage(request.get(), field));
        anyMessage->set_type(typeXml);
        anyMessage->set_payload(payloadBytes);
        return request;
    }

private:
    google::protobuf::DescriptorPool pool_;
    google::protobuf::DynamicMessageFactory factory_;
    std::string fieldName_;
    const google::protobuf::Descriptor* requestDescriptor_ = nullptr;
};

// Builds the "DataType_Payload" message prototype that buildFromTypeXml()
// derives from `typeXml`, registered into `pool` (which must outlive the
// returned prototype and `factory`). Mirrors payloadPrototype() from
// tests/sila/server/test_any_codec_core_link.cc -- a separate TU (internal
// linkage via this anonymous namespace), same encode-side pipeline.
const google::protobuf::Message* anyPayloadPrototype(
    const std::string& typeXml, google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory& factory) {
    DescriptorBuilder builder;
    auto fileProto = builder.buildFromTypeXml(typeXml);
    const auto* file = pool.BuildFile(fileProto);
    if (file == nullptr) return nullptr;
    const auto* desc = pool.FindMessageTypeByName(fileProto.package() + ".DataType_Payload");
    if (desc == nullptr) return nullptr;
    return factory.GetPrototype(desc);
}

// Encodes an Any payload for `typeXml` through the same
// DescriptorBuilder/DynamicMessageFactory pipeline anyPayloadPrototype uses
// (mirrors AcceptsWellFormedIntegerAny's own inline steps): builds the
// DataType_Payload wrapper, hands it plus its Payload field to `fill` so the
// caller sets whatever scalar/nested value the shape needs, then serializes
// -- the bytes a real client would put on the wire. Each call gets a fresh
// pool/factory (DescriptorBuilder names the file by a hash of typeXml, and a
// pool refuses a second BuildFile of the same name).
std::string encodeAnyPayload(
    const std::string& typeXml,
    const std::function<void(google::protobuf::Message&, const google::protobuf::FieldDescriptor&)>&
        fill) {
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const auto* prototype = anyPayloadPrototype(typeXml, encodePool, encodeFactory);
    if (prototype == nullptr) {
        throw std::runtime_error{"unable to build Any payload prototype for type XML: " + typeXml};
    }
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    if (payloadField == nullptr) {
        throw std::runtime_error{"Any payload prototype is missing its Payload field"};
    }
    fill(*wrapper, *payloadField);
    return wrapper->SerializeAsString();
}

const char* kFeatureFqi = "org.silastandard/test/ParameterConstraintsTest/v1";
const char* kAnyFeatureFqi = "org.silastandard/test/AnyTypeTest/v1";

// R10-9c: three shapes the vendored ParameterConstraintsTest FDL cannot
// express (an AllowedTypes list of two scalar Basic types, an AllowedTypes
// entry that is itself Constrained, and a two-element Structure whose element
// order matters). Mirrors the ParameterConstraintsTest envelope (schema
// location, namespaces, per-command Observable/Parameter shape) so it passes
// fdl-validation.xsl (XSD + XSLT) inside CommandParameterValidator's
// constructor -- an invalid fixture would throw there, not in validate().
// Element identifiers "A"/"B" (not "a"/"b") because DataTypes.xsd's
// IdentifierType requires an initial uppercase letter.
std::string allowedTypesInlineFdl() {
    return R"(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>AllowedTypesInlineTest</Identifier>
  <DisplayName>Allowed Types Inline Test</DisplayName>
  <Description>R10-9c test-only shapes for the AllowedTypes constraint on the Any type.</Description>
  <Command>
    <Identifier>CheckIntegerOnly</Identifier>
    <DisplayName>Check Integer Only</DisplayName>
    <Description>An Any parameter whose AllowedTypes list is two scalar Basic types.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>An Any type parameter with an AllowedTypes constraint allowing Integer or String.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Any</Basic></DataType>
          <Constraints>
            <AllowedTypes>
              <DataType><Basic>Integer</Basic></DataType>
              <DataType><Basic>String</Basic></DataType>
            </AllowedTypes>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckBoundedInteger</Identifier>
    <DisplayName>Check Bounded Integer</DisplayName>
    <Description>An Any parameter whose sole AllowedTypes entry is itself Constrained.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>An Any type parameter with an AllowedTypes constraint allowing only an Integer no greater than 100.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Any</Basic></DataType>
          <Constraints>
            <AllowedTypes>
              <DataType>
                <Constrained>
                  <DataType><Basic>Integer</Basic></DataType>
                  <Constraints>
                    <MaximalInclusive>100</MaximalInclusive>
                  </Constraints>
                </Constrained>
              </DataType>
            </AllowedTypes>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckStructureOrder</Identifier>
    <DisplayName>Check Structure Order</DisplayName>
    <Description>An Any parameter whose sole AllowedTypes entry is a two-element Structure.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>An Any type parameter with an AllowedTypes constraint allowing only Structure{A:Integer,B:String}.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Any</Basic></DataType>
          <Constraints>
            <AllowedTypes>
              <DataType>
                <Structure>
                  <Element>
                    <Identifier>A</Identifier>
                    <DisplayName>A</DisplayName>
                    <Description>First element.</Description>
                    <DataType><Basic>Integer</Basic></DataType>
                  </Element>
                  <Element>
                    <Identifier>B</Identifier>
                    <DisplayName>B</DisplayName>
                    <Description>Second element.</Description>
                    <DataType><Basic>String</Basic></DataType>
                  </Element>
                </Structure>
              </DataType>
            </AllowedTypes>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
)";
}

const char* kAllowedTypesInlineFqi = "org.silastandard/test/AllowedTypesInlineTest/v1";

// R10-9e: five Commands, each a Binary parameter with a different ContentType
// -- CheckUppercaseJsonBinary's APPLICATION/JSON proves isTextualContentType
// matches case-insensitively (Part A p70). Mirrors the ParameterConstraintsTest
// envelope so it passes fdl-validation.xsl (XSD + XSLT) inside
// CommandParameterValidator's constructor, same as allowedTypesInlineFdl above.
std::string contentTypeBinaryInlineFdl() {
    return R"(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>ContentTypeBinaryInlineTest</Identifier>
  <DisplayName>Content Type Binary Inline Test</DisplayName>
  <Description>R10-9e test-only Binary parameters carrying a Content Type constraint.</Description>
  <Command>
    <Identifier>CheckJsonBinary</Identifier>
    <DisplayName>Check Json Binary</DisplayName>
    <Description>A Binary parameter with Content Type application/json.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary type parameter with a Content Type constraint of application/json.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <ContentType><Type>application</Type><Subtype>json</Subtype></ContentType>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckTextBinary</Identifier>
    <DisplayName>Check Text Binary</DisplayName>
    <Description>A Binary parameter with Content Type text/plain.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary type parameter with a Content Type constraint of text/plain.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <ContentType><Type>text</Type><Subtype>plain</Subtype></ContentType>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckXmlBinary</Identifier>
    <DisplayName>Check Xml Binary</DisplayName>
    <Description>A Binary parameter with Content Type application/xml.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary type parameter with a Content Type constraint of application/xml.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <ContentType><Type>application</Type><Subtype>xml</Subtype></ContentType>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckImageBinary</Identifier>
    <DisplayName>Check Image Binary</DisplayName>
    <Description>A Binary parameter with Content Type image/png (non-textual).</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary type parameter with a Content Type constraint of image/png.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <ContentType><Type>image</Type><Subtype>png</Subtype></ContentType>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckUppercaseJsonBinary</Identifier>
    <DisplayName>Check Uppercase Json Binary</DisplayName>
    <Description>A Binary parameter with Content Type APPLICATION/JSON, matched case-insensitively.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary type parameter with a Content Type constraint of APPLICATION/JSON.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <ContentType><Type>APPLICATION</Type><Subtype>JSON</Subtype></ContentType>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
)";
}

const char* kContentTypeBinaryFqi = "org.silastandard/test/ContentTypeBinaryInlineTest/v1";

// R10-9f/g2: Commands exercising the Schema constraint's Xml+Inline arm
// (ValueValidator::validateSchema, via FdlRuntimeParser's libxml2) AND its
// Json+Inline arm (via the confined json-schema-validator TU,
// JsonSchemaSupport.cc). CheckStringSchema/CheckNamespacedStringSchema/
// CheckJsonInlineString/CheckJsonLocalRefString/
// CheckJsonAdditionalPropertiesFalseString/CheckJsonRemoteRefString use the
// String parameter name "ConstrainedParameter" (DynamicConstrainedRequest);
// CheckBinarySchema/CheckJsonInlineBinary use the Binary parameter name
// "ContentTypeParameter" (DynamicBinaryRequest) -- both builders already
// exist above (R10-9c/e). The two inline XSDs and every inline JSON Schema
// below are XML-escaped inside <Inline> (a JSON double quote becomes
// &quot;, one consistent choice since Inline is opaque xs:string and quotes
// need no escaping in element text) per Part A p70's note (partA.txt:4595).
// Mirrors contentTypeBinaryInlineFdl's envelope so the FDL passes
// fdl-validation.xsl (XSD + XSLT) inside CommandParameterValidator's
// constructor.
std::string schemaXmlInlineFdl() {
    return R"(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>SchemaXmlInlineTest</Identifier>
  <DisplayName>Schema Xml Inline Test</DisplayName>
  <Description>R10-9f test-only String/Binary parameters carrying an inline Schema constraint.</Description>
  <Command>
    <Identifier>CheckStringSchema</Identifier>
    <DisplayName>Check String Schema</DisplayName>
    <Description>A String parameter with an inline W3C XML Schema constraint (two-level note schema).</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Xml/Inline.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Inline>&lt;xs:schema xmlns:xs=&quot;http://www.w3.org/2001/XMLSchema&quot;&gt;&lt;xs:element name=&quot;note&quot;&gt;&lt;xs:complexType&gt;&lt;xs:sequence&gt;&lt;xs:element name=&quot;header&quot;&gt;&lt;xs:complexType&gt;&lt;xs:sequence&gt;&lt;xs:element name=&quot;to&quot; type=&quot;xs:string&quot;/&gt;&lt;/xs:sequence&gt;&lt;/xs:complexType&gt;&lt;/xs:element&gt;&lt;xs:element name=&quot;body&quot; type=&quot;xs:string&quot;/&gt;&lt;/xs:sequence&gt;&lt;/xs:complexType&gt;&lt;/xs:element&gt;&lt;/xs:schema&gt;</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckBinarySchema</Identifier>
    <DisplayName>Check Binary Schema</DisplayName>
    <Description>A Binary parameter with the same inline W3C XML Schema constraint as CheckStringSchema.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary parameter with a Schema constraint of Xml/Inline.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Inline>&lt;xs:schema xmlns:xs=&quot;http://www.w3.org/2001/XMLSchema&quot;&gt;&lt;xs:element name=&quot;note&quot;&gt;&lt;xs:complexType&gt;&lt;xs:sequence&gt;&lt;xs:element name=&quot;header&quot;&gt;&lt;xs:complexType&gt;&lt;xs:sequence&gt;&lt;xs:element name=&quot;to&quot; type=&quot;xs:string&quot;/&gt;&lt;/xs:sequence&gt;&lt;/xs:complexType&gt;&lt;/xs:element&gt;&lt;xs:element name=&quot;body&quot; type=&quot;xs:string&quot;/&gt;&lt;/xs:sequence&gt;&lt;/xs:complexType&gt;&lt;/xs:element&gt;&lt;/xs:schema&gt;</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckNamespacedStringSchema</Identifier>
    <DisplayName>Check Namespaced String Schema</DisplayName>
    <Description>A String parameter with an inline W3C XML Schema constraint that declares a target namespace.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a namespaced Schema constraint of Xml/Inline.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Inline>&lt;xs:schema xmlns:xs=&quot;http://www.w3.org/2001/XMLSchema&quot; targetNamespace=&quot;urn:sila:test:note&quot; elementFormDefault=&quot;qualified&quot;&gt;&lt;xs:element name=&quot;note&quot; type=&quot;xs:string&quot;/&gt;&lt;/xs:schema&gt;</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonInlineString</Identifier>
    <DisplayName>Check Json Inline String</DisplayName>
    <Description>A String parameter with a Json/Inline Schema constraint requiring an integer id and a string name -- validated (R10-9g2).</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Inline.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Inline>{&quot;type&quot;:&quot;object&quot;,&quot;required&quot;:[&quot;id&quot;,&quot;name&quot;],&quot;properties&quot;:{&quot;id&quot;:{&quot;type&quot;:&quot;integer&quot;},&quot;name&quot;:{&quot;type&quot;:&quot;string&quot;}}}</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonInlineBinary</Identifier>
    <DisplayName>Check Json Inline Binary</DisplayName>
    <Description>A Binary parameter with the same Json/Inline Schema constraint as CheckJsonInlineString.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary parameter with a Schema constraint of Json/Inline.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Inline>{&quot;type&quot;:&quot;object&quot;,&quot;required&quot;:[&quot;id&quot;,&quot;name&quot;],&quot;properties&quot;:{&quot;id&quot;:{&quot;type&quot;:&quot;integer&quot;},&quot;name&quot;:{&quot;type&quot;:&quot;string&quot;}}}</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonLocalRefString</Identifier>
    <DisplayName>Check Json Local Ref String</DisplayName>
    <Description>A String parameter whose Json/Inline Schema resolves a property through a local "#/definitions/..." $ref.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Inline using a local $ref.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Inline>{&quot;definitions&quot;:{&quot;nameType&quot;:{&quot;type&quot;:&quot;string&quot;}},&quot;type&quot;:&quot;object&quot;,&quot;required&quot;:[&quot;name&quot;],&quot;properties&quot;:{&quot;name&quot;:{&quot;$ref&quot;:&quot;#/definitions/nameType&quot;}}}</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonAdditionalPropertiesFalseString</Identifier>
    <DisplayName>Check Json Additional Properties False String</DisplayName>
    <Description>A String parameter whose Json/Inline Schema forbids properties beyond the declared "id".</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Inline using additionalProperties:false.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Inline>{&quot;type&quot;:&quot;object&quot;,&quot;additionalProperties&quot;:false,&quot;properties&quot;:{&quot;id&quot;:{&quot;type&quot;:&quot;integer&quot;}}}</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonRemoteRefString</Identifier>
    <DisplayName>Check Json Remote Ref String</DisplayName>
    <Description>A String parameter whose Json/Inline Schema is entirely a remote $ref -- must fail closed without any network access.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Inline referencing an unroutable remote schema.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Inline>{&quot;$ref&quot;:&quot;http://example.invalid/schema.json&quot;}</Inline>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
)";
}

const char* kSchemaXmlInlineFqi = "org.silastandard/test/SchemaXmlInlineTest/v1";

// R10-9g1: raw (unescaped) form of the same two-level note schema already
// XML-escaped inside schemaXmlInlineFdl's <Inline> above -- this is the text a
// ProvisionedSchema table entry carries for a matching <Url>, since codegen
// embeds the fetched schema verbatim rather than re-escaping it into <Inline>.
const char* kNoteSchemaXml =
    R"xsd(<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema"><xs:element name="note"><xs:complexType><xs:sequence><xs:element name="header"><xs:complexType><xs:sequence><xs:element name="to" type="xs:string"/></xs:sequence></xs:complexType></xs:element><xs:element name="body" type="xs:string"/></xs:sequence></xs:complexType></xs:element></xs:schema>)xsd";

// R10-9g2: a JSON Schema twin of kNoteSchemaXml above -- the same
// note{header{to},body} shape, expressed as a JSON Schema object instead of a
// W3C XML Schema. This is the text a ProvisionedSchema table entry carries
// for a Json/Url Schema: raw JSON, not XML-escaped, since it is never placed
// inside an <Inline> element (codegen embeds the fetched schema verbatim).
const char* kNoteJsonSchema =
    R"json({"type":"object","required":["header","body"],"properties":{"header":{"type":"object","required":["to"],"properties":{"to":{"type":"string"}}},"body":{"type":"string"}}})json";

// R10-9g1: mirrors schemaXmlInlineFdl's <Feature> envelope, but each Schema
// constraint's source is a <Url> instead of an <Inline> -- exercises
// CommandParameterValidator's constructor-time rewriteSchemaUrls against a
// caller-supplied ProvisionedSchema table (Part A p70: Url is an alternative
// SOURCE of the schema, not an exemption from checking it).
std::string schemaXmlUrlFdl() {
    return R"(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>SchemaXmlUrlTest</Identifier>
  <DisplayName>Schema Xml Url Test</DisplayName>
  <Description>R10-9g1 test-only String/Binary parameters carrying a Url-sourced Schema constraint.</Description>
  <Command>
    <Identifier>CheckStringSchemaUrl</Identifier>
    <DisplayName>Check String Schema Url</DisplayName>
    <Description>A String parameter with a Url-sourced W3C XML Schema constraint.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Xml/Url.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Url>https://example.test/note.xsd</Url>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckBinarySchemaUrl</Identifier>
    <DisplayName>Check Binary Schema Url</DisplayName>
    <Description>A Binary parameter with the same Url-sourced W3C XML Schema constraint.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ContentTypeParameter</Identifier>
      <DisplayName>Content Type Parameter</DisplayName>
      <Description>A Binary parameter with a Schema constraint of Xml/Url.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Binary</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Xml</Type>
              <Url>https://example.test/note.xsd</Url>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
  <Command>
    <Identifier>CheckJsonSchemaUrl</Identifier>
    <DisplayName>Check Json Schema Url</DisplayName>
    <Description>A String parameter with a Url-sourced Json Schema constraint, provisioned and validated (R10-9g2).</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>A String parameter with a Schema constraint of Json/Url.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>String</Basic></DataType>
          <Constraints>
            <Schema>
              <Type>Json</Type>
              <Url>https://example.test/note.json</Url>
            </Schema>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
)";
}

const char* kSchemaXmlUrlFqi = "org.silastandard/test/SchemaXmlUrlTest/v1";

// R10-9g2: an Any parameter whose sole AllowedTypes candidate is itself
// Constrained{String, Schema{Xml,Url}} -- exercises rewriteSchemaUrls'
// recursion into AllowedTypes candidates (the g1 sibling gap this batch
// closes) together with the nested resolver in validateAllowedTypesImpl
// routing that candidate's Schema constraint to ValueValidator::validateSchema.
// Mirrors allowedTypesInlineFdl's envelope and CheckBoundedInteger's shape (an
// AllowedTypes entry that is itself Constrained), but with a Schema
// constraint instead of MaximalInclusive.
std::string allowedTypesSchemaUrlFdl() {
    return R"(<?xml version="1.0" encoding="utf-8" ?>
<Feature SiLA2Version="1.0" FeatureVersion="1.0" Originator="org.silastandard" Category="test"
         xmlns="http://www.sila-standard.org"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://www.sila-standard.org https://gitlab.com/SiLA2/sila_base/raw/master/schema/FeatureDefinition.xsd">
  <Identifier>AllowedTypesSchemaUrlTest</Identifier>
  <DisplayName>Allowed Types Schema Url Test</DisplayName>
  <Description>R10-9g2 test-only Any parameter whose AllowedTypes candidate carries a Url-sourced Schema constraint.</Description>
  <Command>
    <Identifier>CheckAllowedTypeSchemaUrl</Identifier>
    <DisplayName>Check Allowed Type Schema Url</DisplayName>
    <Description>An Any parameter whose sole AllowedTypes entry is a String constrained by a Url-sourced W3C XML Schema.</Description>
    <Observable>No</Observable>
    <Parameter>
      <Identifier>ConstrainedParameter</Identifier>
      <DisplayName>Constrained Parameter</DisplayName>
      <Description>An Any type parameter with an AllowedTypes constraint allowing only a Schema-constrained String.</Description>
      <DataType>
        <Constrained>
          <DataType><Basic>Any</Basic></DataType>
          <Constraints>
            <AllowedTypes>
              <DataType>
                <Constrained>
                  <DataType><Basic>String</Basic></DataType>
                  <Constraints>
                    <Schema>
                      <Type>Xml</Type>
                      <Url>https://example.test/note.xsd</Url>
                    </Schema>
                  </Constraints>
                </Constrained>
              </DataType>
            </AllowedTypes>
          </Constraints>
        </Constrained>
      </DataType>
    </Parameter>
  </Command>
</Feature>
)";
}

const char* kAllowedTypesSchemaUrlFqi = "org.silastandard/test/AllowedTypesSchemaUrlTest/v1";

// ---------------------------------------------------------------------------
// Value-dependent constraints (Unit / ContentType / Schema / AllowedTypes) —
// True (positive) path: the resolver accepts instead of rejecting outright.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, AcceptsValueWithDelegatedContentTypeConstraint) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<a/>");

    // Before S65 this threw ValidationError unconditionally, regardless of
    // the value, because the resolver rejected every ContentType constraint.
    EXPECT_NO_THROW(validator.validate("CheckStringConstraintContentType", *request));
}

TEST(CommandParameterValidator, AcceptsValidPatternValue) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("01/02/2003");

    EXPECT_NO_THROW(validator.validate("CheckStringConstraintPattern", *request));
}

// ---------------------------------------------------------------------------
// Value-independent constraint (Pattern) — False (negative) path — CAUGHT:
// proves the S65 resolver change did not weaken the constraints ValueValidator
// checks itself.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, RejectsValueViolatingPatternConstraint) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("not-a-date");

    EXPECT_THROW(validator.validate("CheckStringConstraintPattern", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9b: the Basic-Any branch of ValueValidator no longer rejects every Any
// outright ("Any values are not supported", ValueValidator.cc:1070 pre-fix)
// -- it decodes the wire Any {type,payload} and validates the decoded value
// against its own embedded type (Part B p66). Exercised here through the
// same CommandParameterValidator entry point the gRPC and cloud transports
// share, using the real AnyTypeTest FDL (SetAnyTypeValue/AnyTypeValue).
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, AcceptsWellFormedIntegerAny) {
    CommandParameterValidator validator{anyTypeTestFdl(), kAnyFeatureFqi};

    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>";
    // Encode payload = Integer(7) through the same DescriptorBuilder pipeline
    // AnyCodec::decode expects (test_any_codec_core_link.cc's pattern): build
    // the DataType_Payload wrapper for typeXml, set its scalar 'value' field,
    // serialize -- that is the bytes a real client would put on the wire.
    google::protobuf::DescriptorPool encodePool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory encodeFactory{&encodePool};
    const auto* prototype = anyPayloadPrototype(typeXml, encodePool, encodeFactory);
    ASSERT_NE(prototype, nullptr);
    std::unique_ptr<google::protobuf::Message> wrapper{prototype->New()};
    const auto* payloadField = wrapper->GetDescriptor()->FindFieldByName("Payload");
    ASSERT_NE(payloadField, nullptr);
    auto* inner = wrapper->GetReflection()->MutableMessage(wrapper.get(), payloadField);
    const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
    inner->GetReflection()->SetInt64(inner, valueField, 7);
    const std::string serializedPayload = wrapper->SerializeAsString();

    DynamicAnyRequest requests;
    const auto request = requests.newRequest(typeXml, serializedPayload);

    // Before R10-9b this threw ValidationError unconditionally, regardless of
    // the value, because the validator rejected every Any.
    EXPECT_NO_THROW(validator.validate("SetAnyTypeValue", *request));
}

TEST(CommandParameterValidator, RejectsMalformedAnyOnServerTransport) {
    CommandParameterValidator validator{anyTypeTestFdl(), kAnyFeatureFqi};
    DynamicAnyRequest requests;
    const auto request = requests.newRequest("<not closed", "");

    // parseDataTypeXml throws on malformed XML; validateAnyValue (R10-9b)
    // catches that and returns a diagnostic, which CommandParameterValidator
    // wraps as a ValidationError -- the malformed-Any rejection now reaches
    // both transports (direct gRPC and server-initiated cloud) through the
    // one shared validator.
    EXPECT_THROW(validator.validate("SetAnyTypeValue", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9c: the server resolver now decides AllowedTypes itself (Part A p67
// A224 / p69) instead of returning std::nullopt for it -- it reads the wire
// Any's own type S (ValueValidator::validateAllowedTypes), requires S to
// structurally match at least one allowed entry T (Part A p66: Constrained
// layers stripped first), and requires the decoded value to satisfy that T's
// Constraints. Exercised through the same CommandParameterValidator entry
// point as the R10-9b Any tests above, using both the real
// ParameterConstraintsTest FDL (CheckAllowedTypesConstraint) and the inline
// fixture above for shapes that fixture cannot express.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, AcceptsIntegerAnyInAllowedList) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 42);
        });
    const auto request = requests.newRequest(typeXml, payload);

    // AllowedTypes[Integer, List<Integer>] -- S (Integer) skeleton-matches
    // the first entry and the decoded value has no Constraints to violate.
    EXPECT_NO_THROW(validator.validate("CheckAllowedTypesConstraint", *request));
}

TEST(CommandParameterValidator, AcceptsSenderConstrainedIntegerAny) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    // The sender's own Any type is itself Constrained (Part A p66); stripping
    // that layer for the skeleton match still yields Integer, so this matches
    // the same allowed entry as the plain-Integer case above. The sender's
    // MinimalInclusive 10 is enforced separately, by validateAnyValue's own
    // S-vs-value check after the resolver accepts.
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\">"
        "<Constrained><DataType><Basic>Integer</Basic></DataType>"
        "<Constraints><MinimalInclusive>10</MinimalInclusive></Constraints></Constrained>"
        "</DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 42);
        });
    const auto request = requests.newRequest(typeXml, payload);

    EXPECT_NO_THROW(validator.validate("CheckAllowedTypesConstraint", *request));
}

TEST(CommandParameterValidator, AcceptsIntegerUnderBoundedAllowedType) {
    CommandParameterValidator validator{allowedTypesInlineFdl(), kAllowedTypesInlineFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 42);
        });
    const auto request = requests.newRequest(typeXml, payload);

    // AllowedTypes[Constrained{Integer, MaximalInclusive 100}] -- S (plain
    // Integer) matches the entry's stripped skeleton, and 42 satisfies the
    // entry's own MaximalInclusive 100 applied to the decoded value.
    EXPECT_NO_THROW(validator.validate("CheckBoundedInteger", *request));
}

TEST(CommandParameterValidator, AcceptsOrderedStructureAny) {
    CommandParameterValidator validator{allowedTypesInlineFdl(), kAllowedTypesInlineFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Structure>"
        "<Element><Identifier>A</Identifier><DisplayName>A</DisplayName>"
        "<Description>A</Description><DataType><Basic>Integer</Basic></DataType></Element>"
        "<Element><Identifier>B</Identifier><DisplayName>B</DisplayName>"
        "<Description>B</Description><DataType><Basic>String</Basic></DataType></Element>"
        "</Structure></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* fieldA = inner->GetDescriptor()->FindFieldByName("A");
            auto* aMessage = inner->GetReflection()->MutableMessage(inner, fieldA);
            const auto* aValue = aMessage->GetDescriptor()->FindFieldByName("value");
            aMessage->GetReflection()->SetInt64(aMessage, aValue, 1);
            const auto* fieldB = inner->GetDescriptor()->FindFieldByName("B");
            auto* bMessage = inner->GetReflection()->MutableMessage(inner, fieldB);
            const auto* bValue = bMessage->GetDescriptor()->FindFieldByName("value");
            bMessage->GetReflection()->SetString(bMessage, bValue, "x");
        });
    const auto request = requests.newRequest(typeXml, payload);

    // AllowedTypes[Structure{A:Integer,B:String}] -- S's elements match the
    // allowed entry's identifiers and types in the same order (Part B p70).
    EXPECT_NO_THROW(validator.validate("CheckStructureOrder", *request));
}

// ---------------------------------------------------------------------------
// R10-9c: AllowedTypes — False (negative) path — CAUGHT: a wire Any whose
// type does not structurally match any allowed entry, or whose decoded value
// violates the one entry it does match, or whose type XML is malformed.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, RejectsBooleanAnyNotInAllowedList) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Boolean</Basic></DataType>";
    // The skeleton mismatch is decided before any decode, so an empty payload
    // is fine -- it is never read.
    const auto request = requests.newRequest(typeXml, "");

    // AllowedTypes[Integer, List<Integer>] -- Boolean matches neither entry.
    EXPECT_THROW(validator.validate("CheckAllowedTypesConstraint", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsListAnyWhenOnlyScalarAllowed) {
    CommandParameterValidator validator{allowedTypesInlineFdl(), kAllowedTypesInlineFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><List><DataType><Basic>Integer</Basic>"
        "</DataType></List></DataType>";
    const auto request = requests.newRequest(typeXml, "");

    // AllowedTypes[Integer, String] -- a List matches neither scalar entry.
    EXPECT_THROW(validator.validate("CheckIntegerOnly", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsIntegerViolatingBoundedAllowedType) {
    CommandParameterValidator validator{allowedTypesInlineFdl(), kAllowedTypesInlineFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>Integer</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetInt64(inner, valueField, 500);
        });
    const auto request = requests.newRequest(typeXml, payload);

    // AllowedTypes[Constrained{Integer, MaximalInclusive 100}] -- S matches
    // the entry's stripped skeleton, but 500 violates MaximalInclusive 100.
    EXPECT_THROW(validator.validate("CheckBoundedInteger", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsStructureWithWrongElementOrder) {
    CommandParameterValidator validator{allowedTypesInlineFdl(), kAllowedTypesInlineFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    // Same two elements as AcceptsOrderedStructureAny, reversed: B (String)
    // first, A (Integer) second.
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Structure>"
        "<Element><Identifier>B</Identifier><DisplayName>B</DisplayName>"
        "<Description>B</Description><DataType><Basic>String</Basic></DataType></Element>"
        "<Element><Identifier>A</Identifier><DisplayName>A</DisplayName>"
        "<Description>A</Description><DataType><Basic>Integer</Basic></DataType></Element>"
        "</Structure></DataType>";
    // The element-order mismatch is decided before any decode (skeletonMatch
    // compares identifiers positionally), so an empty payload is fine.
    const auto request = requests.newRequest(typeXml, "");

    // AllowedTypes[Structure{A:Integer,B:String}] -- reversed order does not
    // skeleton-match (Part B p70: protobuf field numbers derive from order).
    EXPECT_THROW(validator.validate("CheckStructureOrder", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsMalformedAnyUnderAllowedTypes) {
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const auto request = requests.newRequest("<not closed", "");

    // readWireAnyType's parseDataTypeXml throws on the malformed type XML;
    // validateAllowedTypesImpl catches that and returns a diagnostic rather
    // than letting the exception escape uncaught, exactly as
    // RejectsMalformedAnyOnServerTransport does for validateAnyValue.
    EXPECT_THROW(validator.validate("CheckAllowedTypesConstraint", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9e: the server resolver now decides ContentType itself for a SiLA
// Binary (Part A p70/p63: a textual Content Type requires UTF-8 bytes)
// instead of returning std::nullopt for it. Non-textual media (image/png)
// accepts any bytes; a textual media type is matched case-insensitively
// (Part A p70) and its bytes checked with types::isValidUtf8.
// AcceptsValueWithDelegatedContentTypeConstraint above (String field) is left
// untouched -- ContentType on a String stays a no-op.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, AcceptsJsonBinaryWithValidUtf8) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    const auto request = requests.newRequest(R"({"k":"v"})");

    EXPECT_NO_THROW(validator.validate("CheckJsonBinary", *request));
}

TEST(CommandParameterValidator, AcceptsTextBinaryWithMultibyteUtf8) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // "안녕" -- two three-byte UTF-8 code points.
    const auto request = requests.newRequest(std::string{"\xEC\x95\x88\xEB\x85\x95", 6});

    EXPECT_NO_THROW(validator.validate("CheckTextBinary", *request));
}

TEST(CommandParameterValidator, AcceptsImageBinaryWithArbitraryBytes) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // Not valid UTF-8, but image/png is non-textual media (Part A p70
    // restricts only character data), so any bytes are accepted.
    const auto request = requests.newRequest(std::string{"\xFF\xFE\x00\x01", 4});

    EXPECT_NO_THROW(validator.validate("CheckImageBinary", *request));
}

TEST(CommandParameterValidator, AcceptsEmptyTextBinary) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    const auto request = requests.newRequest("");

    EXPECT_NO_THROW(validator.validate("CheckTextBinary", *request));
}

TEST(CommandParameterValidator, RejectsJsonBinaryWithOverlongEncoding) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // 0xC0 0x80 -- overlong two-byte encoding of NUL.
    const auto request = requests.newRequest(std::string{"\xC0\x80", 2});

    EXPECT_THROW(validator.validate("CheckJsonBinary", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsTextBinaryWithSurrogate) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // 0xED 0xA0 0x80 -- encodes U+D800, a UTF-16 surrogate half.
    const auto request = requests.newRequest(std::string{"\xED\xA0\x80", 3});

    EXPECT_THROW(validator.validate("CheckTextBinary", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsXmlBinaryWithTruncatedSequence) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // 0xE2 0x82 starts a three-byte sequence but is missing its third byte.
    // application/xml matches the "*/xml" textual predicate.
    const auto request = requests.newRequest(std::string{"\xE2\x82", 2});

    EXPECT_THROW(validator.validate("CheckXmlBinary", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsUppercaseJsonBinaryWithStrayContinuation) {
    CommandParameterValidator validator{contentTypeBinaryInlineFdl(), kContentTypeBinaryFqi};
    DynamicBinaryRequest requests;
    // 0x80 is a stray continuation byte. APPLICATION/JSON must still be
    // recognized as textual -- proves the case-insensitive match (Part A p70).
    const auto request = requests.newRequest(std::string{"\x80", 1});

    EXPECT_THROW(validator.validate("CheckUppercaseJsonBinary", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9f/g2: the server resolver now decides Schema{Xml,Inline} AND
// Schema{Json,Inline} itself (Part A p67 A224 / p70) instead of returning
// std::nullopt for Json -- ValueValidator::validateSchema parses the value as
// XML or JSON per the Schema's Type and validates it against the inline
// schema (self-contained: no xs:import/xs:include for Xml, and the Json path
// installs a refusing $ref loader so no remote/external $ref is ever
// fetched). A Binary value is UTF-8-checked first (Part A p70).
//
// R10-9g1/g2 (below): CommandParameterValidator's constructor accepts an
// optional std::span<const ProvisionedSchema> and, when a non-empty table is
// supplied, rewrites every Xml OR Json Url Schema in the parsed feature to
// Inline against it BEFORE validate ever runs -- so it is checked exactly
// like the respective Inline case above, including a Schema Url nested
// inside an Any's AllowedTypes candidate (AcceptsAnyAllowedTypeCandidate-
// ConformingToUrlSchema / RejectsAnyAllowedTypeCandidateViolatingUrlSchema --
// the g1 sibling gap this batch closes). The tests below with a table prove
// that MUST enforcement (Part A p67 A224). AcceptsUrlSchemaWithoutProvisionedTable
// (renamed from AcceptsUrlSchemaPendingProvisioning) is the regression anchor
// for the documented no-table asymmetry: a caller that constructs from FDL
// text alone (the runtime client path, or this file's own direct-from-text
// fixtures) gets no table, so a Url stays unresolved and validateSchema keeps
// accepting it.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, AcceptsStringConformingToInlineXmlSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<note><header><to>Tove</to></header><body>Hi</body></note>");

    EXPECT_NO_THROW(validator.validate("CheckStringSchema", *request));
}

TEST(CommandParameterValidator, AcceptsBinaryConformingToInlineXmlSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicBinaryRequest requests;
    const auto request =
        requests.newRequest("<note><header><to>Tove</to></header><body>Hi</body></note>");

    EXPECT_NO_THROW(validator.validate("CheckBinarySchema", *request));
}

TEST(CommandParameterValidator, AcceptsNamespacedDocumentUnderInlineXmlSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"(<note xmlns="urn:sila:test:note">hello</note>)");

    // A conforming document that uses a namespace declared in the schema.
    EXPECT_NO_THROW(validator.validate("CheckNamespacedStringSchema", *request));
}

TEST(CommandParameterValidator, AcceptsUrlSchemaWithoutProvisionedTable) {
    // R10-9g1: constructed with NO ProvisionedSchema table (the 2-arg ctor
    // form, defaulted to an empty span) -- the documented asymmetry: a real
    // server always constructs from generated code (a non-empty table when
    // the FDL declares a Url), so only the runtime client path and this kind
    // of direct-from-text fixture ever hit this lenient no-table arm. WITH a
    // table (below), the same Xml/Url Schema is enforced as a MUST.
    CommandParameterValidator validator{parameterConstraintsTestFdl(), kFeatureFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<a/>");

    EXPECT_NO_THROW(validator.validate("CheckStringConstraintSchema", *request));
}

TEST(CommandParameterValidator, AcceptsStringConformingToUrlXmlSchemaWithTable) {
    // R10-9g1: a ProvisionedSchema table resolves the Xml/Url Schema to Inline
    // at construction, so a conforming value is accepted exactly as it would
    // be under Xml/Inline (Part A p70: Url is an alternative SOURCE of the
    // schema, not an exemption from checking it).
    // schemaXmlUrlFdl() also declares CheckJsonSchemaUrl (note.json) in the
    // same Feature; rewriteSchemaUrls resolves every Schema/Url in the
    // Feature at construction, so the table must carry both entries even
    // though this test only exercises the Xml command.
    std::array<ProvisionedSchema, 2> table{{{"https://example.test/note.xsd", kNoteSchemaXml},
                                             {"https://example.test/note.json", kNoteJsonSchema}}};
    CommandParameterValidator validator{schemaXmlUrlFdl(), kSchemaXmlUrlFqi, table};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<note><header><to>Tove</to></header><body>Hi</body></note>");

    EXPECT_NO_THROW(validator.validate("CheckStringSchemaUrl", *request));
}

TEST(CommandParameterValidator, AcceptsBinaryConformingToUrlXmlSchemaWithTable) {
    // Same reason as above: schemaXmlUrlFdl()'s CheckJsonSchemaUrl command
    // requires note.json to be present in the table too.
    std::array<ProvisionedSchema, 2> table{{{"https://example.test/note.xsd", kNoteSchemaXml},
                                             {"https://example.test/note.json", kNoteJsonSchema}}};
    CommandParameterValidator validator{schemaXmlUrlFdl(), kSchemaXmlUrlFqi, table};
    DynamicBinaryRequest requests;
    const auto request =
        requests.newRequest("<note><header><to>Tove</to></header><body>Hi</body></note>");

    EXPECT_NO_THROW(validator.validate("CheckBinarySchemaUrl", *request));
}

TEST(CommandParameterValidator, ValidatesStringAgainstInlineJsonSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"id":1,"name":"x"})");

    // Schema{Json,Inline} is now checked (Part A p70/p67 A224): a value
    // conforming to the required-id/name object schema is accepted.
    EXPECT_NO_THROW(validator.validate("CheckJsonInlineString", *request));
}

TEST(CommandParameterValidator, AcceptsBinaryConformingToInlineJsonSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicBinaryRequest requests;
    const auto request = requests.newRequest(R"({"id":1,"name":"x"})");

    EXPECT_NO_THROW(validator.validate("CheckJsonInlineBinary", *request));
}

TEST(CommandParameterValidator, AcceptsJsonDocumentUsingLocalRef) {
    // A local "#/definitions/..." $ref resolves inside the root schema
    // without touching the refusing $ref loader -- only a remote/external
    // $ref is refused.
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"name":"x"})");

    EXPECT_NO_THROW(validator.validate("CheckJsonLocalRefString", *request));
}

TEST(CommandParameterValidator, AcceptsJsonUnderAdditionalPropertiesFalse) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"id":1})");

    EXPECT_NO_THROW(validator.validate("CheckJsonAdditionalPropertiesFalseString", *request));
}

TEST(CommandParameterValidator, ValidatesStringAgainstUrlJsonSchemaWithTable) {
    // Proves rewriteSchemaUrls now provisions a Json/Url Schema too (the g1
    // Xml-only guard removed in g2) and that validateSchema checks it exactly
    // like the Inline case above.
    std::array<ProvisionedSchema, 2> table{{{"https://example.test/note.xsd", kNoteSchemaXml},
                                             {"https://example.test/note.json", kNoteJsonSchema}}};
    CommandParameterValidator validator{schemaXmlUrlFdl(), kSchemaXmlUrlFqi, table};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"header":{"to":"Tove"},"body":"Hi"})");

    EXPECT_NO_THROW(validator.validate("CheckJsonSchemaUrl", *request));
}

TEST(CommandParameterValidator, AcceptsAnyAllowedTypeCandidateConformingToUrlSchema) {
    // Proves rewriteSchemaUrls' recursion into AllowedTypes candidates (the
    // g1 sibling gap this batch closes): the candidate's Schema{Xml,Url} is
    // provisioned to Inline at construction, and the nested resolver in
    // validateAllowedTypesImpl routes it to ValueValidator::validateSchema.
    std::array<ProvisionedSchema, 1> table{{{"https://example.test/note.xsd", kNoteSchemaXml}}};
    CommandParameterValidator validator{allowedTypesSchemaUrlFdl(), kAllowedTypesSchemaUrlFqi, table};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetString(
                inner, valueField, "<note><header><to>Tove</to></header><body>Hi</body></note>");
        });
    const auto request = requests.newRequest(typeXml, payload);

    // The Any's own type (plain String) skeleton-matches the AllowedTypes
    // entry (Constrained{String, Schema}); the decoded value then passes that
    // entry's own Schema{Xml,Url} constraint, resolved via the table.
    EXPECT_NO_THROW(validator.validate("CheckAllowedTypeSchemaUrl", *request));
}

// ---------------------------------------------------------------------------
// R10-9f: Schema{Xml,Inline} — False (negative) path — CAUGHT.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, RejectsStringViolatingInlineXmlSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<memo/>");

    // Root element "memo" is not declared by the inline note schema.
    EXPECT_THROW(validator.validate("CheckStringSchema", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsNotWellFormedXmlValue) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<note><header><to>Tove</header></note>");

    // Mismatched tags -- the value fails to parse as XML before any schema
    // check runs.
    EXPECT_THROW(validator.validate("CheckStringSchema", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsBinaryWithInvalidUtf8BeforeSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicBinaryRequest requests;
    const auto request = requests.newRequest(std::string{"\xFF\xFE", 2});

    // Rejected by the UTF-8 pre-check (Part A p70) before any schema parse.
    EXPECT_THROW(validator.validate("CheckBinarySchema", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsDocumentWithNestedSchemaViolation) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<note><header><cc>x</cc></header><body>Hi</body></note>");

    // "cc" inside "header" is unexpected -- proves the libxml2 structured
    // diagnostics path reaches a nested node, not just the document root.
    EXPECT_THROW(validator.validate("CheckStringSchema", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsXmlValueWithExternalEntity) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(
        R"(<?xml version="1.0"?><!DOCTYPE note [<!ENTITY xxe SYSTEM "file:///etc/hostname">]><note>&xxe;</note>)");

    // XML_PARSE_NO_XXE (secureXmlParseOptions) never loads the external
    // entity, so the file is never fetched; the document is rejected in every
    // safe outcome -- a parse error (entity not loaded), or (if silently
    // dropped) an element-only "note" with no header/body, or (were it ever
    // loaded) text content under an element-only complexType. The value is
    // rejected regardless, and no fetch occurs.
    EXPECT_THROW(validator.validate("CheckStringSchema", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9g2: Schema{Json,Inline} — False (negative) path — CAUGHT.
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, RejectsStringMissingRequiredJsonProperty) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"id":1})");

    // "name" is required by the inline schema but absent from the value.
    EXPECT_THROW(validator.validate("CheckJsonInlineString", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsStringWithWrongJsonType) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"id":"notint","name":"x"})");

    // "id" must be an integer per the inline schema; the value carries a string.
    EXPECT_THROW(validator.validate("CheckJsonInlineString", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsNonJsonValueUnderJsonSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("not-json-at-all");

    // json::parse fails on the value itself, before any schema check runs.
    EXPECT_THROW(validator.validate("CheckJsonInlineString", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsBinaryWithInvalidUtf8BeforeJsonSchema) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicBinaryRequest requests;
    const auto request = requests.newRequest(std::string{"\xFF\xFE", 2});

    // Rejected by the shared UTF-8 pre-check (Part A p70) before any JSON parse.
    EXPECT_THROW(validator.validate("CheckJsonInlineBinary", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsJsonSchemaWithRemoteRefFailsClosed) {
    CommandParameterValidator validator{schemaXmlInlineFdl(), kSchemaXmlInlineFqi};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest(R"({"anything":1})");

    // The inline schema's own root is {"$ref":"http://example.invalid/..."} --
    // the refusing schema_loader throws before any network/filesystem IO is
    // attempted, so the unroutable host is never contacted; set_root_schema
    // fails closed and validateSchema turns that into a diagnostic.
    EXPECT_THROW(validator.validate("CheckJsonRemoteRefString", *request), ValidationError);
}

// ---------------------------------------------------------------------------
// R10-9g1/g2: Schema{Xml,Url} WITH a ProvisionedSchema table — False
// (negative) path — CAUGHT (a value violation, including one carried inside
// an Any's AllowedTypes candidate) and construction-time fail-closed (an
// unresolvable Url).
// ---------------------------------------------------------------------------

TEST(CommandParameterValidator, RejectsStringViolatingUrlXmlSchemaWithTable) {
    // Same reason as the Accepts twins above: schemaXmlUrlFdl()'s
    // CheckJsonSchemaUrl command requires note.json in the table too.
    std::array<ProvisionedSchema, 2> table{{{"https://example.test/note.xsd", kNoteSchemaXml},
                                             {"https://example.test/note.json", kNoteJsonSchema}}};
    CommandParameterValidator validator{schemaXmlUrlFdl(), kSchemaXmlUrlFqi, table};
    DynamicConstrainedRequest requests;
    const auto request = requests.newRequest("<memo/>");

    // Root element "memo" is not declared by the provisioned note schema --
    // proves the Url was actually resolved and checked, not silently accepted.
    EXPECT_THROW(validator.validate("CheckStringSchemaUrl", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsAnyAllowedTypeCandidateViolatingUrlSchema) {
    std::array<ProvisionedSchema, 1> table{{{"https://example.test/note.xsd", kNoteSchemaXml}}};
    CommandParameterValidator validator{allowedTypesSchemaUrlFdl(), kAllowedTypesSchemaUrlFqi, table};
    DynamicAnyRequest requests{"ConstrainedParameter"};
    const std::string typeXml =
        "<DataType xmlns=\"http://www.sila-standard.org\"><Basic>String</Basic></DataType>";
    const std::string payload = encodeAnyPayload(
        typeXml, [](google::protobuf::Message& wrapper, const google::protobuf::FieldDescriptor& payloadField) {
            auto* inner = wrapper.GetReflection()->MutableMessage(&wrapper, &payloadField);
            const auto* valueField = inner->GetDescriptor()->FindFieldByName("value");
            inner->GetReflection()->SetString(inner, valueField, "<memo/>");
        });
    const auto request = requests.newRequest(typeXml, payload);

    // Root element "memo" is not declared by the AllowedTypes candidate's
    // provisioned note schema -- proves the Url was actually resolved and
    // checked for a Schema nested inside AllowedTypes, not silently accepted.
    EXPECT_THROW(validator.validate("CheckAllowedTypeSchemaUrl", *request), ValidationError);
}

TEST(CommandParameterValidator, RejectsUrlSchemaAbsentFromProvisionedTable) {
    // The table provisions a DIFFERENT url than the one schemaXmlUrlFdl's
    // commands reference -- a codegen defect (the fetched-and-embedded table
    // should hold every Url the Feature declares). rewriteSchemaUrls fails
    // closed with std::logic_error at CONSTRUCTION, mirroring the existing
    // "generated Command is absent from its FDL" check, rather than letting
    // an unresolvable Url reach validate() and silently pass.
    std::array<ProvisionedSchema, 1> table{{{"https://example.test/other.xsd", kNoteSchemaXml}}};

    EXPECT_THROW((CommandParameterValidator{schemaXmlUrlFdl(), kSchemaXmlUrlFqi, table}),
                 std::logic_error);
}

}  // namespace
