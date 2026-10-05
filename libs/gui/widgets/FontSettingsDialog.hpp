#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLabel>

class FontSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit FontSettingsDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void refreshFonts();

    QComboBox* m_fontCombo = nullptr;
    QLabel* m_previewLabel = nullptr;
};
