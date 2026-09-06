#ifndef SILA_ENABLE_OTEL
#error "SILA_ENABLE_OTEL must reach generated adapter consumers"
#endif

#include "ShakeControllerServiceAdapter.h"
#include "CloudRouterTestHarness.h"

#include <opentelemetry/exporters/memory/in_memory_span_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace gen_shake = sila2::generated::shakecontroller;
namespace cloud = sila2::org::silastandard;

TEST(OtelSpanSmoke, SpanEmittedFromCodegenAdapterDispatch) {
    auto exporter = std::make_unique<opentelemetry::exporter::memory::InMemorySpanExporter>();
    std::shared_ptr<opentelemetry::exporter::memory::InMemorySpanData> data = exporter->GetData();
    auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
    std::shared_ptr<opentelemetry::trace::TracerProvider> provider =
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor));
    opentelemetry::trace::Provider::SetTracerProvider(provider);

    gen_shake::ShakeControllerServiceAdapter adapter;
    adapter.onStopShaking = [](const auto&, sila2::CallContext&, auto& sink) {
        gen_shake::proto::StopShaking_Responses resp;
        sink.send(resp);
        sink.finish();
    };

    grpc::ServerContext ctx;
    gen_shake::proto::StopShaking_Parameters req;
    gen_shake::proto::StopShaking_Responses resp;
    ASSERT_TRUE(adapter.StopShaking(&ctx, &req, &resp).ok());

    const auto spans = data->GetSpans();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(std::string{spans[0]->GetName()},
              std::string{gen_shake::ShakeControllerServiceAdapter::kStopShakingFqi});
}

class OtelCloudSpanSmoke : public cloud_test::CloudRouterFixture {};

TEST_F(OtelCloudSpanSmoke, SpanIncludesCloudMessageCaseAndFqi) {
    auto exporter = std::make_unique<opentelemetry::exporter::memory::InMemorySpanExporter>();
    std::shared_ptr<opentelemetry::exporter::memory::InMemorySpanData> data = exporter->GetData();
    auto processor = opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
    std::shared_ptr<opentelemetry::trace::TracerProvider> provider =
        opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor));
    opentelemetry::trace::Provider::SetTracerProvider(provider);

    sila2::FeatureRegistry registry;
    sila2::CloudEnvelopeRouter router{registry};
    constexpr char kFqi[] = "org.test/OtelFeature/Command/v1";
    router.registerCommandHandler(kFqi,
        [](const std::string&, sila2::CallContext&, sila2::StreamWriteSerializer&,
           const std::string&) {
            auto tracer = opentelemetry::trace::Provider::GetTracerProvider()
                              ->GetTracer("sila2");
            tracer->StartSpan("cloud.route.handler")->End();
        });
    cloud::SiLAClientMessage msg;
    msg.set_requestuuid("otel-cloud-route");
    msg.mutable_unobservablecommandexecution()->set_fullyqualifiedcommandid(kFqi);
    router.route(msg, *writer_, writer_, calls_);

    const auto spans = data->GetSpans();
    ASSERT_EQ(spans.size(), 2u);
    const auto route = std::find_if(spans.begin(), spans.end(), [](const auto& span) {
        return span->GetName() == "cloud.route";
    });
    const auto child = std::find_if(spans.begin(), spans.end(), [](const auto& span) {
        return span->GetName() == "cloud.route.handler";
    });
    ASSERT_NE(route, spans.end());
    ASSERT_NE(child, spans.end());
    EXPECT_EQ(child->get()->GetParentSpanId(), route->get()->GetSpanId());
    const auto& attributes = route->get()->GetAttributes();
    EXPECT_EQ(opentelemetry::nostd::get<int32_t>(attributes.at("sila.cloud.message_case")),
              static_cast<int32_t>(cloud::SiLAClientMessage::kUnobservableCommandExecution));
    EXPECT_EQ(opentelemetry::nostd::get<std::string>(attributes.at("sila.fqi")), kFqi);
}
