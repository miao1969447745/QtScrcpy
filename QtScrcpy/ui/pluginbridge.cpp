#include "pluginbridge.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QTimer>
#include <QDateTime>
#include <cmath>

#include "videoform.h"
#include "longscreenshot.h"
#include "phonefeatures.h"
#include "adbprocess.h"
#include <QApplication>
#include <QClipboard>
#include <memory>
#include <limits>

namespace {
constexpr int kProtocolVersion = 1;
constexpr int kMaxRequestBytes = 128 * 1024;

bool exactInteger(const QJsonValue &value, qint64 expected)
{
    return value.isDouble() && value.toDouble() == static_cast<double>(expected);
}

QString bridgeFilePath()
{
    const QString explicitPath = qEnvironmentVariable("QTSCRCPY_BRIDGE_FILE");
    if (!explicitPath.isEmpty()) return explicitPath;
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    return QDir(base).filePath(QStringLiteral("QtScrcpy/plugin-bridge.json"));
}

bool isPrintableAscii(const QString &text)
{
    if (text.isEmpty() || text.toUtf8().size() > 300) {
        return false;
    }
    for (const QChar c : text) {
        const ushort value = c.unicode();
        if (value < 32 || value > 126) {
            return false;
        }
    }
    return true;
}
}

PluginBridge::PluginBridge(QObject *parent)
    : QObject(parent),
      m_server(new QTcpServer(this)),
      m_token(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_discoveryFile(bridgeFilePath())
{
    connect(m_server, &QTcpServer::newConnection, this, &PluginBridge::onNewConnection);
    auto *leases = new QTimer(this);
    connect(leases, &QTimer::timeout, this, [this]() {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->result.value("state").toString() != "running" && now - it->created > 600000) it = m_jobs.erase(it);
            else ++it;
        }
        for (DeviceEntry &entry : m_devices) {
            if (!entry.touchOwner.isEmpty() && now >= entry.touchDeadline) {
                if (entry.form) {
                    const QSize size = entry.form->frameSize();
                    entry.form->injectPluginTouch(QStringLiteral("up"),
                        qBound(0, entry.touchPosition.x(), qMax(0, size.width() - 1)),
                        qBound(0, entry.touchPosition.y(), qMax(0, size.height() - 1)));
                }
                entry.touchOwner.clear();
            }
        }
    });
    leases->start(250);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qWarning() << "PluginBridge: cannot listen:" << m_server->errorString();
        return;
    }
    if (!writeDiscoveryFile()) {
        qWarning() << "PluginBridge: cannot write discovery file:" << m_discoveryFile;
        m_server->close();
    }
}

PluginBridge::~PluginBridge()
{
    const auto jobs = m_jobs;
    for (const Job &job : jobs) if (job.cancel && job.result.value("state").toString() == "running") job.cancel();
    removeDiscoveryFile();
}

bool PluginBridge::isListening() const
{
    return m_server && m_server->isListening();
}

QString PluginBridge::discoveryFile() const
{
    return m_discoveryFile;
}

void PluginBridge::registerDevice(const QString &serial, const QString &deviceName, VideoForm *form)
{
    if (serial.isEmpty() || !form) {
        return;
    }
    DeviceEntry &entry = m_devices[serial];
    entry.name = deviceName.trimmed();
    entry.form = form;
}

void PluginBridge::unregisterDevice(const QString &serial)
{
    m_devices.remove(serial);
}

void PluginBridge::updateDeviceName(const QString &serial, const QString &deviceName)
{
    auto it = m_devices.find(serial);
    if (it != m_devices.end() && !deviceName.trimmed().isEmpty()) {
        it->name = deviceName.trimmed();
    }
}

