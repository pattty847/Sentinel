// Sentinel — StockChartDock
#include "StockChartDock.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QButtonGroup>
#include <QSurfaceFormat>

// ── Helpers ───────────────────────────────────────────────────────────────────

static QToolButton* makePeriodBtn(const QString& label, QWidget* parent) {
    auto* btn = new QToolButton(parent);
    btn->setText(label);
    btn->setCheckable(true);
    btn->setFixedHeight(22);
    btn->setStyleSheet(
        "QToolButton { background:#1a2028; color:#6a8090; border:1px solid #253040;"
        " border-radius:3px; padding:0 6px; font-size:11px; }"
        "QToolButton:checked { background:#1e3a50; color:#60c0e0; border-color:#2a6080; }"
        "QToolButton:hover { color:#c0d0dc; }");
    return btn;
}

// ── Construction ─────────────────────────────────────────────────────────────

StockChartDock::StockChartDock(QWidget* parent)
    : DockablePanel("StockChartDock", "Stock Chart", parent)
    , m_periodGroup(new QButtonGroup(this))
    , m_process(new QProcess(this))
{
    connect(m_process, &QProcess::finished,        this, &StockChartDock::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,   this, &StockChartDock::onProcessError);

    buildUi();
}

StockChartDock::~StockChartDock() {
    if (m_process->state() != QProcess::NotRunning)
        m_process->kill();
}

// ── UI ────────────────────────────────────────────────────────────────────────

void StockChartDock::buildUi() {
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->setSpacing(4);

    // ── Toolbar ───────────────────────────────────────────────────────────────
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);

    m_tickerInput = new QLineEdit(m_contentWidget);
    m_tickerInput->setPlaceholderText("Ticker…");
    m_tickerInput->setFixedWidth(80);
    m_tickerInput->setMaxLength(10);
    m_tickerInput->setStyleSheet(
        "QLineEdit { background:#141a20; color:#c0ccd8; border:1px solid #253040;"
        " border-radius:3px; padding:2px 6px; font-size:12px; }");
    toolbar->addWidget(m_tickerInput);

    // Period buttons
    const QStringList periods = {"1y", "2y", "5y", "10y", "max"};
    for (const auto& p : periods) {
        auto* btn = makePeriodBtn(p, m_contentWidget);
        m_periodGroup->addButton(btn);
        toolbar->addWidget(btn);
        if (p == m_currentPeriod) btn->setChecked(true);
        connect(btn, &QToolButton::clicked, this, [this, p]{ onPeriodChanged(p); });
    }

    toolbar->addStretch();

    m_fetchBtn = new QToolButton(m_contentWidget);
    m_fetchBtn->setIcon(QIcon(":/svg/refresh.svg"));
    m_fetchBtn->setToolTip("Fetch candles");
    m_fetchBtn->setFixedSize(26, 26);
    toolbar->addWidget(m_fetchBtn);

    layout->addLayout(toolbar);

    // ── QML chart ─────────────────────────────────────────────────────────────
    m_quickView = new QQuickView;
    m_quickView->setPersistentSceneGraph(true);
    m_quickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_quickView->setColor(Qt::black);
    m_quickView->setFormat(QSurfaceFormat::defaultFormat());
    m_quickView->engine()->addImportPath("qrc:/qt/qml");

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString modPath = QDir(appDir).absoluteFilePath("../../libs/gui");
    if (QFile::exists(QDir(modPath).filePath("qmldir")))
        m_quickView->engine()->addImportPath(modPath);

    // Try QRC first, fall back to source dir in dev builds
    if (QFile::exists(":/Sentinel/Charts/StockChartView.qml"))
        m_quickView->setSource(QUrl("qrc:/Sentinel/Charts/StockChartView.qml"));
    else if (QFile::exists(":/qt/qml/Sentinel/Charts/StockChartView.qml"))
        m_quickView->setSource(QUrl("qrc:/qt/qml/Sentinel/Charts/StockChartView.qml"));
    else {
#ifdef SENTINEL_SOURCE_DIR
        const QString local = QDir(QString::fromUtf8(SENTINEL_SOURCE_DIR)).filePath("libs/gui/qml/StockChartView.qml");
#else
        const QString local = QDir::current().filePath("libs/gui/qml/StockChartView.qml");
#endif
        m_quickView->setSource(QUrl::fromLocalFile(local));
    }

    m_qmlContainer = QWidget::createWindowContainer(m_quickView, m_contentWidget);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(m_qmlContainer, 1);

    m_contentWidget->setLayout(layout);

    // Signals
    connect(m_fetchBtn,    &QToolButton::clicked, this, &StockChartDock::onFetchClicked);
    connect(m_tickerInput, &QLineEdit::returnPressed, this, &StockChartDock::onFetchClicked);
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool v) {
        if (m_qmlContainer) m_qmlContainer->setVisible(v);
        if (m_quickView)    m_quickView->setVisible(v);
    });
}

// ── Public API ────────────────────────────────────────────────────────────────

