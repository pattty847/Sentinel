#include "AgentApiCodec.hpp"
#include "../../core/config/ConfigTypes.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QSet>
#include <cmath>
#include <limits>

namespace AgentApi {
namespace {
ParseResult fail(int status, const char* code, const char* message) {
    ParseResult result;
    result.kind = ParseResult::Kind::Error;
    result.status = status;
    result.code = QString::fromLatin1(code);
    result.message = QString::fromLatin1(message);
    return result;
}

bool allowedHost(const QByteArray& host) {
    const QByteArray lower = host.toLower();
    const qsizetype colon = lower.indexOf(':');
    const QByteArray name = colon < 0 ? lower : lower.left(colon);
    if (name != "127.0.0.1" && name != "localhost") return false;
    if (colon < 0) return true;
    const QByteArray port = lower.mid(colon + 1);
    if (port.isEmpty() || port.size() > 5) return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    bool ok = false;
    const uint value = port.toUInt(&ok);
    return ok && value > 0 && value <= 65535;
}

QJsonValue number(std::optional<double> value) {
    return value && std::isfinite(*value) ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}
template <typename T> QJsonValue integer(std::optional<T> value) {
    return value ? QJsonValue(static_cast<qint64>(*value)) : QJsonValue(QJsonValue::Null);
}
QJsonValue boolean(std::optional<bool> value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}
QJsonValue list(std::optional<QList<qint64>> values) {
    if (!values) return QJsonValue(QJsonValue::Null);
    QJsonArray out;
    for (qint64 value : *values) out.append(value);
    return out;
}
} // namespace

ParseResult RequestParser::feed(const QByteArray& bytes) {
    if (m_finished) return fail(400, "bad_request", "Request already complete");
    // Check before append so an unbounded input is never retained.
    if (bytes.size() > 24580 - m_buffer.size()) {
        m_finished = true;
        return fail(413, "request_too_large", "Request exceeds size limit");
    }
    m_buffer.append(bytes);
    const qsizetype headerEnd = m_buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (m_buffer.size() > 8192) {
            m_finished = true;
            return fail(431, "headers_too_large", "Headers exceed 8 KiB");
        }
        return {};
    }
    if (headerEnd + 4 > 8192) {
        m_finished = true;
        return fail(431, "headers_too_large", "Headers exceed 8 KiB");
    }
    QList<QByteArray> lines = m_buffer.left(headerEnd + 2).split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.isEmpty()) return fail(400, "bad_request", "Missing request line");
    const QList<QByteArray> parts = lines.front().trimmed().split(' ');
    if (parts.size() != 3 || parts[2] != "HTTP/1.1" || parts[1].isEmpty()) {
        m_finished = true;
        return fail(400, "bad_request", "Invalid request line");
    }
    QByteArray host;
    bool hasLength = false;
    qsizetype contentLength = 0;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (!line.endsWith('\r')) return fail(400, "bad_request", "Invalid header line");
        line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) return fail(400, "bad_request", "Invalid header line");
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "origin") return fail(403, "forbidden_origin", "Browser Origin is not allowed");
        if (key == "host") {
            if (!host.isEmpty()) return fail(400, "bad_request", "Duplicate Host header");
            host = value;
        }
        if (key == "transfer-encoding") return fail(400, "bad_request", "Transfer encoding is unsupported");
        if (key == "content-length") {
            if (hasLength || value.isEmpty()) return fail(400, "bad_request", "Invalid Content-Length");
            hasLength = true;
            for (char c : value) if (c < '0' || c > '9') return fail(400, "bad_request", "Invalid Content-Length");
            bool ok = false;
            contentLength = value.toLongLong(&ok);
            if (!ok || contentLength > 16384) return fail(413, "body_too_large", "Body exceeds 16 KiB");
        }
    }
    if (!allowedHost(host)) return fail(403, "forbidden_host", "Host must be localhost or 127.0.0.1");
    const qsizetype actualBody = m_buffer.size() - headerEnd - 4;
    if (actualBody > 16384 || actualBody > contentLength) return fail(400, "bad_request", "Unexpected body bytes");
    if (actualBody < contentLength) return {};
    m_finished = true;
    const QUrl url = QUrl::fromEncoded(parts[1], QUrl::StrictMode);
    if (!url.isValid() || !parts[1].startsWith('/') || url.hasFragment())
        return fail(400, "bad_request", "Invalid request target");
    const QString path = url.path();
    const bool known = path == "/api/v1/state" || path == "/api/v1/viewport" ||
                       path == "/api/v1/candles" || path == "/api/v1/book" ||
                       path == "/api/v1/trades" ||
                       path == "/api/v1/screenshot" || path == "/screenshot";
    if (!known) return fail(404, "not_found", "Unknown route");
    if (parts[0] != "GET") return fail(405, "method_not_allowed", "Only GET is supported");
    if (contentLength != 0) return fail(400, "bad_request", "GET body is unsupported");
    ParseResult result;
    result.kind = ParseResult::Kind::Complete;
    result.request = {parts[0], path, url.query(QUrl::FullyDecoded)};
    return result;
}

