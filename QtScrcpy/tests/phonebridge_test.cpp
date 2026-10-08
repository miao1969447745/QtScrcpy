#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QTimer>
#include <QDebug>
#include <QPushButton>
#include <cstdio>
#include "pluginbridge.h"
#include "phonefeatures.h"
#include "videoform.h"
#include "toolform.h"
#include "longscreenshot.h"

class PhoneBridgeTests
{
public:
    static int run()
    {
        int failures = 0;
        const auto check = [&failures](bool value, const char *message) { if (!value) { std::fprintf(stderr, "%s\n", message); ++failures; } };
        QTemporaryDir temporary;
        qputenv("QTSCRCPY_BRIDGE_FILE", (temporary.path() + "/bridge.json").toUtf8());
        qputenv("QTSCRCPY_CONFIG_PATH", (temporary.path() + "/config").toUtf8());
        // Reuse this test executable as a harmless fake ADB child. Never use real ADB.
        qputenv("QTSCRCPY_ADB_PATH", QCoreApplication::applicationFilePath().toUtf8());
        PluginBridge bridge;
        check(bridge.isListening(), "isolated bridge listening");
        QFile discoveryFile(bridge.discoveryFile());
        check(discoveryFile.open(QIODevice::ReadOnly), "isolated discovery file");
        const QJsonObject discovery = QJsonDocument::fromJson(discoveryFile.readAll()).object();
        discoveryFile.close();
        const auto request = [&discovery](QJsonObject object) {
            object.insert("token", object.contains("token") ? object.value("token") : discovery.value("token"));
            QTcpSocket socket;
            QEventLoop loop;
            QByteArray bytes;
            QObject::connect(&socket, &QTcpSocket::connected, &loop, [&socket, object]() {
                socket.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
            });
            QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&socket, &bytes, &loop]() {
                bytes += socket.readAll(); if (bytes.contains('\n')) loop.quit();
            });
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            socket.connectToHost("127.0.0.1", static_cast<quint16>(discovery.value("port").toInt()));
            loop.exec();
            return QJsonDocument::fromJson(bytes.trimmed()).object();
        };
        check(request({{"op", "ping"}, {"token", "wrong-token"}}).value("ok") == QJsonValue(false), "authentication rejects wrong token");
        check(request({{"op", "ping"}}).value("feature_api").toInt() == 1, "feature negotiation");
        check(request({{"op", "capabilities"}}).value("catalog").toObject().value("actions").toObject().size() == 50, "all 50 action definitions discoverable");
        QJsonObject parameters, definition;
        QString error;
        check(PhoneFeatures::prepare("long_screenshot", parameters, definition, error) && parameters.value("max_frames").toInt() == 99, "longshot default 99");
        parameters = {{"options", QJsonObject{{"max_fps", true}}}};
        check(!PhoneFeatures::prepare("service_start", parameters, definition, error), "nested boolean is not an integer");
        parameters = {{"options", QJsonObject{{"shell", "whoami"}}}};
        check(!PhoneFeatures::prepare("service_start", parameters, definition, error), "unknown nested server option rejected");
        const QString owner = "offline-owner-012345";
        QJsonObject action{{"op", "qt_action"}, {"action", "service_start"}, {"serial", "OFFLINE-TEST"},
                           {"session_id", owner}, {"parameters", QJsonObject()}};
        check(request(action).value("ok") == QJsonValue(false), "missing specific authorization rejected");
        action.insert("confirmed", true);
        check(request(action).value("ok") == QJsonValue(false), "missing GUI provider rejected");
        bridge.setActionHandler([](const QString &action, const QString &serial, const QJsonObject &) {
            return QJsonObject{{"ok", true}, {"serial", serial}, {"available", action == "service_status"}};
        });
        check(request(action).value("serial") == "OFFLINE-TEST", "serial preserved through handler");
        VideoForm form(false, false, false);
        ToolForm toolbar(&form, ToolForm::AP_OUTSIDE_RIGHT);
        const auto longScreenshotButton = toolbar.findChild<QPushButton *>("longScreenshotBtn");
        check(longScreenshotButton && longScreenshotButton->toolTip() == QString::fromUtf8("电脑兼容拼接长截图（完成后复制到剪贴板）"),
              "long screenshot tooltip is Chinese without requiring a translator");
        form.setSerial("OFFLINE-TEST");
        bridge.registerDevice("OFFLINE-TEST", "Synthetic only", &form);
        action.insert("action", "long_screenshot");
        check(request(action).value("ok") == QJsonValue(false), "missing capture guard rejected");
        action.insert("capture_epoch", static_cast<qint64>(form.pluginCaptureEpoch()));
        action.insert("width", form.frameSize().width()); action.insert("height", form.frameSize().height());
        auto longshot = request(action);
        const QJsonObject job = longshot.value("job").toObject();
        check(job.value("state") == "failed", "unready image becomes failed task, not false success");
        const QString id = job.value("job_id").toString();
        action.insert("action", "job_status"); action.insert("parameters", QJsonObject{{"job_id", id}});
        check(request(action).value("job").toObject().value("state") == "failed", "job owner can read terminal state");
        action.insert("session_id", "another-owner-012345");
        check(request(action).value("ok") == QJsonValue(false), "another owner cannot read task");
        action.insert("session_id", owner); action.insert("action", "push_file");
        action.insert("parameters", QJsonObject{{"local_path", "relative.png"}, {"remote_path", "/sdcard/Download/a.png"}});
        check(request(action).value("ok") == QJsonValue(false), "relative desktop file rejected before ADB");
        action.insert("action", "android_key"); action.insert("parameters", QJsonObject{{"keycode", 26}});
        action.insert("capture_epoch", static_cast<qint64>(form.pluginCaptureEpoch() + 1));
        check(request(action).value("ok") == QJsonValue(false), "stale capture rejected before native dispatch");

        int cancellations = 0;
        const QString running = bridge.newJob("OFFLINE-TEST", owner, [&cancellations]() { ++cancellations; });
        QJsonObject cancel{{"op", "qt_action"}, {"action", "job_cancel"}, {"session_id", "another-owner-12345"},
                           {"parameters", QJsonObject{{"job_id", running}}}};
        check(request(cancel).value("ok") == QJsonValue(false) && cancellations == 0, "foreign owner cannot cancel active task");
        cancel.insert("session_id", owner);
        check(request(cancel).value("job").toObject().value("state") == "cancelled" && cancellations == 1, "owned active task actually cancelled");
        request(cancel);
        bridge.finishJob(running, {{"state", "completed"}});
        check(cancellations == 1 && bridge.m_jobs.value(running).result.value("state") == "cancelled", "cancel is idempotent and late completion cannot overwrite it");
        QJsonObject target{{"action", "power"}, {"serial", "OFFLINE-TEST"}, {"parameters", QJsonObject()},
                           {"capture_epoch", static_cast<qint64>(form.pluginCaptureEpoch())},
                           {"width", form.frameSize().width()}, {"height", form.frameSize().height()}};
        QJsonObject missingTarget = target; missingTarget.insert("serial", "ABSENT-TEST");
        check(request({{"op", "qt_batch"}, {"session_id", owner}, {"requests", QJsonArray{target, missingTarget}}}).value("ok") == QJsonValue(false),
              "batch prevalidation rejects missing target before any native dispatch");

        const auto adbQuery = [&](const QString &name) {
            return request({{"op", "qt_action"}, {"action", name}, {"serial", "OFFLINE-TEST"},
                            {"session_id", owner}, {"parameters", QJsonObject()}}).value("job").toObject();
        };
        const auto awaitTask = [&](const QString &task) {
            QJsonObject result;
            for (int attempts = 0; attempts < 100; ++attempts) {
                result = request({{"op", "qt_action"}, {"action", "job_status"}, {"session_id", owner},
                                  {"parameters", QJsonObject{{"job_id", task}}}}).value("job").toObject();
                if (result.value("state") != "running") break;
                QEventLoop pause; QTimer::singleShot(10, &pause, &QEventLoop::quit); pause.exec();
            }
            return result;
        };
        const auto succeeded = awaitTask(adbQuery("query_device").value("job_id").toString());
        check(succeeded.value("state") == "completed" && succeeded.value("stdout").toString().contains("synthetic-adb"), "fake child exit zero produces completed task");
        check(awaitTask(adbQuery("query_apps").value("job_id").toString()).value("state") == "failed", "fake child exit failure cannot report completed");
        const QString waiting = adbQuery("query_cameras").value("job_id").toString();
        cancel.insert("parameters", QJsonObject{{"job_id", waiting}});
        check(request(cancel).value("job").toObject().value("state") == "cancelled", "running fake child process can be cancelled without touching real ADB");

        // Synthetic scrolling page: fixed red header, green footer, nonrepeating middle.
        const int width = 200, height = 800, top = 80, bottom = 64, viewport = height - top - bottom;
        QImage content(width, 3000, QImage::Format_RGB32);
        for (int y = 0; y < content.height(); ++y) for (int x = 0; x < width; ++x) {
            const quint32 hash = static_cast<quint32>((y / 3 + 1) * 2654435761U) ^ static_cast<quint32>((x / 4 + 1) * 2246822519U);
            content.setPixel(x, y, qRgb((hash >> 16) & 255, (hash >> 8) & 255, hash & 255));
        }
        QVector<QImage> frames;
        for (int i = 0; i < 5; ++i) {
            QImage frame(width, height, QImage::Format_RGB32);
            frame.fill(Qt::red);
            QPainter painter(&frame);
            painter.drawImage(QRect(0, top, width, viewport), content, QRect(0, i * 230, width, viewport));
            painter.fillRect(0, height - bottom, width, bottom, Qt::green);
            painter.end(); frames.append(frame);
        }
        const auto stitched = LongScreenshotController::stitchFrames(frames, 35, true);
        check(!stitched.image.isNull() && stitched.image.height() > 1500, "scrolling middle is retained in long output");
        int redRows = 0, greenRows = 0;
        for (int y = 0; y < stitched.image.height(); ++y) {
            if (stitched.image.pixel(0, y) == qRgb(255, 0, 0)) ++redRows;
            if (stitched.image.pixel(0, y) == qRgb(0, 255, 0)) ++greenRows;
        }
        check(redRows == top && greenRows == bottom, "fixed top and bottom included exactly once");
        check(LongScreenshotController::stitchFrames({}, 35, true).image.isNull(), "empty capture cannot succeed");
        check(LongScreenshotController::stitchFrames({frames.first()}, 35, true).image == frames.first(), "single screen is preserved");
        qInfo() << "Phone feature bridge and synthetic stitching:" << (failures ? "FAILED" : "PASSED");
        return failures ? 1 : 0;
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "-s") {
        // Fixed test identities only, no shell, device or file access.
        if (argc < 3 || QString::fromLocal8Bit(argv[2]) != "OFFLINE-TEST") return 9;
        const QString last = QString::fromLocal8Bit(argv[argc - 1]);
        if (last == "packages") return 7;
        if (last == "media.camera") {
            QCoreApplication helper(argc, argv);
            QTimer::singleShot(30000, &helper, &QCoreApplication::quit);
            return helper.exec();
        }
        std::fputs("synthetic-adb: no phone accessed\n", stdout);
        return 0;
    }
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("QtScrcpyOfflineFeatureTests");
    return PhoneBridgeTests::run();
}
