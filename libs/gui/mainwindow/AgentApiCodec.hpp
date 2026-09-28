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

ValidationResult validateQuery(const Request& request, const QString& activeSymbol = {});
void applyAdvertisedServerConfig(StateSnapshot& snapshot, const ServerConfig& config);

QJsonObject envelope(const Metadata& meta, const QJsonObject& data);
QJsonObject error(const QString& code, const QString& message);
QJsonObject stateJson(const StateSnapshot& snapshot);
QJsonObject viewportJson(const ViewportSnapshot& snapshot);
QJsonObject candlesJson(const CandleSnapshot& snapshot);
QJsonObject bookJson(const BookSnapshot& snapshot);
QJsonObject tradesJson(const TradesSnapshot& snapshot);
QByteArray jsonBytes(const QJsonObject& object);
} // namespace AgentApi
