#ifndef PLUGINBRIDGE_H
#define PLUGINBRIDGE_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QPoint>

class QJsonObject;
class QTcpServer;
class QTcpSocket;
class VideoForm;

// Local, authenticated bridge used by the scrcpy-phone-control plugin.
// It shares QtScrcpy's already-running video/control session and never exposes
// a listener outside 127.0.0.1.
class PluginBridge final : public QObject
{
    Q_OBJECT
public:
    explicit PluginBridge(QObject *parent = nullptr);
    ~PluginBridge() override;

    bool isListening() const;
    QString discoveryFile() const;
    void registerDevice(const QString &serial, const QString &deviceName, VideoForm *form);
    void unregisterDevice(const QString &serial);
    void updateDeviceName(const QString &serial, const QString &deviceName);

private:
    struct DeviceEntry {
        QString name;
        QPointer<VideoForm> form;
        QString touchOwner;
        QPoint touchPosition;
        qint64 touchDeadline = 0;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    QJsonObject dispatch(const QJsonObject &request);
    QJsonObject error(const QString &message) const;
    void reply(QTcpSocket *socket, const QJsonObject &response);
    bool writeDiscoveryFile();
    void removeDiscoveryFile();

private:
    QTcpServer *m_server = nullptr;
    QString m_token;
    QString m_discoveryFile;
    QHash<QString, DeviceEntry> m_devices;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

#endif // PLUGINBRIDGE_H