ValidationResult validateQuery(const Request& request, const QString& activeSymbol) {
    const QUrlQuery query(request.query);
    auto reject = [](int status, const char* code, const char* message) {
        ValidationResult result;
        result.status = status;
        result.code = QString::fromLatin1(code);
        result.message = QString::fromLatin1(message);
        return result;
    };
    if (request.path == "/api/v1/candles" || request.path == "/api/v1/book" ||
        request.path == "/api/v1/trades") {
        ValidationResult result;
        if (request.path == "/api/v1/candles") result.limit = 500;
        const bool candles = request.path == "/api/v1/candles";
        const bool book = request.path == "/api/v1/book";
        QSet<QString> seen;
        for (const auto& item : query.queryItems()) {
            const QString& key = item.first;
            if (seen.contains(key)) return reject(400, "invalid_parameter", "Duplicate query parameter");
            seen.insert(key);
            if (key == "symbol") continue;
            if (!((candles && (key == "startMs" || key == "endMs" || key == "timeframeMs" || key == "limit")) ||
                  (book && key == "levels") || (!candles && !book && (key == "windowMs" || key == "limit"))))
                return reject(400, "invalid_parameter", "Unknown query parameter");
            bool ok = false;
            const qint64 value = item.second.toLongLong(&ok);
            if (!ok || item.second.isEmpty() || value < 0 ||
                (value == 0 && key != "startMs" && key != "endMs"))
                return reject(422, "invalid_parameter", "Query parameter must be a nonnegative time or positive limit");
            if (key == "startMs") result.startMs = value;
            else if (key == "endMs") result.endMs = value;
            else if (key == "timeframeMs") result.timeframeMs = value;
            else if (key == "windowMs") result.windowMs = value;
            else if (key == "limit") {
                if (value > (candles ? 2000 : 1000)) return reject(422, "invalid_limit", "limit exceeds maximum");
                result.limit = static_cast<int>(value);
            } else if (key == "levels") {
                if (value > 200) return reject(422, "invalid_levels", "levels exceeds 200");
                result.levels = static_cast<int>(value);
            }
        }
        if (seen.contains("symbol") && query.queryItemValue("symbol") != activeSymbol)
            return reject(409, "symbol_mismatch", "Only the active symbol is available");
        if (candles) {
            if (!seen.contains("startMs") || !seen.contains("endMs") || !seen.contains("timeframeMs"))
                return reject(422, "missing_parameter", "startMs, endMs and timeframeMs are required");
            if (result.startMs >= result.endMs)
                return reject(422, "invalid_range", "startMs must precede endMs");
            if (result.timeframeMs % 1000 != 0)
                return reject(422, "invalid_timeframe", "timeframeMs must be whole seconds");
        }
        if (!candles && !book && result.windowMs > 900000)
            return reject(422, "invalid_window", "windowMs exceeds 900000");
        return result;
    }
    if (request.path == "/api/v1/state" || request.path == "/api/v1/viewport") {
        for (const auto& item : query.queryItems()) {
            if (item.first != "symbol") return reject(400, "invalid_parameter", "Unknown query parameter");
        }
        if (query.allQueryItemValues("symbol").size() > 1)
            return reject(400, "invalid_parameter", "Duplicate symbol");
        if (query.hasQueryItem("symbol") && query.queryItemValue("symbol") != activeSymbol)
            return reject(409, "symbol_mismatch", "Only the active symbol is available");
        return {};
    }
    if (request.path == "/api/v1/screenshot") {
        for (const auto& item : query.queryItems()) {
            if (item.first != "name" && item.first != "target")
                return reject(422, "unsupported_parameter", "Screenshot operation waits are not available in slice 1");
        }
        if (query.allQueryItemValues("name").size() > 1 || query.allQueryItemValues("target").size() > 1)
            return reject(400, "invalid_parameter", "Duplicate screenshot parameter");
        ValidationResult result;
        result.screenshotName = query.queryItemValue("name");
        result.screenshotTarget = query.queryItemValue("target");
        if (result.screenshotTarget.isEmpty()) result.screenshotTarget = "main";
        const QString& name = result.screenshotName;
        bool validName = name.size() <= 80 && name != "." && name != "..";
        for (const QChar c : name) {
            const ushort u = c.unicode();
            if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                  (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.')) {
                validName = false;
                break;
            }
        }
        if (!validName) return reject(422, "invalid_name", "Invalid screenshot name");
        if (result.screenshotTarget != "main" && result.screenshotTarget != "heatmap" && result.screenshotTarget != "lab")
            return reject(422, "invalid_target", "Unknown screenshot target");
        return result;
    }
    return {};
}

