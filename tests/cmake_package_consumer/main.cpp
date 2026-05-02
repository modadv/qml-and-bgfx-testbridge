#include <QCoreApplication>

#include "testbridge/test_bridge.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    auto& bridge = testbridge::TestBridge::instance();
    bridge.stop();
    return 0;
}