void PluginBridge::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        if (m_buffers.size() >= 32) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_buffers.insert(socket, QByteArray());
        QTimer::singleShot(5000, socket, [socket]() { socket->disconnectFromHost(); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void PluginBridge::onReadyRead(QTcpSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    if (buffer.size() > kMaxRequestBytes) {
        reply(socket, error(QStringLiteral("Request is too large.")));
        return;
    }
    const int newline = buffer.indexOf('\n');
    if (newline < 0) {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(buffer.left(newline), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reply(socket, error(QStringLiteral("Invalid JSON request.")));
        return;
    }
    reply(socket, dispatch(document.object()));
}

QJsonObject PluginBridge::dispatch(const QJsonObject &request)
{
    if (request.value(QStringLiteral("token")).toString() != m_token) {
        return error(QStringLiteral("Authentication failed."));
    }

    const QString operation = request.value(QStringLiteral("op")).toString();
    if (operation == QStringLiteral("ping")) {
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("protocol"), kProtocolVersion},
                {QStringLiteral("server_version"), QStringLiteral("4.1")},
                {QStringLiteral("application"), QStringLiteral("QtScrcpy")},
                {QStringLiteral("feature_api"), 1},
                {QStringLiteral("application_version"), QCoreApplication::applicationVersion()}};
    }
    if (operation == "capabilities") return {{"ok", true}, {"catalog", PhoneFeatures::catalog()}};
    if (operation == "qt_batch") {
        if (!request.value("requests").isArray()) return error("Expected explicit target requests.");
        const QJsonArray requests = request.value("requests").toArray();
        if (requests.isEmpty() || requests.size() > 20) return error("Batch requires 1..20 explicit targets.");
        QSet<QString> targets;
        for (const QJsonValue &value : requests) {
            if (!value.isObject()) return error("Invalid target request.");
            const QJsonObject child = value.toObject();
            const QString serial = child.value("serial").toString();
            auto form = m_devices.value(serial).form;
            QJsonObject parameters = child.value("parameters").toObject(), definition;
            QString reason;
            if (!PhoneFeatures::prepare(child.value("action").toString(), parameters, definition, reason)
                    || definition.value("kind") != "native" || targets.contains(serial) || !form
                    || !child.value("parameters").isObject()
                    || !m_devices.value(serial).touchOwner.isEmpty() || form->longScreenshotController()->isActive()
                    || !exactInteger(child.value("capture_epoch"), form->pluginCaptureEpoch())
                    || !exactInteger(child.value("width"), form->frameSize().width())
                    || !exactInteger(child.value("height"), form->frameSize().height())
                    || (definition.value("confirmed").toBool() && request.value("confirmed") != QJsonValue(true)))
                return error("Batch target/action/observation is invalid; nothing was dispatched.");
            targets.insert(serial);
        }
        QJsonArray results;
        for (const QJsonValue &value : requests) {
            QJsonObject child = value.toObject();
            child.insert("session_id", request.value("session_id")); child.insert("confirmed", request.value("confirmed"));
            const QJsonObject result = dispatchPhoneAction(child);
            results.append(result);
            if (result.value("ok") != QJsonValue(true)) return {{"ok", true}, {"partial", true}, {"results", results}, {"action_may_have_executed", true}};
        }
        return {{"ok", true}, {"results", results}, {"delivery", "sent_to_explicit_targets; not an atomic transaction or UI verification"}};
    }
    if (operation == "qt_action") return dispatchPhoneAction(request);
    if (operation == QStringLiteral("list")) {
        QJsonArray devices;
        QStringList serials = m_devices.keys();
        serials.sort();
        for (const QString &serial : serials) {
            const DeviceEntry &entry = m_devices[serial];
            if (!entry.form) {
                continue;
            }
            devices.append(QJsonObject{{QStringLiteral("serial"), serial},
                                       {QStringLiteral("device_name"), entry.name},
                                       {QStringLiteral("frame_width"), entry.form->frameSize().width()},
                                       {QStringLiteral("frame_height"), entry.form->frameSize().height()}});
        }
        return {{QStringLiteral("ok"), true}, {QStringLiteral("devices"), devices}};
    }

    const QString serial = request.value(QStringLiteral("serial")).toString();
    auto it = m_devices.find(serial);
    if (serial.isEmpty() || it == m_devices.end() || !it->form) {
        return error(QStringLiteral("The requested device is not open in QtScrcpy."));
    }
    DeviceEntry &entry = it.value();
    VideoForm *form = entry.form;

    if (operation == QStringLiteral("observe")) {
        const QImage image = form->pluginFrame();
        if (image.isNull()) {
            return error(QStringLiteral("QtScrcpy has no decoded frame for this device yet."));
        }
        QByteArray jpeg;
        QBuffer output(&jpeg);
        output.open(QIODevice::WriteOnly);
        if (!image.save(&output, "JPEG", 90)) {
            return error(QStringLiteral("Could not encode the shared phone frame."));
        }
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("serial"), serial},
                {QStringLiteral("device_name"), entry.name},
                {QStringLiteral("width"), image.width()},
                {QStringLiteral("height"), image.height()},
                {QStringLiteral("frame_sequence"), static_cast<qint64>(form->pluginFrameSequence())},
                {QStringLiteral("capture_epoch"), static_cast<qint64>(form->pluginCaptureEpoch())},
                {QStringLiteral("decoded_frame_age_ms"), form->pluginFrameAge()},
                {QStringLiteral("mime_type"), QStringLiteral("image/jpeg")},
                {QStringLiteral("image_base64"), QString::fromLatin1(jpeg.toBase64())}};
    }

    const QString owner = request.value(QStringLiteral("session_id")).toString();
    if (owner.size() < 8 || owner.size() > 64) return error(QStringLiteral("Invalid plugin session."));
    const bool release = operation == QStringLiteral("touch")
        && request.value(QStringLiteral("action")).toString() == QStringLiteral("up")
        && entry.touchOwner == owner;
    if (operation == QStringLiteral("close") || release) {
        if (entry.touchOwner == owner) {
            const QSize size = form->frameSize();
            form->injectPluginTouch(QStringLiteral("up"),
                qBound(0, entry.touchPosition.x(), qMax(0, size.width() - 1)),
                qBound(0, entry.touchPosition.y(), qMax(0, size.height() - 1)));
            entry.touchOwner.clear();
        }
        return {{QStringLiteral("ok"), true}};
    }
    if (!exactInteger(request.value(QStringLiteral("capture_epoch")), form->pluginCaptureEpoch())
        || !exactInteger(request.value(QStringLiteral("width")), form->frameSize().width())
        || !exactInteger(request.value(QStringLiteral("height")), form->frameSize().height())) {
        return error(QStringLiteral("Capture changed; observe the phone again before acting."));
    }
    if (form->longScreenshotController()->isActive()) return error("Long screenshot is active; query or cancel it before other input.");

    if (operation == QStringLiteral("touch")) {
        const QString action = request.value(QStringLiteral("action")).toString();
        const int x = request.value(QStringLiteral("x")).toInt(-1);
        const int y = request.value(QStringLiteral("y")).toInt(-1);
        if (!exactInteger(request.value(QStringLiteral("x")), x)
            || !exactInteger(request.value(QStringLiteral("y")), y)
            || (action != QStringLiteral("down") && action != QStringLiteral("move"))
            || (action == QStringLiteral("down") && !entry.touchOwner.isEmpty())
            || (action == QStringLiteral("move") && entry.touchOwner != owner)) {
            return error(QStringLiteral("Invalid or conflicting plugin gesture."));
        }
        if (!form->injectPluginTouch(action, x, y)) {
            return error(QStringLiteral("Touch was rejected by the current QtScrcpy session."));
        }
        entry.touchOwner = owner;
        entry.touchPosition = QPoint(x, y);
        entry.touchDeadline = QDateTime::currentMSecsSinceEpoch() + 12000;
        return {{QStringLiteral("ok"), true}};
    }

    if (operation == QStringLiteral("key")) {
        static const QSet<int> allowedKeys = {3, 4, 24, 25, 61, 66, 67, 82, 187, 224};
        const int keycode = request.value(QStringLiteral("keycode")).toInt(-1);
        if (!exactInteger(request.value(QStringLiteral("keycode")), keycode)
            || !allowedKeys.contains(keycode) || !form->injectPluginKey(keycode)) {
            return error(QStringLiteral("Key was rejected by the current QtScrcpy session."));
        }
        return {{QStringLiteral("ok"), true}};
    }

    if (operation == QStringLiteral("text")) {
        const QString method = request.value(QStringLiteral("method")).toString();
        const QString text = request.value(QStringLiteral("text")).toString();
        const bool clipboard = method == QStringLiteral("clipboard");
        if ((!clipboard && (method != QStringLiteral("ascii") || !isPrintableAscii(text)))
                || text.contains(QChar(0))
                || (clipboard && (text.isEmpty() || text.toUtf8().size() > 65536))
                || !form->injectPluginText(text, clipboard)) {
            return error(QStringLiteral("Text was rejected by the current QtScrcpy session."));
        }
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("delivery"), clipboard
                     ? QStringLiteral("phone_clipboard_set_and_paste_requested")
                     : QStringLiteral("ascii_injected")}};
    }

    if (operation == QStringLiteral("start_app")) {
        const QString packageName = request.value(QStringLiteral("package")).toString();
        static const QRegularExpression packagePattern(
            QStringLiteral("^[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+$"));
        if (!packagePattern.match(packageName).hasMatch() || packageName.toUtf8().size() > 255
                || !form->launchPluginApp(packageName)) {
            return error(QStringLiteral("Package launch was rejected by the current QtScrcpy session."));
        }
        return {{QStringLiteral("ok"), true}};
    }

    return error(QStringLiteral("Unsupported bridge operation."));
}