void applyAdvertisedServerConfig(StateSnapshot& s, const ServerConfig& config) {
    auto has = [&config](const char* field) { return config.wasAdvertised(field); };
    if (has("defaultSymbols")) {
        QStringList symbols;
        for (const auto& symbol : config.defaultSymbols) symbols.append(QString::fromStdString(symbol));
        s.defaultSymbols = symbols;
    }
    if (has("heatmap.configuredTimeframesMs")) {
        QList<qint64> values;
        for (int64_t tf : config.heatmap.timeframesMs) values.append(tf);
        s.configuredTimeframesMs = values;
    }
    if (has("heatmap.servedTimeframesMs")) {
        QList<qint64> values;
        for (int64_t tf : config.heatmap.servedTimeframesMs) values.append(tf);
        s.servedTimeframesMs = values;
    }
    if (has("heatmap.activeTimeframeMs")) s.activeTimeframeMs = config.heatmap.activeTimeframeMs;
    if (has("heatmap.gridWidth")) s.gridWidth = config.heatmap.gridWidth;
    if (has("heatmap.gridHeight")) s.gridHeight = config.heatmap.gridHeight;
    if (has("orderbook.tickSize")) s.tickSize = config.orderbook.tickSize;
    if (has("orderbook.bandPct")) s.bandPct = config.orderbook.bandPct;
    if (has("candles.bpsFast")) s.candleBpsFast = config.candles.bpsFast;
    if (has("candles.bpsSlow")) s.candleBpsSlow = config.candles.bpsSlow;
    if (has("candles.tickMultFast")) s.candleTickMultFast = config.candles.tickMultFast;
    if (has("candles.tickMultSlow")) s.candleTickMultSlow = config.candles.tickMultSlow;
    if (has("candles.silenceMsFast")) s.candleSilenceMsFast = config.candles.silenceMsFast;
    if (has("candles.silenceMsSlow")) s.candleSilenceMsSlow = config.candles.silenceMsSlow;
    if (has("candles.volumeFast")) s.candleVolumeFast = config.candles.volumeFast;
    if (has("candles.volumeSlow")) s.candleVolumeSlow = config.candles.volumeSlow;
    if (has("candles.tickSize")) s.candleTickSize = config.candles.tickSize;
}

