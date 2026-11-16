#pragma once
#include "AgentApiTypes.hpp"
#include <QJsonObject>
struct ServerConfig;

namespace AgentApi {
class RequestParser {
public:
    ParseResult feed(const QByteArray& bytes);
private:
    QByteArray m_buffer;
    bool m_finished = false;
};

// Widget screenshot targets include retained dock ids, settings[:Tab], toolbar and chartmenu.
bool isWidgetScreenshotTarget(const QString& target);
ValidationResult validateQuery(const Request& request, const QString& activeSymbol = {});
struct ControlBody {
    QString symbol;
    qint64 timeframeMs = 0;
    std::optional<qint64> startMs, endMs;
    std::optional<double> priceMin, priceMax;
    std::optional<bool> followLive;
    std::optional<bool> autoScale; // viewport: the auto price scale toggle
    std::optional<bool> lockMid; // viewport: lock to mid (exclusive with autoScale:true and price bounds)
    QString fit; // viewport: "time" | "price" | "both" | "default", exclusive with bounds/followLive/autoScale
    QJsonObject layers;
    QJsonObject dockVisible;
    QString dockFocus;
    bool persistDocks = false;
    QJsonObject heatmapSettings;
    bool persistHeatmapSettings = true;
    InputCommand input;
};
struct ControlValidation {
    int status = 200;
    QString code, message;
    ControlBody body;
};
ControlValidation validateControl(const Request& request, const std::optional<QList<qint64>>& servedTimeframes,
                                  const QJsonObject& currentDocks = {});
void applyAdvertisedServerConfig(StateSnapshot& snapshot, const ServerConfig& config);
// POST /api/v1/layers keys that are heatmap chart settings, or TPO look strings.
enum class LayerKeyKind { Setting, TpoLook };
bool isLayerKey(const QString& key, LayerKeyKind kind);
// GET /api/v1/schema data: {heatmapSettings: {key: entry}, layers: {key: entry}}, generated from
// the tables that validate POST /api/v1/heatmap/settings and POST /api/v1/layers.
QJsonObject schemaJson();

QJsonObject envelope(const Metadata& meta, const QJsonObject& data);
QJsonObject error(const QString& code, const QString& message);
QJsonObject stateJson(const StateSnapshot& snapshot);
QJsonObject viewportJson(const ViewportSnapshot& snapshot);
QJsonObject candlesJson(const CandleSnapshot& snapshot);
QJsonObject bookJson(const BookSnapshot& snapshot);
QJsonObject tradesJson(const TradesSnapshot& snapshot);
QJsonObject wallsJson(const WallsSnapshot& snapshot);
QByteArray jsonBytes(const QJsonObject& object);
} // namespace AgentApi
