#include "AgentApiCodec.hpp"
#include "../render/heatmap/HeatmapSettingsStore.hpp"
#include "../../core/config/ConfigTypes.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QSet>
#include <QRegularExpression>
#include <cmath>
#include <climits>
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
    bool jsonContent = false;
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
        if (key == "content-type") jsonContent = value.toLower() == "application/json";
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
    const bool operation = path.startsWith("/api/v1/operations/") && path.size() > 19;
    const bool control = path == "/api/v1/symbol" || path == "/api/v1/timeframe" ||
                         path == "/api/v1/viewport" || path == "/api/v1/layers" ||
                         path == "/api/v1/heatmap/settings" || path == "/api/v1/input" || path == "/api/v1/docks";
    const bool known = operation || control || path == "/api/v1/state" ||
                       path == "/api/v1/candles" || path == "/api/v1/book" ||
                       path == "/api/v1/trades" || path == "/api/v1/heatmap/walls" || path == "/api/v1/heatmap/state" ||
                       path == "/api/v1/screenshot" || path == "/screenshot" || path == "/api/v1/docks";
    if (!known) return fail(404, "not_found", "Unknown route");
    const bool readable = operation || path == "/api/v1/state" || path == "/api/v1/viewport" ||
                          path == "/api/v1/candles" || path == "/api/v1/book" ||
                          path == "/api/v1/trades" || path == "/api/v1/heatmap/walls" || path == "/api/v1/heatmap/state" ||
                          path == "/api/v1/screenshot" || path == "/screenshot" || path == "/api/v1/docks";
    if (!((parts[0] == "GET" && readable) || (parts[0] == "POST" && control)))
        return fail(405, "method_not_allowed", "Method not allowed");
    if (parts[0] == "GET" && contentLength != 0) return fail(400, "bad_request", "GET body is unsupported");
    if (parts[0] == "POST" && (!jsonContent || !contentLength))
        return fail(400, "invalid_content_type", "POST requires an application/json body");
    ParseResult result;
    result.kind = ParseResult::Kind::Complete;
    result.request = {parts[0], path, url.query(QUrl::FullyDecoded), m_buffer.mid(headerEnd + 4, contentLength)};
    return result;
}

