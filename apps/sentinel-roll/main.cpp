#include "roller/Roller.hpp"
#include <QCoreApplication>
#include "SentinelLogSink.hpp"
int main(int argc,char** argv) {
    sentinel::logging::installLogSink("sentinel-roll",argc,argv);
    QCoreApplication app(argc,argv);
    return sentinel::roller::rollMain(argc,argv);
}
