#ifndef RULEENGINE_H
#define RULEENGINE_H

#include <QObject>
#include <vector>
#include <memory>
#include "Rule.h"

class StatisticsProcessor; // Forward-declaration to avoid circular dependencies

/**
 * @class RuleEngine
 * @brief Manages and evaluates a collection of trading rules.
 *
 * This engine holds a list of rules. It receives data updates, passes them
 * to each rule for evaluation, and emits an alert signal if any rule's
 * conditions are met.
 */
class RuleEngine : public QObject
{
    Q_OBJECT

public:
    // The engine needs a reference to the processor to get the latest data
    explicit RuleEngine(StatisticsProcessor *processor, QObject *parent = nullptr);

    // Public method to allow adding new rules from the outside
    void addRule(std::unique_ptr<Rule> rule);

public slots:
    /**
     * @brief Slot to receive new trades and trigger rule evaluation.
     * This is the main entry point for the engine's logic.
     * @param trade The latest trade to evaluate rules against.
     */
    void onNewTrade(const Trade &trade);

signals:
    /**
     * @brief Emitted when any rule's check() method returns true.
     * @param alertMessage The descriptive message from the rule that fired.
     */
    void alertTriggered(const QString &alertMessage);

private:
    // A list to hold all our rule objects. We use unique_ptr for automatic memory management.
    std::vector<std::unique_ptr<Rule>> m_rules;

    // A pointer to the statistics processor. The engine doesn't own this.
    StatisticsProcessor *m_processor;
};

#endif // RULEENGINE_H 