void StockChartDock::loadSymbol(const QString& ticker, const QString& companyName) {
    m_currentTicker  = ticker.toUpper().trimmed();
    m_currentCompany = companyName;
    m_tickerInput->setText(m_currentTicker);

    if (auto* root = qmlRoot()) {
        root->setProperty("ticker",  m_currentTicker);
        root->setProperty("company", m_currentCompany);
    }

    startFetch();
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void StockChartDock::onFetchClicked() {
    const QString t = m_tickerInput->text().trimmed().toUpper();
    if (t.isEmpty()) return;
    m_currentTicker = t;
    if (auto* root = qmlRoot())
        root->setProperty("ticker", m_currentTicker);
    startFetch();
}

void StockChartDock::onPeriodChanged(const QString& period) {
    m_currentPeriod = period;
    if (auto* root = qmlRoot())
        root->setProperty("period", period);
    if (!m_currentTicker.isEmpty())
        startFetch();
}

void StockChartDock::onProcessFinished(int exitCode, QProcess::ExitStatus /*status*/) {
    const QString output = QString::fromUtf8(m_process->readAllStandardOutput());

    if (auto* root = qmlRoot()) {
        root->setProperty("loading", false);
    }

    // Find our marker line
    const int idx = output.indexOf("OHLCV_DATA:");
    const int errIdx = output.indexOf("ERROR_DATA:");

    if (idx != -1) {
        const QByteArray json = output.mid(idx + 11).trimmed().toUtf8();
        const QJsonObject obj = QJsonDocument::fromJson(json).object();
        const QJsonArray candles = obj["candles"].toArray();
        const int count = candles.size();

        // Convert to QVariantList for QML setCandles()
        QVariantList list;
        list.reserve(count);
        for (const QJsonValue& v : candles) {
            const QJsonObject c = v.toObject();
            QVariantMap m;
            m["date"]      = c["date"].toString();
            m["timestamp"] = static_cast<qint64>(c["ts_ms"].toDouble());
            m["open"]      = c["open"].toDouble();
            m["high"]      = c["high"].toDouble();
            m["low"]       = c["low"].toDouble();
            m["close"]     = c["close"].toDouble();
            m["volume"]    = c["volume"].toDouble();
            list.append(m);
        }

        if (auto* root = qmlRoot()) {
            QMetaObject::invokeMethod(root, "setCandles", Q_ARG(QVariant, QVariant::fromValue(list)));
            root->setProperty("statusMsg", "");  // Clear; ticker/period/candles live in header only
        }
    } else if (errIdx != -1) {
        const QByteArray json = output.mid(errIdx + 11).trimmed().toUtf8();
        const QJsonObject obj = QJsonDocument::fromJson(json).object();
        setStatus(obj["error"].toString(), true);
    } else {
        setStatus(exitCode == 0 ? "No data returned" : "Fetch failed", true);
    }
}

void StockChartDock::onProcessError(QProcess::ProcessError error) {
    if (auto* root = qmlRoot()) root->setProperty("loading", false);
    const QString msg = (error == QProcess::FailedToStart)
        ? "Could not start Python. Is uv in PATH?"
        : "Process error: " + m_process->errorString();
    setStatus(msg, true);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void StockChartDock::startFetch() {
    if (m_currentTicker.isEmpty()) return;
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(500);
    }

    // Locate the script relative to the app or source dir.
    // Binary is at build/windows-msvc-vs/apps/sentinel-gui/Debug/ — 5 levels up = repo root.
    QString scriptPath;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).absoluteFilePath("../../../../../scripts/stocks/fetch_daily_ohlcv.py"), // Debug build (5 up)
        QDir(appDir).absoluteFilePath("../../../../scripts/stocks/fetch_daily_ohlcv.py"),    // Release/flat (4 up)
        QDir(appDir).absoluteFilePath("../../../scripts/stocks/fetch_daily_ohlcv.py"),       // 3 up fallback
#ifdef SENTINEL_SOURCE_DIR
        QDir(QString::fromUtf8(SENTINEL_SOURCE_DIR)).filePath("scripts/stocks/fetch_daily_ohlcv.py"),
#endif
    };
    for (const auto& c : candidates) {
        if (QFile::exists(c)) { scriptPath = c; break; }
    }

    if (scriptPath.isEmpty()) {
        setStatus("Script not found: scripts/stocks/fetch_daily_ohlcv.py", true);
        return;
    }

    setStatus(QString("Fetching %1 %2…").arg(m_currentTicker, m_currentPeriod));
    if (auto* root = qmlRoot()) {
        root->setProperty("loading",   true);
        root->setProperty("statusMsg", QString("Loading %1…").arg(m_currentTicker));
    }

    // Run via uv so the venv is activated automatically.
    // scriptPath = .../scripts/stocks/fetch_daily_ohlcv.py
    // absolutePath() = .../scripts/stocks/ → one level up = scripts/ where pyproject.toml lives
    const QString scriptsDir = QFileInfo(scriptPath).absolutePath() + "/..";
    m_process->setWorkingDirectory(QDir(scriptsDir).absolutePath());
    m_process->start("uv", {"run", "python", scriptPath, m_currentTicker, m_currentPeriod});
}

QObject* StockChartDock::qmlRoot() const {
    return m_quickView ? m_quickView->rootObject() : nullptr;
}

void StockChartDock::setStatus(const QString& msg, bool error) {
    if (auto* root = qmlRoot())
        root->setProperty("statusMsg", msg);
    Q_UNUSED(error);  // QML empty state shows statusMsg; styling can be extended there if needed
}
