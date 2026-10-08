#include <QApplication>
#include <QDebug>
#include "QtScrcpyCore.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    auto &manager = qsc::IDeviceManage::getInstance();
    qsc::DeviceParams first;
    first.serial = "offline-cancel-test-A";
    first.display = false;
    first.recordFile = true;
    auto second = first;
    second.serial = "offline-cancel-test-B";
    // Do NOT pump the event loop. Device::connectDevice starts its ADB work
    // via a zero-timer. Cancelling before that timer runs touches no phone.
    for (int i = 0; i < 3; ++i) {
        if (!manager.connectDevice(first) || manager.connectDevice(first)) return 1;
        if (!manager.connectDevice(second)) return 2;
        if (!manager.disconnectDevice(first.serial) || manager.getDevice(first.serial)) return 3;
        if (!manager.getDevice(second.serial)) return 4;
        if (!manager.connectDevice(first)) return 5;
        manager.disconnectAllDevice();
        if (manager.getDevice(first.serial) || manager.getDevice(second.serial)) return 6;
    }
    qInfo() << "Core startup cancellation and restart: PASSED (no phone operations)";
    return 0;
}
