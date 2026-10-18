#include "CandleWireFixture.hpp"
#include <QScopeGuard>
#include "CandleWireMeasurement.hpp"
#include <QTimer>
#include <random>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().contains("--measure"))
            return candle_measurement::run(app, !app.arguments().contains("--without-100"));
        CandleFixture fixture(true, 17200);
        fixture.start();
        std::cout << Json{{"port", 17200}, {"cert", fixture.config.tls.certFile}}.dump() << std::endl;
        std::mt19937 random(42);
        double price = 65000;
        int64_t tick = 0;
        const int64_t epoch = QDateTime::currentMSecsSinceEpoch();
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            const int64_t now = epoch + tick * 50;
            if (tick % 300 < 260) {
                price += (static_cast<int>(random() % 3) - 1) * .25;
                tradeAt(*fixture.model, now, "BTC-USD", price, .01);
            }
            if (tick % 20 == 0) fixture.model->onFeedWatermark("BTC-USD", now);
            ++tick;
        });
        timer.start(50);
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << "candle fixture failed: " << e.what() << '\n';
        return 1;
    }
}
