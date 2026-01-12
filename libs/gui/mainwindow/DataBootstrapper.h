/*
Sentinel — DataBootstrapper
Role: Loads configuration and initializes data components (Authenticator, DataCache, MarketDataCore).
Inputs/Outputs: Loads QSettings, creates data components, starts MarketDataCore.
Threading: Runs on main GUI thread (but MarketDataCore spawns worker threads).
Performance: Setup-only, not on hot path.
Integration: Called from MainWindowGPU constructor.
Observability: Logs component initialization via sLog_App.
Related: MainWindowGpu.cpp, MarketDataCore.hpp, Authenticator.hpp, DataCache.hpp.
*/
#pragma once

#include <memory>
#include <QString>

// Forward declarations
class Authenticator;
class DataCache;
class MarketDataCore;

struct DataComponents {
    std::unique_ptr<Authenticator> authenticator;
    std::unique_ptr<DataCache> dataCache;
    std::unique_ptr<MarketDataCore> marketDataCore;
};

class DataBootstrapper {
public:
    static DataComponents initialize();
    
private:
    static QString getKeyFileFromConfig();
};

