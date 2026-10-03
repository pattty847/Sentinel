#pragma once
#include <QString>
#include <QHash>
#include <QList>
#include <optional>

namespace AgentApi {
struct Operation {
    QString id, kind, status = "applied";
    QString errorCode, errorMessage;
    quint64 revision = 0, frameId = 0, viewportVersion = 0;
};

// GUI-thread state. The frame acknowledgement is supplied by an atomic-only
// render callback and consumed by poll(); no QObject is read on the render thread.
class Operations {
public:
    Operation apply(const QString& kind, quint64 viewportVersion, bool pending = false);
    std::optional<Operation> activate(const QString& id);
    void poll(quint64 renderedRevision, quint64 frameId);
    void fail(const QString& id, const QString& code = {}, const QString& message = {});
    void supersede(const QString& id);
    std::optional<Operation> find(const QString& id) const;
private:
    quint64 m_nextRevision = 0;
    QList<QString> m_order;
    QHash<QString, Operation> m_ops;
};
bool shouldDefer(const Operation& operation, qint64 nowMs, qint64 deadlineMs);
}
