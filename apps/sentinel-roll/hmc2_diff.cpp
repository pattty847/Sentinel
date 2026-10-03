#include "roller/Roller.hpp"
#include <QCoreApplication>
#include "SentinelLogSink.hpp"
int main(int argc,char** argv) {
    sentinel::logging::installLogSink("hmc2-diff",argc,argv);
    QCoreApplication app(argc,argv);
    return sentinel::roller::diffMain(argc,argv);
}
