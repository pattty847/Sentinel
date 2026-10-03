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
        QVector<Action> actions;
        if (m_pending.value(consumer) != normalized) actions = abandon(consumer);
        if (m_held.value(consumer) == normalized) {
            m_pending.remove(consumer);
            if (m_confirmed.contains(normalized)) {
                actions.push_back({Action::Activate, consumer, normalized});
                return actions;
            }
            if (m_requested.contains(normalized)) return actions;
            m_requested.insert(normalized);
            actions.push_back({Action::Subscribe, {}, normalized});
            return actions;
        }
        m_pending.insert(consumer, normalized);
        if (m_confirmed.contains(normalized)) {
            actions.append(activate(normalized));
            return actions;
        }
        if (m_requested.contains(normalized)) return actions;
        m_requested.insert(normalized);
        actions.push_back({Action::Subscribe, {}, normalized});
        return actions;
    }

    QVector<Action> acknowledged(const QString& symbol) {
        const QString normalized = symbol.trimmed().toUpper();
        if (!m_requested.contains(normalized)) return {};
        m_confirmed.insert(normalized);
        return activate(normalized);
    }

    QVector<Action> refused(const QString& symbol) {
        QString normalized = symbol.trimmed().toUpper();
        if (normalized.isEmpty() && m_pending.size() == 1)
            normalized = m_pending.cbegin().value();
        if (normalized.isEmpty()) return {};
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

    // Cancel an unacknowledged switch. An unsubscribe follows its subscribe on
    // the stream, even if the acknowledgement was lost.
    QVector<Action> abandon(const QString& consumer) {
        const QString symbol = m_pending.take(consumer);
        if (symbol.isEmpty() || m_pending.values().contains(symbol)
            || m_held.values().contains(symbol)) return {};
        m_requested.remove(symbol);
        m_confirmed.remove(symbol);
        return {{Action::Unsubscribe, {}, symbol}};
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

    QString held(const QString& consumer) const { return m_held.value(consumer); }
    QString pending(const QString& consumer) const { return m_pending.value(consumer); }
    bool requested(const QString& symbol) const { return m_requested.contains(symbol.trimmed().toUpper()); }

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
    // A second chart joins by calling request() with its own consumer ID.
    QHash<QString, QString> m_pending;
    QSet<QString> m_requested;
    QSet<QString> m_confirmed;
};
