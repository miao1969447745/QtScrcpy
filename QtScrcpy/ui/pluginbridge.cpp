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

namespace {
constexpr int kProtocolVersion = 1;
constexpr int kMaxRequestBytes = 128 * 1024;

bool exactInteger(const QJsonValue &value, qint64 expected)
{
    return value.isDouble() && value.toDouble() == static_cast<double>(expected);
}

QString bridgeFilePath()
{
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
                {QStringLiteral("application_version"), QCoreApplication::applicationVersion()}};
    }
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