void PluginBridge::setActionHandler(std::function<QJsonObject(const QString &, const QString &, const QJsonObject &)> handler)
{
    m_actionHandler = handler;
}

QString PluginBridge::newJob(const QString &serial, const QString &owner, std::function<void()> cancel)
{
    if (m_jobs.size() >= 32) {
        QString oldest;
        qint64 time = std::numeric_limits<qint64>::max();
        for (auto it = m_jobs.cbegin(); it != m_jobs.cend(); ++it) {
            if (it->result.value("state").toString() != "running" && it->created < time) { oldest = it.key(); time = it->created; }
        }
        if (oldest.isEmpty()) return {};
        m_jobs.remove(oldest);
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Job job;
    job.serial = serial; job.owner = owner; job.cancel = cancel; job.created = QDateTime::currentMSecsSinceEpoch();
    job.result = {{"state", "running"}, {"serial", serial}, {"job_id", id}};
    m_jobs.insert(id, job);
    return id;
}

void PluginBridge::finishJob(const QString &id, const QJsonObject &result)
{
    auto it = m_jobs.find(id);
    if (it == m_jobs.end() || it->result.value("state").toString() != "running") return;
    for (auto field = result.begin(); field != result.end(); ++field) it->result.insert(field.key(), field.value());
    it->cancel = {};
}

QJsonObject PluginBridge::startAdbJob(const QString &serial, const QString &owner, const QStringList &arguments)
{
    auto *adb = new qsc::AdbProcess(this);
    const QPointer<qsc::AdbProcess> safe(adb);
    const QString id = newJob(serial, owner, [safe]() { if (safe) safe->kill(); });
    if (id.isEmpty()) { adb->deleteLater(); return error("Too many active jobs."); }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this, adb, id](qsc::AdbProcess::ADB_EXEC_RESULT result) {
        if (result == qsc::AdbProcess::AER_SUCCESS_START) return;
        const bool success = result == qsc::AdbProcess::AER_SUCCESS_EXEC;
        finishJob(id, {{"state", success ? "completed" : "failed"},
                       {"stdout", adb->getStdOut().left(32768)}, {"stderr", adb->getErrorOut().left(8192)},
                       {"output_may_be_truncated", adb->getStdOut().size() > 32768},
                       {"delivery", "ADB process completion; phone UI outcome not verified"}});
        adb->deleteLater();
    });
    QTimer::singleShot(120000, adb, [this, adb, id]() {
        if (m_jobs.value(id).result.value("state").toString() != "running") return;
        finishJob(id, {{"state", "failed"}, {"error", "ADB job timed out; changes or partial file copies may already exist."}});
        adb->kill(); adb->deleteLater();
    });
    adb->execute(serial, arguments);
    return {{"ok", true}, {"job", m_jobs.value(id).result}};
}

