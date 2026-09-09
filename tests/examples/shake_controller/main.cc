// main.cc — ShakeController example server entry point.
//
// This is teaching material: it shows how a Feature owner assembles a
// SiLA2 server out of SilaServerBase::Builder plus a single hand-written
// Feature implementation (ShakeControllerImpl). See ShakeControllerImpl.h
// for how the generated ServiceAdapter and the domain logic are wired
// together; this file only covers the Builder side of that assembly.
//
// build: cmake -B build -DSILA2_BUILD_EXAMPLES=ON && cmake --build build
// Run:   ./build/examples/shake_controller_example [--port 50052]
#include "ShakeControllerImpl.h"
#include "ShakeControllerMeta.h"

#include <sila/server/config/ServerConfig.h>
#include <sila/server/SilaServerBase.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

// Scans argv for "--port <value>", falling back to the SiLA2 example
// default port if not given.
std::uint16_t parsePort(int argc, char* argv[]) {
    constexpr std::uint16_t kDefaultPort = 50052;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string{argv[i]} == "--port") {
            return static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        }
    }
    return kDefaultPort;
}

}  // namespace

int main(int argc, char* argv[]) {
    namespace gen = sila2::generated::shakecontroller;

    const std::uint16_t port = parsePort(argc, argv);

    // Step 1: Create a Builder and configure server identity + TLS.
    sila2::SilaServerBase::Builder builder;
    builder.withSelfSignedCertificate("localhost", "127.0.0.1")
           .withConfig(std::make_unique<sila2::InMemoryServerConfig>(
               "ShakeControllerExample"));

    // Step 2: Create the Feature implementation, passing the Builder's
    // interceptor chain so the generated ServiceAdapter can participate in
    // auth / binary-transfer / metadata extraction.
    shake_example::ShakeControllerImpl shakeController(builder.chain());

    // Step 3: Register the Feature (FQI + FDL XML + gRPC service) and the
    // ObservableCommandManager so the server can cancel in-flight commands
    // on shutdown.
    auto server = builder
        .addFeature(std::string{gen::kFqi},
                    std::string{gen::kFdlXml},
                    shakeController.service())
        .registerCommandManager(&shakeController.commandManager())
        .withDiscovery(port)
        .build();

    std::cout << "ShakeController example server listening on port " << port << "\n";
    server.run(true);
    return 0;
}
