#pragma once
#include <functional>
#include <memory>
#include <string>

class QCoreApplication;
class Authenticator;
class CoinbaseRestClient;
class MarketDataCoreEngine;
struct ProductMetadataResult;
struct ServerMdcConfig;

namespace sentinel::capture {
// Offline integration tests replace external services while running the same
// command parsing, ingest observer, signal handling, queue and shutdown path.
// Empty hooks use the production REST client and engine; no test CLI switches.
struct ApplicationDependencies {
    std::function<ProductMetadataResult(CoinbaseRestClient&, const std::string&)> fetchMetadata;
    std::function<std::unique_ptr<MarketDataCoreEngine>(Authenticator&, const ServerMdcConfig&)> makeEngine;
};

int runApplication(QCoreApplication& application);
int runApplication(QCoreApplication& application, const ApplicationDependencies& dependencies);
} // namespace sentinel::capture
