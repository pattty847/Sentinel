#include "TpoStyleEditor.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
// Which properties an element uses; the others are shown disabled.
struct ElementSpec {
    const char* key;
    const char* label;
    tpo::Element tpo::StyleModel::* field;
    bool color, opacity, width;
    const char* widthSuffix;
    double widthMax;
    const char* help;
};

constexpr std::array<ElementSpec, TpoStyleEditor::kElements> kSpecs{{
    {"cells", "Cells", &tpo::StyleModel::cells, false, true, true, " px gap", 4,
     "Cell fill (colour from the theme). Opacity lets candles show through; width is the gap between cells."},
    {"letters", "Letters", &tpo::StyleModel::letters, true, true, true, " px", 20,
     "Period letters. Off draws blocks. Width is the largest letter size."},
    {"outsideVa", "Outside value area", &tpo::StyleModel::outsideVa, false, true, false, "", 0,
     "Dim the rows outside the 70% value area. Opacity is the brightness they keep."},
    {"valueArea", "Value area band", &tpo::StyleModel::valueArea, true, true, false, "", 0,
     "A band behind the value-area rows, across the level-line reach."},
    {"poc", "POC", &tpo::StyleModel::poc, true, true, true, " px", 8,
     "Point of control: the colour of the POC row and its line."},
    {"vah", "VAH", &tpo::StyleModel::vah, true, true, true, " px", 8, "Value-area high line."},
    {"val", "VAL", &tpo::StyleModel::val, true, true, true, " px", 8, "Value-area low line."},
    {"ibh", "IBH", &tpo::StyleModel::ibh, true, true, true, " px", 8, "Initial-balance high: the first hour's high."},
    {"ibl", "IBL", &tpo::StyleModel::ibl, true, true, true, " px", 8, "Initial-balance low: the first hour's low."},
    {"tags", "Level tags", &tpo::StyleModel::tags, true, true, true, " px", 20,
     "Tags at the plot's right edge for the newest session. Colour is the text; opacity is the chip."},
}};

const std::array<std::pair<const char*, const char*>, 4> kPresetItems{{
    {"Letters", "letters"}, {"Blocks", "blocks"}, {"Extended levels", "extended"}, {"Indigo", "indigo"}}};
} // namespace

TpoStyleEditor::TpoStyleEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout();
    m_preset = new QComboBox(this);
    m_preset->setObjectName("tpoPreset");
    for (const auto& [label, key] : kPresetItems) m_preset->addItem(label, key);
    m_preset->addItem("Custom", "custom");
    m_preset->setToolTip("Pick a starting look. Editing any property below makes it Custom; the theme stays.");
    form->addRow("Preset", m_preset);
    m_reach = new QComboBox(this);
    m_reach->setObjectName("tpoStyle.reach");
    m_reach->addItem("Profile width", "profile");
    m_reach->addItem("Across the session", "session");
    m_reach->setToolTip("POC, VAH, VAL, IBH and IBL lines run across the profile or across the whole session.");
    form->addRow("Level lines", m_reach);
    for (QWidget* field : {static_cast<QWidget*>(m_preset), static_cast<QWidget*>(m_reach)})
        if (QWidget* label = form->labelForField(field)) label->setToolTip(field->toolTip());
    layout->addLayout(form);

    auto* grid = new QGridLayout();
    int column = 0;
    for (const char* header : {"Element", "Show", "Colour", "Opacity", "Width"})
        grid->addWidget(new QLabel(header, this), 0, column++);
    for (int i = 0; i < kElements; ++i) buildRow(i, grid);
    layout->addLayout(grid);

    connect(m_preset, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const QString key = m_preset->itemData(index).toString();
        if (key != "custom") emit presetChosen(key);
    });
    connect(m_reach, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { readControls(); });
    setStyleModel(tpo::presetStyle(tpo::Preset::Letters));
}

