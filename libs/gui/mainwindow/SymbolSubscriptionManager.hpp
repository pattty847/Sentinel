#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

// GUI-thread state for subscriptions on the main window's shared stream client.
// A consumer keeps its old lease until the server acknowledges its replacement.
class SymbolSubscriptionManager {
public:
    struct Action {
        enum Kind { Subscribe, Unsubscribe, Activate, Refused } kind;
        QString consumer;
        QString symbol;
    };

    QVector<Action> request(const QString& consumer, const QString& symbol) {
        const QString normalized = symbol.trimmed().toUpper();
        if (consumer.isEmpty() || normalized.isEmpty()) return {};
        if (m_held.value(consumer) == normalized) {
            m_pending.remove(consumer);
            if (m_confirmed.contains(normalized)) return {{Action::Activate, consumer, normalized}};
            if (m_requested.contains(normalized)) return {};
            m_requested.insert(normalized);
            return {{Action::Subscribe, {}, normalized}};
        }
        m_pending.insert(consumer, normalized);
        if (m_confirmed.contains(normalized)) return activate(normalized);
        if (m_requested.contains(normalized)) return {};
        m_requested.insert(normalized);
        return {{Action::Subscribe, {}, normalized}};
    }

    QVector<Action> acknowledged(const QString& symbol) {
        const QString normalized = symbol.trimmed().toUpper();
        if (!m_requested.contains(normalized)) return {};
        m_confirmed.insert(normalized);
        return activate(normalized);
    }

    QVector<Action> refused(const QString& symbol) {
        const QString normalized = symbol.trimmed().toUpper();
        m_requested.remove(normalized);
        QVector<Action> actions;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (it.value() == normalized) {
                actions.push_back({Action::Refused, it.key(), normalized});
                it = m_pending.erase(it);
            } else ++it;
        }
        return actions;
    }

    // The server discarded this connection's subscriptions. Reacquire only
    // leases already held by consumers; an unacknowledged switch is abandoned.
    QVector<Action> reconnect() {
        m_pending.clear();
        m_confirmed.clear();
        m_requested.clear();
        QSet<QString> unique;
        for (const auto& symbol : m_held) unique.insert(symbol);
        QVector<Action> actions;
        for (const auto& symbol : unique) {
            m_requested.insert(symbol);
            actions.push_back({Action::Subscribe, {}, symbol});
        }
        return actions;
    }

    QVector<Action> release(const QString& consumer) {
        m_pending.remove(consumer);
        const QString old = m_held.take(consumer);
        if (old.isEmpty() || m_held.values().contains(old)) return {};
        m_confirmed.remove(old);
        m_requested.remove(old);
        return {{Action::Unsubscribe, {}, old}};
    }

    QString held(const QString& consumer) const { return m_held.value(consumer); }
    QSet<QString> heldSymbols() const {
        QSet<QString> result;
        for (const auto& symbol : m_held) result.insert(symbol);
        return result;
    }

private:
    QVector<Action> activate(const QString& symbol) {
        QVector<Action> actions;
        QSet<QString> oldSymbols;
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (it.value() != symbol) { ++it; continue; }
            const QString consumer = it.key();
            const QString old = m_held.value(consumer);
            m_held.insert(consumer, symbol);
            actions.push_back({Action::Activate, consumer, symbol});
            if (!old.isEmpty() && old != symbol) oldSymbols.insert(old);
            it = m_pending.erase(it);
        }
        for (const auto& old : oldSymbols) {
            if (m_held.values().contains(old)) continue;
            m_confirmed.remove(old);
            m_requested.remove(old);
            actions.push_back({Action::Unsubscribe, {}, old});
        }
        // An ack for a superseded request still occupies a server slot.
        if (actions.isEmpty() && !m_held.values().contains(symbol)) {
            m_confirmed.remove(symbol);
            m_requested.remove(symbol);
            actions.push_back({Action::Unsubscribe, {}, symbol});
        }
        return actions;
    }

    QHash<QString, QString> m_held;
    QHash<QString, QString> m_pending;
    QSet<QString> m_requested;
    QSet<QString> m_confirmed;
};
