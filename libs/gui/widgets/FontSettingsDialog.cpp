#include "FontSettingsDialog.hpp"
#include "../themes/FontManager.hpp"
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QApplication>

FontSettingsDialog::FontSettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Font Settings");
    setModal(false);
    resize(320, 160);
    setStyleSheet("QDialog { background-color: #1B1F24; }");
    buildUi();
    refreshFonts();
}

void FontSettingsDialog::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);

    m_fontCombo = new QComboBox(this);
    m_fontCombo->setToolTip("Select UI font family");
    m_fontCombo->setStyleSheet(
        "QComboBox { background-color: #252A31; color: #E6EDF3; border: 1px solid #3A3F46; padding: 6px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background-color: #1C1F24; color: #E6EDF3; selection-background-color: #2B5A7A; }"
    );
    layout->addWidget(m_fontCombo);

    m_previewLabel = new QLabel("Preview: Sentinel UI", this);
    m_previewLabel->setStyleSheet("QLabel { padding: 6px; color: #E6EDF3; }");
    layout->addWidget(m_previewLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setStyleSheet(
        "QDialogButtonBox QPushButton { background-color: #2B5A7A; color: #FFFFFF; border: none; padding: 6px 12px; border-radius: 4px; }"
        "QDialogButtonBox QPushButton:hover { background-color: #3472A0; }"
    );
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    connect(m_fontCombo, &QComboBox::currentTextChanged, this, [this](const QString& family) {
        auto* app = qobject_cast<QApplication*>(QApplication::instance());
        if (!app) return;
        FontManager::instance().applyFontFamily(family, app);
        m_previewLabel->setFont(QFont(family, 11));
    });
}

void FontSettingsDialog::refreshFonts() {
    const QSignalBlocker blocker(m_fontCombo);
    m_fontCombo->clear();
    const auto fonts = FontManager::instance().availableFonts();
    m_fontCombo->addItems(fonts);

    const QString current = FontManager::instance().currentFontFamily();
    if (!current.isEmpty()) {
        const int index = m_fontCombo->findText(current);
        if (index >= 0) {
            m_fontCombo->setCurrentIndex(index);
        }
        m_previewLabel->setFont(QFont(current, 11));
    }
}