void TpoStyleEditor::buildRow(int index, QGridLayout* grid) {
    const ElementSpec& spec = kSpecs[static_cast<size_t>(index)];
    Row& row = m_rows[static_cast<size_t>(index)];
    const QString name = QStringLiteral("tpoStyle.") + spec.key;
    auto* label = new QLabel(spec.label, this);
    label->setToolTip(spec.help);
    row.visible = new QCheckBox(this);
    row.visible->setObjectName(name + ".visible");
    row.color = new QToolButton(this);
    row.color->setObjectName(name + ".color");
    row.color->setEnabled(spec.color);
    row.opacity = new QSpinBox(this);
    row.opacity->setObjectName(name + ".opacity");
    row.opacity->setRange(0, 100);
    row.opacity->setSuffix("%");
    row.opacity->setEnabled(spec.opacity);
    row.width = new QDoubleSpinBox(this);
    row.width->setObjectName(name + ".width");
    row.width->setRange(0.0, spec.widthMax);
    row.width->setDecimals(1);
    row.width->setSingleStep(1.0);
    row.width->setSuffix(spec.widthSuffix);
    row.width->setEnabled(spec.width);
    const QString help = QString::fromUtf8(spec.help);
    row.visible->setToolTip(QStringLiteral("Show %1. ").arg(spec.label) + help);
    row.color->setToolTip(QStringLiteral("Pick the %1 colour.").arg(spec.label));
    row.opacity->setToolTip(QStringLiteral("%1 opacity. ").arg(spec.label) + help);
    row.width->setToolTip(QStringLiteral("%1 width. ").arg(spec.label) + help);
    const int r = index + 1;
    grid->addWidget(label, r, 0);
    grid->addWidget(row.visible, r, 1);
    grid->addWidget(row.color, r, 2);
    grid->addWidget(row.opacity, r, 3);
    grid->addWidget(row.width, r, 4);

    connect(row.visible, &QCheckBox::toggled, this, [this] { readControls(); });
    connect(row.opacity, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { readControls(); });
    connect(row.width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { readControls(); });
    connect(row.color, &QToolButton::clicked, this, [this, index] {
        tpo::Element& element = m_style.*(kSpecs[static_cast<size_t>(index)].field);
        const QColor chosen = QColorDialog::getColor(QColor(element.color.r, element.color.g, element.color.b), this,
                                                     kSpecs[static_cast<size_t>(index)].label);
        if (!chosen.isValid()) return;
        element.color = {static_cast<uint8_t>(chosen.red()), static_cast<uint8_t>(chosen.green()),
                         static_cast<uint8_t>(chosen.blue()), 255};
        showColor(m_rows[static_cast<size_t>(index)].color, element.color);
        syncPresetCombo();
        emit styleEdited(m_style);
    });
}

void TpoStyleEditor::showColor(QToolButton* button, const tpo::Rgba& color) {
    const QString hex = QString::fromStdString(tpo::hexColor(color));
    button->setText(hex);
    button->setStyleSheet(QStringLiteral("QToolButton { border-left: 12px solid %1; padding-left: 4px; }").arg(hex));
}

void TpoStyleEditor::setStyleModel(const tpo::StyleModel& style) {
    m_loading = true;
    m_style = style;
    {
        const QSignalBlocker block(m_reach);
        m_reach->setCurrentIndex(style.reach == tpo::LineReach::Session ? 1 : 0);
    }
    for (int i = 0; i < kElements; ++i) {
        const tpo::Element& element = style.*(kSpecs[static_cast<size_t>(i)].field);
        const Row& row = m_rows[static_cast<size_t>(i)];
        const QSignalBlocker a(row.visible), b(row.opacity), c(row.width);
        row.visible->setChecked(element.visible);
        row.opacity->setValue(static_cast<int>(std::lround(element.opacity * 100.0)));
        row.width->setValue(element.width);
        showColor(row.color, element.color);
    }
    syncPresetCombo();
    m_loading = false;
}

void TpoStyleEditor::readControls() {
    if (m_loading) return;
    m_style.reach = m_reach->currentIndex() == 1 ? tpo::LineReach::Session : tpo::LineReach::Profile;
    for (int i = 0; i < kElements; ++i) {
        const ElementSpec& spec = kSpecs[static_cast<size_t>(i)];
        tpo::Element& element = m_style.*(spec.field);
        const Row& row = m_rows[static_cast<size_t>(i)];
        element.visible = row.visible->isChecked();
        if (spec.opacity) element.opacity = row.opacity->value() / 100.0;
        if (spec.width) element.width = row.width->value();
    }
    syncPresetCombo();
    emit styleEdited(m_style);
}

void TpoStyleEditor::syncPresetCombo() {
    tpo::Preset preset = tpo::Preset::Letters;
    const QString key = tpo::matchPreset(m_style, preset) ? QString::fromLatin1(tpo::presetName(preset))
                                                          : QStringLiteral("custom");
    const QSignalBlocker block(m_preset);
    m_preset->setCurrentIndex(std::max(0, m_preset->findData(key)));
}
