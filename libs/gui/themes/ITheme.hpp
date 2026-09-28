#pragma once

#include <QString>
#include <QStringList>

/**
 * Interface for theme implementations.
 * Each theme provides a name and stylesheet.
 */
class ITheme {
public:
    virtual ~ITheme() = default;
    
    /**
     * Get the display name of this theme.
     */
    virtual QString name() const = 0;
    
    /**
     * Get the unique identifier for this theme.
     */
    virtual QString id() const = 0;
    
    /**
     * Get the stylesheet for this theme.
     */
    virtual QString stylesheet() const = 0;
    
    /**
     * Optional: Get a description of the theme.
     */
    virtual QString description() const { return QString(); }
};