QJsonObject envelope(const Metadata& meta, const QJsonObject& data) {
    return {{"ok", true}, {"meta", QJsonObject{
        {"sessionId", meta.sessionId}, {"symbol", meta.symbol},
        {"selectionEpoch", QString::number(meta.selectionEpoch)},
        {"observedAtMs", meta.observedAtMs}, {"source", "gui-cache"},
        {"stale", meta.stale}, {"coverage", meta.coverage}, {"truncated", meta.truncated}
    }}, {"data", data}};
}
QJsonObject error(const QString& code, const QString& message) {
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}
QJsonObject stateJson(const StateSnapshot& s) {
    QJsonArray symbols;
    if (s.defaultSymbols) for (const QString& symbol : *s.defaultSymbols) symbols.append(symbol);
    QJsonObject data{
        {"connected", s.connected}, {"serverConfigReady", s.serverConfigReady},
        {"server", QJsonObject{{"host", s.serverHost.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.serverHost)},
                                {"port", integer(s.serverPort)}}},
        {"serverConfig", QJsonObject{
            {"defaultSymbols", s.defaultSymbols ? QJsonValue(symbols) : QJsonValue(QJsonValue::Null)},
            {"heatmap", QJsonObject{{"configuredTimeframesMs", list(s.configuredTimeframesMs)},
                {"servedTimeframesMs", list(s.servedTimeframesMs)}, {"activeTimeframeMs", integer(s.activeTimeframeMs)},
                {"gridWidth", integer(s.gridWidth)}, {"gridHeight", integer(s.gridHeight)}}},
            {"orderbook", QJsonObject{{"tickSize", number(s.tickSize)}, {"bandPct", number(s.bandPct)}}},
            {"candles", QJsonObject{{"bpsFast", number(s.candleBpsFast)}, {"bpsSlow", number(s.candleBpsSlow)},
                {"tickMultFast", integer(s.candleTickMultFast)}, {"tickMultSlow", integer(s.candleTickMultSlow)},
                {"silenceMsFast", integer(s.candleSilenceMsFast)}, {"silenceMsSlow", integer(s.candleSilenceMsSlow)},
                {"volumeFast", number(s.candleVolumeFast)}, {"volumeSlow", number(s.candleVolumeSlow)},
                {"tickSize", number(s.candleTickSize)}}}}},
        {"lastReceivedAtMs", QJsonObject{{"heatmap", integer(s.heatmapReceivedAtMs)},
            {"candles", integer(s.candlesReceivedAtMs)}, {"book", integer(s.bookReceivedAtMs)},
            {"trades", integer(s.tradesReceivedAtMs)}}},
        {"layers", QJsonObject{{"heatmap", boolean(s.heatmapLayer)}, {"candles", boolean(s.candlesLayer)},
            {"footprint", boolean(s.footprintLayer)}, {"tpo", boolean(s.tpoLayer)},
            {"volumeProfile", boolean(s.volumeProfileLayer)}}}
    };
    return envelope(s.meta, data);
}
QJsonObject viewportJson(const ViewportSnapshot& s) {
    std::optional<double> msPerPx, pricePerPx;
    if (s.startMs && s.endMs && s.widthPx && *s.endMs > *s.startMs && *s.widthPx > 0)
        msPerPx = static_cast<double>(*s.endMs - *s.startMs) / *s.widthPx;
    if (s.priceMin && s.priceMax && s.heightPx && *s.priceMax > *s.priceMin && *s.heightPx > 0)
        pricePerPx = (*s.priceMax - *s.priceMin) / *s.heightPx;
    return envelope(s.meta, {{"startMs", integer(s.startMs)}, {"endMs", integer(s.endMs)},
        {"priceMin", number(s.priceMin)}, {"priceMax", number(s.priceMax)},
        {"heatmapTimeframeMs", integer(s.heatmapTimeframeMs)}, {"candleTimeframeMs", integer(s.candleTimeframeMs)},
        {"followLive", boolean(s.followLive)},
        {"viewportVersion", s.viewportVersion ? QJsonValue(QString::number(*s.viewportVersion)) : QJsonValue(QJsonValue::Null)},
        {"widthPx", number(s.widthPx)}, {"heightPx", number(s.heightPx)},
        {"zoom", QJsonObject{{"msPerPx", number(msPerPx)}, {"pricePerPx", number(pricePerPx)}}}});
}
QJsonObject candlesJson(const CandleSnapshot& s) {
    QJsonArray bars;
    for (const auto& b : s.bars) bars.append(QJsonObject{
        {"startMs", b.startMs}, {"endMs", b.endMs}, {"open", b.open},
        {"high", b.high}, {"low", b.low}, {"close", b.close},
        {"volume", b.volume}, {"closed", b.closed}, {"seq", QString::number(b.seq)}});
    return envelope(s.meta, {{"bars", bars}, {"nextStartMs", integer(s.nextStartMs)}});
}
QJsonObject bookJson(const BookSnapshot& s) {
    QJsonArray bids, asks, band;
    for (const auto& b : s.bids) bids.append(QJsonArray{b.price, b.qty});
    for (const auto& a : s.asks) asks.append(QJsonArray{a.price, a.qty});
    if (s.bandMin && s.bandMax) { band.append(*s.bandMin); band.append(*s.bandMax); }
    return envelope(s.meta, {{"bestBid", number(s.bestBid)}, {"bestAsk", number(s.bestAsk)},
        {"spread", number(s.spread)}, {"bids", bids}, {"asks", asks},
        {"bandLimited", s.bandLimited},
        {"band", s.bandMin && s.bandMax ? QJsonValue(band) : QJsonValue(QJsonValue::Null)},
        {"receivedAtMs", integer(s.receivedAtMs)}, {"scanLimited", s.scanLimited}});
}
QJsonObject tradesJson(const TradesSnapshot& s) {
    QJsonArray rows;
    for (const auto& t : s.trades) rows.append(QJsonObject{
        {"receivedAtMs", t.receivedAtMs}, {"eventTimeMs", QJsonValue(QJsonValue::Null)},
        {"id", t.id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(t.id)},
        {"side", t.side}, {"price", t.price}, {"qty", t.qty}});
    const auto& a = s.summary;
    return envelope(s.meta, {{"timeBasis", "received"}, {"trades", rows},
        {"summary", QJsonObject{{"count", static_cast<qint64>(a.count)},
            {"buyQty", a.buyQty}, {"sellQty", a.sellQty}, {"unknownQty", a.unknownQty},
            {"deltaQty", a.deltaQty}, {"vwap", number(a.vwap)}}},
        {"retentionLimited", s.retentionLimited}});
}