bool isWidgetScreenshotTarget(const QString& target) {
    static const QStringList tabs{"Chart", "Tick", "Look", "Budgets", "Live", "Debug", "TPO"};
    static const QStringList docks{"orderBook", "watchlist", "screener", "stockChart", "paperTrading",
                                   "sec", "copenet", "telemetry", "statusBar"};
    return docks.contains(target) || target == "window" || target == "toolbar" || target == "chartmenu" || target == "settings" ||
           (target.startsWith("settings:") && tabs.contains(target.mid(9)));
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
    if (request.path == "/api/v1/docks") {
        if (!query.isEmpty()) return reject(400, "invalid_parameter", "Docks do not accept query parameters");
        return {};
    }
    if (request.path == "/api/v1/heatmap/walls") {
        ValidationResult result;
        QSet<QString> seen;
        for (const auto& item : query.queryItems()) {
            const QString& key = item.first;
            if (seen.contains(key)) return reject(400, "invalid_parameter", "Duplicate query parameter");
            seen.insert(key);
            if (key == "symbol") continue;
            if (key == "startMs" || key == "endMs" || key == "from_ms" || key == "to_ms" || key == "limit") {
                bool ok = false;
                const qint64 value = item.second.toLongLong(&ok);
                if (!ok || value < 0 || value > 9007199254740991LL || (key == "limit" && value == 0))
                    return reject(422, "invalid_parameter", "Time must be a nonnegative safe integer; limit must be positive");
                if (key == "startMs" || key == "from_ms") {
                    if (result.walls.startMs) return reject(400, "invalid_parameter", "Duplicate range start");
                    result.walls.startMs = value;
                } else if (key == "endMs" || key == "to_ms") {
                    if (result.walls.endMs) return reject(400, "invalid_parameter", "Duplicate range end");
                    result.walls.endMs = value;
                }
                else {
                    if (value > 100) return reject(422, "invalid_limit", "limit exceeds 100");
                    result.walls.limit = static_cast<int>(value);
                }
            } else if (key == "priceMin" || key == "priceMax" || key == "minQty" || key == "tick") {
                bool ok = false;
                const double value = item.second.toDouble(&ok);
                if (!ok || !std::isfinite(value) || value < 0 || (key != "minQty" && value == 0))
                    return reject(422, "invalid_parameter", "Price must be finite and positive; minQty must be nonnegative");
                if (key == "priceMin") result.walls.priceMin = value;
                else if (key == "priceMax") result.walls.priceMax = value;
                else if (key == "tick") result.walls.tick = value;
                else result.walls.minQty = value;
            } else return reject(400, "invalid_parameter", "Unknown query parameter");
        }
        if (seen.contains("symbol") && query.queryItemValue("symbol") != activeSymbol)
            return reject(409, "symbol_mismatch", "Only the active symbol is available");
        if ((seen.contains("from_ms") || seen.contains("to_ms")) &&
            !(result.walls.startMs && result.walls.endMs))
            return reject(422, "invalid_range", "from_ms and to_ms must specify a complete period");
        if (result.walls.startMs && result.walls.endMs && *result.walls.startMs >= *result.walls.endMs)
            return reject(422, "invalid_range", "startMs must precede endMs");
        if (result.walls.priceMin && result.walls.priceMax && *result.walls.priceMin >= *result.walls.priceMax)
            return reject(422, "invalid_range", "priceMin must be below priceMax");
        return result;
    }
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
    if (request.path == "/api/v1/state" || request.path == "/api/v1/viewport" || request.path == "/api/v1/heatmap/state") {
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
        ValidationResult result;
        QSet<QString> seen;
        for (const auto& item : query.queryItems()) {
            if (seen.contains(item.first)) return reject(400, "invalid_parameter", "Duplicate screenshot parameter");
            seen.insert(item.first);
            if (item.first != "name" && item.first != "target" && item.first != "afterOperation" && item.first != "waitMs")
                return reject(400, "invalid_parameter", "Unknown screenshot parameter");
        }
        result.afterOperation = query.queryItemValue("afterOperation");
        if (seen.contains("afterOperation") && !result.afterOperation.startsWith('o'))
            return reject(422, "invalid_operation", "Invalid operation ID");
        if (seen.contains("waitMs")) {
            bool ok = false;
            result.waitMs = query.queryItemValue("waitMs").toInt(&ok);
            if (!ok || result.waitMs < 0 || result.waitMs > 5000)
                return reject(422, "invalid_wait", "waitMs must be 0..5000");
        }
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
        if (result.screenshotTarget != "main" && result.screenshotTarget != "heatmap" &&
            !isWidgetScreenshotTarget(result.screenshotTarget))
            return reject(422, "invalid_target", "Unknown screenshot target");
        return result;
    }
    if (request.path.startsWith("/api/v1/operations/")) {
        ValidationResult result;
        if (query.queryItems().size() > 1 ||
            (!query.queryItems().isEmpty() && query.queryItems().front().first != "waitMs"))
            return reject(400, "invalid_parameter", "Only waitMs is supported");
        if (query.hasQueryItem("waitMs")) {
            bool ok = false;
            result.waitMs = query.queryItemValue("waitMs").toInt(&ok);
            if (!ok || result.waitMs < 0 || result.waitMs > 5000)
                return reject(422, "invalid_wait", "waitMs must be 0..5000");
        }
        return result;
    }
    return {};
}

