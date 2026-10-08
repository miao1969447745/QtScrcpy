#include <QApplication>
#include <QPushButton>
#include <QTranslator>
#include <QDebug>
#include "deviceservicelist.h"

static int failures = 0;
static void check(bool condition, const char *message)
{
    if (!condition) { qCritical() << message; ++failures; }
}

static QPushButton *button(DeviceServiceList &list, const QString &serial, const char *name)
{
    for (int i = 0; i < list.topLevelItemCount(); ++i) {
        auto *item = list.topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == serial)
            return list.itemWidget(item, 2)->findChild<QPushButton *>(name);
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTranslator translator;
    check(argc > 1 && translator.load(QString::fromLocal8Bit(argv[1])), "load Chinese translations");
    app.installTranslator(&translator);
    DeviceServiceList list;
    check(list.headerItem()->text(1) == QString::fromUtf8("服务状态"), "translated status header");
    QStringList starts, stops;
    QObject::connect(&list, &DeviceServiceList::startRequested, [&starts](const QString &s) { starts.append(s); });
    QObject::connect(&list, &DeviceServiceList::stopRequested, [&stops](const QString &s) { stops.append(s); });
    list.setDevices({"A", "B"});
    list.setDeviceName("A", "MEIZU 21");
    list.resize(480, 180);
    list.show(); // CTest runs with the offscreen platform, not a desktop window.
    app.processEvents();
    auto *startButton = button(list, "A", "startService");
    auto *stopButton = button(list, "A", "stopService");
    check(startButton->width() >= startButton->minimumSizeHint().width()
          && stopButton->width() >= stopButton->minimumSizeHint().width(), "row actions are not truncated");
    check(button(list, "A", "startService")->isEnabled(), "stopped can start");
    check(!button(list, "A", "stopService")->isEnabled(), "stopped cannot stop");
    button(list, "A", "startService")->click();
    const quint64 oldGeneration = list.generation("A");
    check(list.isCurrentStart("A", oldGeneration), "startup generation valid");
    list.requestStart("A");
    check(starts == QStringList{"A"}, "repeated start ignored");
    check(!button(list, "A", "startService")->isEnabled(), "starting cannot repeat start");
    check(button(list, "A", "stopService")->isEnabled(), "startup can cancel");
    list.setState("A", DeviceServiceList::Running);
    list.selectSerial("B");
    list.setDevices({"B", "A"});
    check(list.state("A") == DeviceServiceList::Running, "refresh preserves running state");
    check(list.currentItem()->data(0, Qt::UserRole).toString() == "B", "refresh preserves selection");
    button(list, "A", "stopService")->click();
    check(stops == QStringList{"A"}, "row action uses serial not selection or index");
    check(list.state("B") == DeviceServiceList::Stopped, "second phone unaffected");
    check(!button(list, "A", "startService")->isEnabled()
          && !button(list, "A", "stopService")->isEnabled(), "stopping disables both buttons");
    check(!list.isCurrentStart("A", oldGeneration), "stop invalidates old startup callback");
    list.setState("A", DeviceServiceList::Stopped);
    check(button(list, "A", "startService")->isEnabled(), "can restart after stop");
    list.requestStart("A");
    list.setState("A", DeviceServiceList::Failed);
    check(button(list, "A", "startService")->isEnabled(), "failed startup can retry");
    list.requestStart("A");
    check(!list.isCurrentStart("A", oldGeneration), "retry does not resurrect old callback");
    list.requestStart("B");
    list.requestStopAll();
    check(list.state("A") == DeviceServiceList::Stopping && list.state("B") == DeviceServiceList::Stopping,
          "stop all includes pending startups");
    list.setState("A", DeviceServiceList::Running);
    list.setState("B", DeviceServiceList::Stopped);
    list.setDevices({"B"});
    check(button(list, "A", "stopService") != nullptr, "offline active session remains stoppable");
    check(!button(list, "A", "startService")->isEnabled(), "offline cannot start");
    list.setState("A", DeviceServiceList::Stopped);
    const int previousStarts = starts.size();
    list.requestStart("A");
    check(starts.size() == previousStarts, "offline request rejected");
    list.setDevices({"A", "B"});
    auto *item = list.topLevelItem(0);
    check(QMetaObject::invokeMethod(&list, "itemDoubleClicked", Qt::DirectConnection,
          Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 0)), "emit device-name double click");
    check(list.state("A") == DeviceServiceList::Starting, "double click shares start action");
    check(starts.last() == "A", "double click targets name serial");
    qInfo() << "Device service list checks:" << (failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
