// Mapping provider contract for render-frame time/price transforms.
#pragma once

#include "TimeAxisMapping.hpp"
#include <QtPlugin>

class ITimeAxisMappingProvider {
public:
    virtual ~ITimeAxisMappingProvider() = default;
    virtual TimeAxisMapping currentTimeAxisMapping() const = 0;
};

#define SENTINEL_ITIMEAXISMAPPINGPROVIDER_IID "io.sentinel.ITimeAxisMappingProvider/1.0"
Q_DECLARE_INTERFACE(ITimeAxisMappingProvider, SENTINEL_ITIMEAXISMAPPINGPROVIDER_IID)
