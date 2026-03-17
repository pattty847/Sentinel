#include "MarketEventSource.hpp"
#include "../servermodel/TickBinaryLogger.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace trading {

VectorMarketEventSource::VectorMarketEventSource(std::vector<MarketEvent> events)
    : m_events(std::move(events)) {}

std::optional<MarketEvent> VectorMarketEventSource::next() {
    if (m_index >= m_events.size()) {
        return std::nullopt;
    }
    return m_events[m_index++];
}

CsvTradeEventSource::CsvTradeEventSource(std::istream& input)
    : m_input(input) {}

std::optional<MarketEvent> CsvTradeEventSource::next() {
    std::string line;
    while (std::getline(m_input, line)) {
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("timestamp_ms")) {
            continue;
        }

        std::stringstream ss(line);
        std::string timestampCell;
        std::string symbolCell;
        std::string priceCell;
        std::string qtyCell;
        if (!std::getline(ss, timestampCell, ',')) {
            continue;
        }
        if (!std::getline(ss, symbolCell, ',')) {
            continue;
        }
        if (!std::getline(ss, priceCell, ',')) {
            continue;
        }
        if (!std::getline(ss, qtyCell, ',')) {
            continue;
        }

        TradeEvent trade;
        trade.timestampMs = std::stoll(timestampCell);
        trade.symbol = symbolCell;
        trade.price = std::stod(priceCell);
        trade.qty = std::stod(qtyCell);

        MarketEvent event;
        event.type = MarketEventType::Trade;
        event.timestampMs = trade.timestampMs;
        event.trade = trade;
        return event;
    }
    return std::nullopt;
}

TickBinaryTradeEventSource::TickBinaryTradeEventSource(const std::filesystem::path& path,
                                                       std::string symbolFilter)
    : m_files(enumerateFiles(path))
    , m_symbolFilter(std::move(symbolFilter)) {}

std::optional<MarketEvent> TickBinaryTradeEventSource::next() {
    while (true) {
        if (!m_currentFile.is_open()) {
            if (!openNextFile()) {
                return std::nullopt;
            }
        }

        LogFormat::RecordHeader header{};
        m_currentFile.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!m_currentFile.good()) {
            closeCurrentFile();
            continue;
        }

        if (header.payload_len == 0) {
            continue;
        }

        switch (header.type) {
        case LogFormat::RecordType::Trade: {
            if (header.payload_len < sizeof(LogFormat::TradePayload)) {
                m_currentFile.seekg(static_cast<std::streamoff>(header.payload_len), std::ios::cur);
                continue;
            }

            LogFormat::TradePayload payload{};
            m_currentFile.read(reinterpret_cast<char*>(&payload), sizeof(payload));
            if (!m_currentFile.good()) {
                closeCurrentFile();
                continue;
            }

            const std::streamoff remaining =
                static_cast<std::streamoff>(header.payload_len - sizeof(LogFormat::TradePayload));
            if (remaining > 0) {
                m_currentFile.seekg(remaining, std::ios::cur);
            }

            if (!m_symbolFilter.empty() && m_currentSymbol != m_symbolFilter) {
                continue;
            }

            TradeEvent trade;
            trade.symbol = m_currentSymbol;
            trade.price = payload.price;
            trade.qty = payload.size;
            trade.timestampMs = static_cast<int64_t>(header.timestamp_ms);

            MarketEvent event;
            event.type = MarketEventType::Trade;
            event.timestampMs = trade.timestampMs;
            event.trade = trade;
            return event;
        }
        case LogFormat::RecordType::BookUpdate:
        case LogFormat::RecordType::BookSnapshot:
            m_currentFile.seekg(static_cast<std::streamoff>(header.payload_len), std::ios::cur);
            break;
        }
    }
}

bool TickBinaryTradeEventSource::openNextFile() {
    while (m_fileIndex < m_files.size()) {
        closeCurrentFile();
        m_currentFile.open(m_files[m_fileIndex++], std::ios::binary);
        if (!m_currentFile.is_open()) {
            continue;
        }

        LogFormat::FileHeader header{};
        m_currentFile.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!m_currentFile.good() || header.magic != LogFormat::MAGIC || header.version != LogFormat::VERSION) {
            closeCurrentFile();
            continue;
        }

        m_currentSymbol = trimNullTerminated(header.symbol, sizeof(header.symbol));
        if (m_currentSymbol.empty()) {
            closeCurrentFile();
            continue;
        }
        return true;
    }
    closeCurrentFile();
    return false;
}

void TickBinaryTradeEventSource::closeCurrentFile() {
    if (m_currentFile.is_open()) {
        m_currentFile.close();
    }
    m_currentSymbol.clear();
}

std::vector<std::filesystem::path> TickBinaryTradeEventSource::enumerateFiles(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> files;
    if (path.empty() || !std::filesystem::exists(path)) {
        return files;
    }

    if (std::filesystem::is_regular_file(path)) {
        files.push_back(path);
    } else if (std::filesystem::is_directory(path)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().extension() == ".bin") {
                files.push_back(entry.path());
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string TickBinaryTradeEventSource::trimNullTerminated(const char* data, std::size_t size) {
    const auto* end = static_cast<const char*>(std::memchr(data, '\0', size));
    const std::size_t length = end ? static_cast<std::size_t>(end - data) : size;
    return std::string(data, length);
}

} // namespace trading
