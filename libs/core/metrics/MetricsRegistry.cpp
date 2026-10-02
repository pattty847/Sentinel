#include "MetricsRegistry.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace sentinel::metrics {

struct MetricsRegistry::Series {
    std::string labelText; // rendered "{k=\"v\",...}" or empty
    std::unique_ptr<Counter> counter;
    std::unique_ptr<Gauge> gauge;
    Sampler sampler;
};

struct MetricsRegistry::Family {
    std::string name, help;
    Type type;
    std::deque<Series> series; // deque: stable addresses for returned references
};

MetricsRegistry::MetricsRegistry() = default;
MetricsRegistry::~MetricsRegistry() = default;

namespace {
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
} // namespace

bool MetricsRegistry::validMetricName(std::string_view name) {
    if (name.empty() || !(isAlpha(name[0]) || name[0] == '_' || name[0] == ':')) return false;
    for (char c : name)
        if (!(isAlpha(c) || isDigit(c) || c == '_' || c == ':')) return false;
    return true;
}

bool MetricsRegistry::validLabelName(std::string_view name) {
    if (name.empty() || !(isAlpha(name[0]) || name[0] == '_')) return false;
    if (name.substr(0, 2) == "__") return false; // reserved for Prometheus internals
    for (char c : name)
        if (!(isAlpha(c) || isDigit(c) || c == '_')) return false;
    return true;
}

std::string MetricsRegistry::escapeLabelValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string MetricsRegistry::escapeHelp(std::string_view help) {
    std::string out;
    out.reserve(help.size());
    for (char c : help) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string MetricsRegistry::formatValue(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";
    // std::to_chars: shortest round-trip text and independent of the C locale
    // (snprintf would print "1,5" after QCoreApplication under de_DE).
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
    return ec == std::errc() ? std::string(buf, end) : std::string("NaN");
}

std::string MetricsRegistry::renderLabels(const Labels& labels) {
    if (labels.empty()) return {};
    std::string out = "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (!validLabelName(labels[i].first))
            throw std::invalid_argument("invalid metric label name: " + labels[i].first);
        for (size_t j = 0; j < i; ++j)
            if (labels[j].first == labels[i].first)
                throw std::invalid_argument("duplicate metric label name: " + labels[i].first);
        if (i) out += ',';
        out += labels[i].first;
        out += "=\"";
        out += escapeLabelValue(labels[i].second);
        out += '"';
    }
    out += '}';
    return out;
}

MetricsRegistry::Family& MetricsRegistry::family(std::string_view name, std::string_view help, Type type) {
    if (!validMetricName(name)) throw std::invalid_argument("invalid metric name: " + std::string(name));
    for (auto& f : families_) {
        if (f->name != name) continue;
        if (f->type != type) throw std::logic_error("metric registered with two types: " + std::string(name));
        return *f;
    }
    families_.push_back(std::make_unique<Family>(Family{std::string(name), std::string(help), type, {}}));
    return *families_.back();
}

MetricsRegistry::Series* MetricsRegistry::find(Family& f, const std::string& labelText) {
    for (auto& s : f.series)
        if (s.labelText == labelText) return &s;
    return nullptr;
}

Counter& MetricsRegistry::counter(std::string_view name, std::string_view help, const Labels& labels) {
    const std::string labelText = renderLabels(labels);
    std::lock_guard lock(mutex_);
    Family& f = family(name, help, Type::Counter);
    if (Series* s = find(f, labelText)) {
        if (!s->counter) throw std::logic_error("metric series already has a sampler: " + std::string(name));
        return *s->counter;
    }
    f.series.push_back({labelText, std::make_unique<Counter>(), nullptr, {}});
    return *f.series.back().counter;
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string_view help, const Labels& labels) {
    const std::string labelText = renderLabels(labels);
    std::lock_guard lock(mutex_);
    Family& f = family(name, help, Type::Gauge);
    if (Series* s = find(f, labelText)) {
        if (!s->gauge) throw std::logic_error("metric series already has a sampler: " + std::string(name));
        return *s->gauge;
    }
    f.series.push_back({labelText, nullptr, std::make_unique<Gauge>(), {}});
    return *f.series.back().gauge;
}

void MetricsRegistry::counterFn(std::string_view name, std::string_view help, const Labels& labels, Sampler sampler) {
    addSampler(name, help, Type::Counter, labels, std::move(sampler));
}

void MetricsRegistry::gaugeFn(std::string_view name, std::string_view help, const Labels& labels, Sampler sampler) {
    addSampler(name, help, Type::Gauge, labels, std::move(sampler));
}

void MetricsRegistry::addSampler(std::string_view name, std::string_view help, Type type, const Labels& labels,
                                 Sampler sampler) {
    std::string labelText = renderLabels(labels);
    std::lock_guard lock(mutex_);
    Family& f = family(name, help, type);
    if (find(f, labelText)) throw std::logic_error("metric series registered twice: " + std::string(name));
    f.series.push_back({std::move(labelText), nullptr, nullptr, std::move(sampler)});
}

std::string MetricsRegistry::render() const {
    std::string out;
    out.reserve(4096);
    std::lock_guard lock(mutex_);
    for (const auto& f : families_) {
        out += "# HELP ";
        out += f->name;
        out += ' ';
        out += escapeHelp(f->help);
        out += "\n# TYPE ";
        out += f->name;
        out += f->type == Type::Counter ? " counter\n" : " gauge\n";
        for (const auto& s : f->series) {
            std::string value;
            if (s.counter) {
                char buf[24];
                const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, s.counter->value());
                value.assign(buf, ec == std::errc() ? end : buf);
            } else if (s.gauge) {
                value = formatValue(s.gauge->value());
            } else {
                const std::optional<double> v = s.sampler ? s.sampler() : std::nullopt;
                if (!v) continue;
                value = formatValue(*v);
            }
            out += f->name;
            out += s.labelText;
            out += ' ';
            out += value;
            out += '\n';
        }
    }
    return out;
}

} // namespace sentinel::metrics