ControlValidation validateControl(const Request& request, const std::optional<QList<qint64>>& served,
                                  const QJsonObject& currentDocks) {
    auto reject = [](const char* code, const char* message) {
        ControlValidation r; r.status = 422; r.code = code; r.message = message; return r;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return reject("invalid_body", "Body must be a JSON object");
    const QJsonObject obj = doc.object();
    if (obj.isEmpty()) return reject("invalid_body", "Body must contain a control");
    const QString kind = request.path.mid(QStringLiteral("/api/v1/").size());
    ControlValidation result;
    if (kind == "docks") {
        QStringList ids = currentDocks.keys();
        const QString validIds = ids.join(", ");
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
            if (it.key() != "visible" && it.key() != "focus" && it.key() != "persist")
                return reject("invalid_field", "Unknown dock control field");
        if (obj.contains("persist")) {
            if (!obj.value("persist").isBool()) return reject("invalid_docks", "persist must be boolean");
            result.body.persistDocks = obj.value("persist").toBool();
        }
        if (obj.contains("focus") == obj.contains("visible"))
            return reject("invalid_docks", "Supply exactly one of focus or visible");
        QJsonObject desired = currentDocks;
        if (obj.contains("focus")) {
            if (!obj.value("focus").isString() || !currentDocks.contains(obj.value("focus").toString())) {
                result.status = 422; result.code = "unknown_dock";
                result.message = "Unknown dock id; valid ids: " + validIds;
                return result;
            }
            result.body.dockFocus = obj.value("focus").toString();
            for (auto it = desired.begin(); it != desired.end(); ++it)
                it.value() = it.key() == result.body.dockFocus;
        } else {
            const auto value = obj.value("visible");
            if (!value.isObject() || value.toObject().isEmpty())
                return reject("invalid_docks", "visible must be a nonempty object");
            result.body.dockVisible = value.toObject();
            for (auto it = result.body.dockVisible.constBegin(); it != result.body.dockVisible.constEnd(); ++it) {
                if (!currentDocks.contains(it.key())) {
                    result.status = 422; result.code = "unknown_dock";
                    result.message = "Unknown dock id " + it.key() + "; valid ids: " + validIds;
                    return result;
                }
                if (!it.value().isBool()) return reject("invalid_docks", "Dock visibility must be boolean");
                desired.insert(it.key(), it.value());
            }
        }
        bool any = false;
        for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) any |= it.value().toBool();
        if (!any) return reject("hide_all_docks", "At least one dock must remain visible");
        return result;
    }
    if (kind == "heatmap/settings") {
        auto patch = obj;
        if (patch.contains("persist")) {
            if (!patch["persist"].isBool()) return reject("invalid_settings", "persist must be boolean");
            result.body.persistHeatmapSettings = patch.take("persist").toBool();
        }
        if (patch.isEmpty()) return reject("invalid_settings", "Body must contain a heatmap setting");
        heatmap::HeatmapChartSettings check;
        const auto error = heatmap::applySettingsPatch(check, patch);
        if (!error.isEmpty()) {
            result.status = 422; result.code = "invalid_settings"; result.message = error;
        } else result.body.heatmapSettings = patch;
        return result;
    }
    if (kind == "input") {
        const QStringList keys{"kind", "target", "x", "y", "deltaY", "modifiers"};
        for (auto it = obj.begin(); it != obj.end(); ++it)
            if (!keys.contains(it.key())) return reject("invalid_field", "Unknown input field");
        const QStringList kinds{"wheel", "dragStart", "dragMove", "dragEnd", "click", "doubleClick"};
        const QStringList targets{"chart", "priceAxis", "timeAxis"};
        if (!obj["kind"].isString() || !kinds.contains(obj["kind"].toString())) return reject("invalid_kind", "Unknown input kind");
        if (!obj["target"].isString() || !targets.contains(obj["target"].toString())) return reject("invalid_target", "Unknown input target");
        for (const char *key : {"x", "y"})
            if (!obj[key].isDouble() || !std::isfinite(obj[key].toDouble()) || obj[key].toDouble() < 0 || obj[key].toDouble() > 1000000)
                return reject("invalid_position", "x and y must be finite logical pixels in 0..1000000");
        auto &c = result.body.input;
        c.kind = obj["kind"].toString(); c.target = obj["target"].toString();
        c.x = obj["x"].toDouble(); c.y = obj["y"].toDouble();
        if (c.kind == "wheel") {
            const auto v = obj["deltaY"];
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() == 0 ||
                std::abs(v.toDouble()) > 12000 || std::floor(v.toDouble()) != v.toDouble())
                return reject("invalid_delta", "Wheel deltaY must be a nonzero integer in +/-12000");
            c.deltaY = int(v.toDouble());
        } else if (obj.contains("deltaY")) return reject("invalid_delta", "deltaY is only supported for wheel");
        if (obj.contains("modifiers")) {
            if (!obj["modifiers"].isArray()) return reject("invalid_modifiers", "modifiers must be an array");
            const QStringList allowed{"shift", "control", "alt", "meta"};
            for (const auto &v : obj["modifiers"].toArray()) {
                if (!v.isString() || !allowed.contains(v.toString()) || c.modifiers.contains(v.toString()))
                    return reject("invalid_modifiers", "Unknown or duplicate modifier");
                c.modifiers.append(v.toString());
            }
        }
        return result;
    }
    if (kind != "symbol" && kind != "timeframe" && kind != "viewport" && kind != "layers")
        return reject("invalid_kind", "Unknown control route");
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QString key = it.key();
        const QJsonValue v = it.value();
        if (kind == "symbol") {
            if (key != "symbol" || !v.isString()) return reject("invalid_symbol", "Expected symbol string");
            result.body.symbol = v.toString();
            static const QRegularExpression pattern("^[A-Z0-9]{2,20}-[A-Z0-9]{2,20}$");
            if (!pattern.match(result.body.symbol).hasMatch()) return reject("invalid_symbol", "Invalid symbol format");
        } else if (kind == "timeframe") {
            if (key != "heatmapTimeframeMs" && key != "candleTimeframeMs") return reject("invalid_field", "Unknown timeframe field");
            if (!v.isDouble() || v.toDouble() <= 0 || v.toDouble() > INT_MAX || std::floor(v.toDouble()) != v.toDouble())
                return reject("invalid_timeframe", "Timeframe must be a positive integer");
            const qint64 ms = static_cast<qint64>(v.toDouble());
            if (result.body.timeframeMs && result.body.timeframeMs != ms)
                return reject("timeframe_mismatch", "Linked timeframes must be equal");
            result.body.timeframeMs = ms;
        } else if (kind == "viewport") {
            if (key == "followLive") {
                if (!v.isBool()) return reject("invalid_follow", "followLive must be boolean");
                result.body.followLive = v.toBool();
            } else if (key == "autoScale") {
                if (!v.isBool()) return reject("invalid_auto_scale", "autoScale must be boolean");
                result.body.autoScale = v.toBool();
            } else if (key == "fit") {
                static const QStringList kFits{"time", "price", "both", "default"};
                if (!v.isString() || !kFits.contains(v.toString()))
                    return reject("invalid_fit", "fit must be time, price, both or default");
                result.body.fit = v.toString();
            } else if (key == "startMs" || key == "endMs") {
                if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() > 9007199254740991.0 || std::floor(v.toDouble()) != v.toDouble())
                    return reject("invalid_range", "Time bounds must be nonnegative safe integers");
                if (key == "startMs") result.body.startMs = static_cast<qint64>(v.toDouble());
                else result.body.endMs = static_cast<qint64>(v.toDouble());
            } else if (key == "priceMin" || key == "priceMax") {
                if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() <= 0)
                    return reject("invalid_range", "Price bounds must be finite positive numbers");
                if (key == "priceMin") result.body.priceMin = v.toDouble();
                else result.body.priceMax = v.toDouble();
            } else return reject("invalid_field", "Unknown viewport field");
        } else if (kind == "layers") {
            if (key == "tpoLayout" || key == "tpoTheme") {
                static const QStringList kLayouts{"split", "collapsed"};
                static const QStringList kThemes{"rainbow", "calm", "sage"};
                const auto& allowed = key == "tpoLayout" ? kLayouts : kThemes;
                if (!v.isString() || !allowed.contains(v.toString()))
                    return reject("invalid_layer", key == "tpoLayout" ? "tpoLayout must be split or collapsed"
                                                                      : "tpoTheme must be rainbow, calm or sage");
                result.body.layers.insert(key, v);
                continue;
            }
            if (key != "heatmap" && key != "candles" && key != "footprint" && key != "tpo" && key != "volumeProfile")
                return reject("invalid_field", "Unknown layer");
            if (!v.isBool()) return reject("invalid_layer", "Layer values must be boolean");
            result.body.layers.insert(key, v);
        }
    }
    if (kind == "timeframe" && (!served || !served->contains(result.body.timeframeMs)))
        return reject("timeframe_unavailable", "Timeframe is not advertised as served");
    if (kind == "viewport") {
        const auto& b = result.body;
        if (b.startMs.has_value() != b.endMs.has_value() || b.priceMin.has_value() != b.priceMax.has_value())
            return reject("invalid_range", "Bounds must be paired");
        if (b.startMs && *b.startMs >= *b.endMs)
            return reject("invalid_range", "Time bounds must increase");
        if (b.priceMin && *b.priceMin >= *b.priceMax)
            return reject("invalid_range", "Price bounds must increase");
        if (b.followLive.value_or(false) && (b.startMs || b.priceMin))
            return reject("invalid_range", "Bounds cannot enable followLive");
        if (!b.fit.isEmpty() && (b.startMs || b.priceMin || b.followLive || b.autoScale))
            return reject("invalid_fit", "fit cannot be combined with bounds, followLive or autoScale");
        if (b.autoScale.value_or(false) && b.priceMin)
            return reject("invalid_auto_scale", "Price bounds cannot enable autoScale");
    }
    return result;
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
        {"lastSubscriptionRefusal", s.lastSubscriptionRefusal.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s.lastSubscriptionRefusal)},
        {"render", QJsonObject{{"frameP50Ms", number(s.frameP50Ms)},
            {"frameP95Ms", number(s.frameP95Ms)}, {"rateHz", number(s.renderRateHz)},
            {"idle", boolean(s.frameIdle)}}},
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
            {"trades", integer(s.tradesReceivedAtMs)}, {"footprint", integer(s.footprintReceivedAtMs)},
            {"tpo", integer(s.tpoReceivedAtMs)}, {"volumeProfile", integer(s.volumeProfileReceivedAtMs)}}},
        {"layers", QJsonObject{{"heatmap", boolean(s.heatmapLayer)}, {"candles", boolean(s.candlesLayer)},
            {"footprint", boolean(s.footprintLayer)}, {"tpo", boolean(s.tpoLayer)},
            {"volumeProfile", boolean(s.volumeProfileLayer)},
            {"tpoLayout", s.tpoLayout ? QJsonValue(*s.tpoLayout) : QJsonValue()},
            {"tpoTheme", s.tpoTheme ? QJsonValue(*s.tpoTheme) : QJsonValue()}}}
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
        {"followLive", boolean(s.followLive)}, {"autoScale", boolean(s.autoScale)},
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
        {"receivedAtMs", t.receivedAtMs}, {"eventTimeMs", t.eventTimeMs ? QJsonValue(*t.eventTimeMs) : QJsonValue(QJsonValue::Null)},
        {"id", t.id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(t.id)},
        {"side", t.side}, {"price", t.price}, {"qty", t.qty}});
    const auto& a = s.summary;
    return envelope(s.meta, {{"timeBasis", "received"}, {"trades", rows},
        {"summary", QJsonObject{{"count", static_cast<qint64>(a.count)},
            {"buyQty", a.buyQty}, {"sellQty", a.sellQty}, {"unknownQty", a.unknownQty},
            {"deltaQty", a.deltaQty}, {"vwap", number(a.vwap)}}},
        {"retentionLimited", s.retentionLimited}});
}

