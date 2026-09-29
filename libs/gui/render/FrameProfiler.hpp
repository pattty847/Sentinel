/*
Sentinel — FrameProfiler
Role: per-stage render timing for UnifiedGridRenderer::updatePaintNode.
      Enabled with SENTINEL_FRAME_PROFILE=1; prints one line per second:
      frame count, then avg/max milliseconds for each stage.
Threading: render thread only (one instance per renderer).
*/
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cstdint>

class FrameProfiler {
public:
    enum Stage { Context, Mapping, Uploads, Overlays, AxisText, Labels, TextEnd, StageCount };

    static bool enabled() {
        static const bool on = qEnvironmentVariableIsSet("SENTINEL_FRAME_PROFILE");
        return on;
    }

    void beginFrame() {
        if (!m_window.isValid()) m_window.start();
        m_frame.start();
        m_lastNs = 0;
    }

    // Charges the time since the previous mark (or beginFrame) to `stage`.
    void mark(Stage stage) {
        const qint64 now = m_frame.nsecsElapsed();
        const qint64 spent = now - m_lastNs;
        m_lastNs = now;
        m_sumNs[stage] += spent;
        if (spent > m_maxNs[stage]) m_maxNs[stage] = spent;
    }

    // Returns a report once per second, otherwise an empty string.
    QString endFrame() {
        const qint64 total = m_frame.nsecsElapsed();
        m_totalSumNs += total;
        if (total > m_totalMaxNs) m_totalMaxNs = total;
        ++m_frames;
        if (m_window.elapsed() < 1000 || m_frames == 0) return {};

        static const char* kNames[StageCount] = {"context", "mapping", "uploads", "overlays",
                                                 "axis", "labels", "textEnd"};
        auto ms = [](qint64 ns) { return QString::number(ns / 1e6, 'f', 2); };
        QString line = QString("FRAME PROFILE frames=%1 total avg=%2 max=%3 ms |")
                           .arg(m_frames).arg(ms(m_totalSumNs / m_frames)).arg(ms(m_totalMaxNs));
        for (int i = 0; i < StageCount; ++i) {
            line += QString(" %1 %2/%3").arg(kNames[i], ms(m_sumNs[i] / m_frames), ms(m_maxNs[i]));
        }
        m_sumNs.fill(0);
        m_maxNs.fill(0);
        m_totalSumNs = 0;
        m_totalMaxNs = 0;
        m_frames = 0;
        m_window.restart();
        return line;
    }

private:
    QElapsedTimer m_window;
    QElapsedTimer m_frame;
    qint64 m_lastNs = 0;
    std::array<qint64, StageCount> m_sumNs{};
    std::array<qint64, StageCount> m_maxNs{};
    qint64 m_totalSumNs = 0;
    qint64 m_totalMaxNs = 0;
    int m_frames = 0;
};
