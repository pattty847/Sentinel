/*
Sentinel — ChartModeController
Role: QML-compatible controller for primary field selection and overlay toggles.
Threading: Lives and operates on the main GUI thread.
Integration: Exposed to QML; its properties gate layer visibility.
Related: ChartModeController.cpp, MainWindowGpu.h.
*/
#pragma once
#include <QObject>
class ChartModeController : public QObject {
    Q_OBJECT
    Q_PROPERTY(int primaryField READ primaryField NOTIFY primaryFieldChanged)
    Q_PROPERTY(bool candlesEnabled READ candlesEnabled NOTIFY candlesEnabledChanged)
public:
    enum class PrimaryField {
        Heatmap = 0,
        FootprintCells = 1,
        TpoProfile = 2
    };
    Q_ENUM(PrimaryField)

    explicit ChartModeController(QObject* parent = nullptr) : QObject(parent) {}

    int primaryField() const;
    bool candlesEnabled() const { return m_candlesEnabled; }

    void setPrimaryField(PrimaryField field);
    Q_INVOKABLE void setPrimaryField(int field);
    Q_INVOKABLE void setCandlesEnabled(bool enabled);

signals:
    void primaryFieldChanged(int field);
    void candlesEnabledChanged(bool enabled);

private:
    PrimaryField m_primaryField{PrimaryField::Heatmap};
    bool m_candlesEnabled = true;
};

