#pragma once

#include "ITheme.hpp"
#include <QString>

/**
 * Dark theme for Sentinel Trading Terminal.
 * Professional dark theme optimized for trading environments.
 */
class DarkTheme : public ITheme {
public:
    QString name() const override { return "Dark"; }
    QString id() const override { return "dark"; }
    QString description() const override { return "Professional dark theme optimized for trading"; }
    QString stylesheet() const override;
};

