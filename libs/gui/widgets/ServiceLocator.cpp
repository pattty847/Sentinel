#include "ServiceLocator.hpp"
#include "../datasources/IGridDataSource.hpp"
#include <QPointer>

IGridDataSource* ServiceLocator::s_dataSource = nullptr;

void ServiceLocator::registerDataSource(IGridDataSource* source) {
    s_dataSource = source;
}

IGridDataSource* ServiceLocator::dataSource() {
    return s_dataSource;
}

