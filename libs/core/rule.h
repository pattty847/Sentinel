#ifndef RULE_H
#define RULE_H

#include <QtCore/QString>
#include "TradeData.h" // We might need trade data for some rules

/**
 * @class Rule
 * @brief An abstract base class for all trading rules.
 *
 * This class defines the interface that all concrete rule classes must implement.
 * It uses a pure virtual function, check(), which makes it an "abstract" class.
 * You cannot create an instance of Rule directly; you must create an instance
 * of a class that inherits from it (e.g., CvdThresholdRule).
 */
class Rule
{
public:
    // Virtual destructor: Essential for any class intended for inheritance.
    // It ensures that when you delete a derived class through a base class pointer,
    // the correct destructor is called, preventing memory leaks.
    virtual ~Rule() {}

    /**
     * @brief The core evaluation function for the rule.
     * @param trade The most recent trade object.
     * @param cvd The most recent Cumulative Volume Delta value.
     * @return true if the rule's conditions are met (i.e., an alert should be triggered), false otherwise.
     *
     * This is a "pure virtual function" (indicated by '= 0'). Derived classes MUST provide
     * their own implementation for this function.
     */
    virtual bool check(const Trade &trade, double cvd) = 0;

    /**
     * @brief Gets the alert message for this rule.
     * @return A QString describing the alert.
     */
    virtual QString getAlertMessage() const = 0;
};

#endif // RULE_H 