QJsonObject wallsJson(const WallsSnapshot& s) {
    QJsonArray rows;
    for (const auto& w : s.data.walls) {
        QJsonObject row{
        {"bucketStartMs", qint64(w.bucketStartMs)}, {"priceLow", w.priceLow},
        {"priceHigh", w.priceHigh}, {"side", w.ask ? "ask" : "bid"},
        {"qty", w.qty}, {"notional", w.notional}, {"forming", w.forming},
        {"meanQty", w.meanQty}, {"firstSeenMs", qint64(w.firstSeenMs)}, {"lastSeenMs", qint64(w.lastSeenMs)},
        {"columns", qint64(w.columns)}};
        row.insert("rank", rows.size() + 1);
        rows.append(row);
    }
    auto out = envelope(s.meta, {{"basis", "recording-twap-sum"},
        {"bandTick", s.data.bandTick > 0 ? QJsonValue(s.data.bandTick) : QJsonValue(QJsonValue::Null)},
        {"loadedRange", QJsonArray{qint64(s.data.loadedStartMs), qint64(s.data.loadedEndMs)}},
        {"recordedColumns", qint64(s.data.recordedColumns)}, {"missingColumns", qint64(s.data.missingColumns)},
        {"note", "Aggregated resting size per cell, not individual orders"}, {"walls", rows}});
    {
        auto data = out.value("data").toObject();
        data.insert("renderer", "gpu"); // the constant "gpu" since S8a (goes in S8b)
        data.insert("tick", s.data.bandTick);
        data.insert("unknownRows", s.data.unknownRows);
        data.insert("range", QJsonObject{{"from_ms", qint64(s.data.rangeStartMs)}, {"to_ms", qint64(s.data.rangeEndMs)},
                                      {"priceMin", s.data.rangePriceMin}, {"priceMax", s.data.rangePriceMax}});
        out.insert("data", data);
    }
    return out;
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