QJsonObject PluginBridge::dispatchPhoneAction(const QJsonObject &request)
{
    const QString action = request.value("action").toString();
    if (!request.value("parameters").isObject()) return error("Expected parameters object.");
    QJsonObject parameters = request.value("parameters").toObject(), definition;
    QString reason;
    if (!PhoneFeatures::prepare(action, parameters, definition, reason)) return error(reason);
    if (definition.value("confirmed").toBool() && request.value("confirmed") != QJsonValue(true))
        return error("This action requires explicit user authorization (confirmed=true is only an attestation).");
    const QString owner = request.value("session_id").toString();
    if (owner.size() < 8 || owner.size() > 64) return error("Invalid action owner.");
    const QString kind = definition.value("kind").toString();
    if (kind == "job") {
        const QString id = parameters.value("job_id").toString();
        auto it = m_jobs.find(id);
        if (it == m_jobs.end() || it->owner != owner) return error("Job is absent, expired, or owned by another plugin session.");
        if (action == "job_cancel" && it->result.value("state").toString() == "running") {
            auto cancel = it->cancel;
            finishJob(id, {{"state", "cancelled"}, {"notice", "Already delivered input or partial copies are not undone."}});
            if (cancel) cancel();
        }
        return {{"ok", true}, {"job", m_jobs.value(id).result}};
    }
    const QString serial = request.value("serial").toString();
    if (definition.value("serial").toBool() && serial.isEmpty()) return error("Explicit device serial is required.");
    auto it = m_devices.find(serial);
    VideoForm *form = it != m_devices.end() ? it->form.data() : nullptr;
    if (definition.value("guard").toBool()) {
        if (!form || !exactInteger(request.value("capture_epoch"), form->pluginCaptureEpoch())
                || !exactInteger(request.value("width"), form->frameSize().width())
                || !exactInteger(request.value("height"), form->frameSize().height())) return error("Observe the selected device again; capture changed or is not open.");
        if (!it->touchOwner.isEmpty() || form->longScreenshotController()->isActive()) return error("Device has an active gesture or long screenshot.");
    }
    if (kind == "dialog") return m_actionHandler ? m_actionHandler(action, serial, parameters) : error("GUI action handler is unavailable.");
    if (kind == "native") {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (action == "scroll") {
            if (!form || parameters.value("x").toInt() >= form->frameSize().width()
                    || parameters.value("y").toInt() >= form->frameSize().height()
                    || (device && device->isCurrentCustomKeymap())) return error("Invalid scroll target or custom key mapping active.");
            parameters.insert("width", form->frameSize().width()); parameters.insert("height", form->frameSize().height());
        }
        if (action == "clipboard_set" && parameters.value("text").toString().toUtf8().size() > 65536) return error("Phone clipboard text exceeds 64 KiB.");
        if (!device || !device->pluginCommand(action, parameters)) return error("Operation is incompatible with this camera/display session.");
        return {{"ok", true}, {"delivery", "sent_to_native_control; UI outcome not verified"}, {"serial", serial}};
    }
    if (kind == "screenshot") {
        const QImage image = form->pluginFrame();
        if (image.isNull()) return error("No decoded phone image.");
        QApplication::clipboard()->setImage(image);
        return {{"ok", true}, {"state", "completed"}, {"clipboard_written", true}, {"saved_file", false},
                {"width", image.width()}, {"height", image.height()}};
    }
    if (kind == "longshot") {
        const QPointer<LongScreenshotController> controller(form->longScreenshotController());
        const QString id = newJob(serial, owner, [controller]() { if (controller) controller->cancel(); });
        if (id.isEmpty()) return error("Too many active jobs.");
        auto *scope = new QObject(this);
        connect(controller, &LongScreenshotController::finished, scope, [this, id, scope](const QJsonObject &result) {
            finishJob(id, result); scope->deleteLater();
        });
        connect(form, &QObject::destroyed, scope, [this, id, scope]() { finishJob(id, {{"state", "failed"}, {"error", "Device window closed."}}); scope->deleteLater(); });
        if (!controller->startRemote(parameters.value("max_frames").toInt())) {
            finishJob(id, {{"state", "failed"}, {"error", "Long screenshot unavailable in this session or key mapping mode."}});
            scope->deleteLater();
        }
        return {{"ok", true}, {"job", m_jobs.value(id).result}};
    }
    if (kind == "clipboard") {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device || device->isCameraMode()) return error("Phone clipboard is unavailable in camera mode.");
        if (m_clipboardPending.contains(serial)) return error("A clipboard reply is already pending for this device.");
        auto *scope = new QObject(this);
        const QPointer<QObject> safe(scope);
        const QString id = newJob(serial, owner, [safe]() { if (safe) safe->deleteLater(); });
        if (id.isEmpty()) { scope->deleteLater(); return error("Too many active jobs."); }
        m_clipboardPending.insert(serial);
        connect(scope, &QObject::destroyed, this, [this, serial]() { m_clipboardPending.remove(serial); });
        connect(device, &qsc::IDevice::clipboardReceived, scope, [this, id, scope](const QString &text) {
            finishJob(id, {{"state", "completed"}, {"text", text.left(65536)}, {"computer_clipboard_may_change", true}}); scope->deleteLater();
        });
        QTimer::singleShot(3000, scope, [this, id, scope]() { finishJob(id, {{"state", "failed"}, {"error", "Phone clipboard reply timed out; no cached text substituted."}}); scope->deleteLater(); });
        if (!device->pluginCommand("clipboard_get", {})) {
            finishJob(id, {{"state", "failed"}, {"error", "Clipboard control channel is unavailable."}});
            scope->deleteLater();
        }
        return {{"ok", true}, {"job", m_jobs.value(id).result}};
    }
    if (kind == "adb") {
        if (!m_actionHandler || m_actionHandler("service_status", serial, {}).value("available") != QJsonValue(true)) return error("Device is offline or absent from the GUI's authorized device list.");
        QStringList arguments;
        if (action == "push_file" || action == "pull_file" || action == "install_apk") {
            const QString local = parameters.value("local_path").toString();
            const QFileInfo file(local);
            if (!file.isAbsolute() || file.isSymLink()) return error("Use an explicit absolute non-symlink local file path.");
            if (action == "pull_file") {
                if (file.exists() || !file.dir().exists()) return error("Pull destination must be a new file in an existing directory; no overwrite.");
            } else if (!file.exists() || !file.isFile()) return error("Local input file does not exist.");
            if (action == "install_apk") {
                if (file.suffix().toLower() != "apk") return error("Installation requires an APK file.");
                arguments << "install" << "-r" << file.absoluteFilePath();
            } else {
                const QString remote = parameters.value("remote_path").toString();
                static const QRegularExpression safeRemote("^/[A-Za-z0-9_./ -]+$");
                if (!safeRemote.match(remote).hasMatch() || remote.contains("..")) return error("Use an absolute Android file path without traversal or shell syntax.");
                arguments << (action == "push_file" ? "push" : "pull");
                arguments << (action == "push_file" ? file.absoluteFilePath() : remote);
                arguments << (action == "push_file" ? remote : file.absoluteFilePath());
            }
        } else if (action == "show_touches") arguments << "shell" << "settings" << "put" << "system" << "show_touches" << (parameters.value("enabled").toBool() ? "1" : "0");
        else if (action == "query_apps") arguments << "shell" << "pm" << "list" << "packages";
        else if (action == "query_ip") arguments << "shell" << "ip" << "-o" << "addr" << "show" << "wlan0";
        else if (action == "query_device") arguments << "shell" << "getprop";
        else if (action == "query_displays") arguments << "shell" << "dumpsys" << "display";
        else if (action == "query_cameras") arguments << "shell" << "dumpsys" << "media.camera";
        else if (action == "network_connect" || action == "network_disconnect") {
            const QString address = parameters.value("address").toString();
            static const QRegularExpression safeAddress("^(?:[A-Za-z0-9][A-Za-z0-9.-]*|\\[[0-9A-Fa-f:]+\\]):[0-9]{1,5}$");
            if (!safeAddress.match(address).hasMatch() || address.section(':', -1).toInt() < 1 || address.section(':', -1).toInt() > 65535) return error("Invalid explicit network address and port.");
            if (action == "network_disconnect" && serial != address) return error("Network disconnect must target the selected network serial exactly.");
            arguments << (action == "network_connect" ? "connect" : "disconnect") << address;
        } else return error("Unknown ADB action.");
        return startAdbJob(serial, owner, arguments);
    }
    return error("Phone action has no handler.");
}

QJsonObject PluginBridge::error(const QString &message) const
{
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), message}};
}

void PluginBridge::reply(QTcpSocket *socket, const QJsonObject &response)
{
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
    socket->disconnectFromHost();
}

bool PluginBridge::writeDiscoveryFile()
{
    const QFileInfo info(m_discoveryFile);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QSaveFile file(m_discoveryFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QJsonObject discovery{{QStringLiteral("protocol"), kProtocolVersion},
                                {QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                {QStringLiteral("port"), static_cast<int>(m_server->serverPort())},
                                {QStringLiteral("token"), m_token}};
    file.write(QJsonDocument(discovery).toJson(QJsonDocument::Compact));
    file.write("\n");
    if (!file.commit()) {
        return false;
    }
    QFile::setPermissions(m_discoveryFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void PluginBridge::removeDiscoveryFile()
{
    QFile file(m_discoveryFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (document.isObject() && document.object().value(QStringLiteral("token")).toString() == m_token) {
        QFile::remove(m_discoveryFile);
    }
}
