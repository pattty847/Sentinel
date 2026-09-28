#include "AgentApiOperations.hpp"

namespace AgentApi {
Operation Operations::apply(const QString& kind, quint64 viewportVersion) {
    for (auto it = m_ops.begin(); it != m_ops.end(); ++it)
        if (it->kind == kind && it->status == "applied") it->status = "superseded";
    Operation op;
    op.id = "o" + QString::number(++m_nextRevision);
    op.kind = kind;
    op.revision = m_nextRevision;
    op.viewportVersion = viewportVersion;
    m_order.append(op.id);
    m_ops.insert(op.id, op);
    if (m_order.size() > 256) m_ops.remove(m_order.takeFirst());
    return op;
}
void Operations::poll(quint64 renderedRevision, quint64 frameId) {
    if (!frameId) return;
    for (auto it = m_ops.begin(); it != m_ops.end(); ++it) {
        if (it->status == "applied" && it->revision <= renderedRevision) {
            it->status = "rendered";
            it->frameId = frameId;
        }
    }
}
bool shouldDefer(const Operation& operation, qint64 nowMs, qint64 deadlineMs) {
    return operation.status == "applied" && nowMs < deadlineMs;
}
void Operations::fail(const QString& id) {
    auto it = m_ops.find(id);
    if (it != m_ops.end() && it->status == "applied") it->status = "failed";
}
std::optional<Operation> Operations::find(const QString& id) const {
    auto it = m_ops.constFind(id);
    if (it == m_ops.cend()) return std::nullopt;
    return *it;
}
}
