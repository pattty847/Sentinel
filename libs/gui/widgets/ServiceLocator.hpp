#pragma once
class IGridDataSource;
class ServiceLocator {
public:
    static void registerDataSource(IGridDataSource* source);
    static IGridDataSource* dataSource();
private:
    static IGridDataSource* s_dataSource;
};
