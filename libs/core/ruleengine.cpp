#include "ruleengine.h"
#include "statisticsprocessor.h" // We need the full definition here
#include <utility> // For std::move

RuleEngine::RuleEngine(StatisticsProcessor *processor, QObject *parent)
    : QObject(parent), m_processor(processor)
{
    // We must have a valid processor to function
    Q_ASSERT(m_processor != nullptr);
}

void RuleEngine::addRule(std::unique_ptr<Rule> rule)
{
    // Use push_back for std::vector, not append.
    m_rules.push_back(std::move(rule));
}

/**
 * @brief This is the heart of the rule engine.
 * 
 * When a new trade arrives, it gets the latest data and iterates
 * through all its rules, checking each one.
 */
void RuleEngine::onNewTrade(const Trade &trade)
{
    // Get the latest CVD from our statistics processor
    double currentCvd = m_processor->getCvd();

    // Polymorphism in action!
    // We loop through a vector of base class pointers (Rule*).
    // The program determines at runtime which derived class's check()
    // method to call (e.g., CvdThresholdRule::check()).
    for (const auto &rule : m_rules) {
        if (rule->check(trade, currentCvd)) {
            // If the rule's conditions are met, emit the alert signal
            emit alertTriggered(rule->getAlertMessage());
        }
    }
} 