void TradeTape::append(TradeRow row) {
    if (!std::isfinite(row.price) || !std::isfinite(row.qty) || row.price <= 0 || row.qty <= 0) return;
    if (row.selectionEpoch != m_epoch) {
        m_head = m_count = 0;
        m_lastEvictedAtMs = 0;
        m_epoch = row.selectionEpoch;
    }
    while (m_count && m_rows[m_head].receivedAtMs < row.receivedAtMs - RetentionMs) {
        m_head = (m_head + 1) % Capacity;
        --m_count;
    }
    if (m_count == Capacity) {
        m_lastEvictedAtMs = m_rows[m_head].receivedAtMs;
        m_head = (m_head + 1) % Capacity;
        --m_count;
    }
    m_rows[(m_head + m_count) % Capacity] = std::move(row);
    ++m_count;
}
TradesSnapshot TradeTape::snapshot(Metadata meta, qint64 windowMs, size_t limit) const {
    TradesSnapshot out;
    out.meta = std::move(meta);
    if (windowMs <= 0 || limit == 0) return out;
    const qint64 cutoff = out.meta.observedAtMs - std::min(windowMs, RetentionMs);
    out.retentionLimited = m_epoch == out.meta.selectionEpoch &&
                           m_lastEvictedAtMs >= cutoff && m_lastEvictedAtMs != 0;
    double notional = 0, qtyTotal = 0;
    for (size_t i = 0; i < m_count; ++i) {
        const TradeRow& t = m_rows[(m_head + m_count - 1 - i) % Capacity];
        if (t.receivedAtMs < cutoff) break;
        if (t.receivedAtMs > out.meta.observedAtMs || t.selectionEpoch != out.meta.selectionEpoch) continue;
        ++out.summary.count;
        if (t.side == "buy") out.summary.buyQty += t.qty;
        else if (t.side == "sell") out.summary.sellQty += t.qty;
        else out.summary.unknownQty += t.qty;
        notional += t.price * t.qty;
        qtyTotal += t.qty;
        if (out.trades.size() < limit) out.trades.push_back(t);
    }
    out.summary.deltaQty = out.summary.buyQty - out.summary.sellQty;
    if (qtyTotal > 0) out.summary.vwap = notional / qtyTotal;
    out.meta.truncated = out.summary.count > out.trades.size();
    out.meta.coverage = out.summary.count ? "partial" : "unknown";
    return out;
}
QByteArray jsonBytes(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
} // namespace AgentApi
