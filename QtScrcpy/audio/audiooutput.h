#ifndef AUDIOOUTPUT_H
#define AUDIOOUTPUT_H
#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QTimer>
#include <QJsonObject>
#include <functional>
#include "adbprocess.h"

class QAudioSink;
class QAudioOutput;
class QIODevice;
class QTcpSocket;
class AudioOutput : public QObject
{
    Q_OBJECT
public:
    enum State { Stopped, Starting, Running, Installing, Failed };
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput();
    bool start(const QString &serial, int port);
    void stop();
    bool installonly(const QString &serial, int port);
    State state() const { return m_state; }
    const QString &serial() const { return m_serial; }
    QJsonObject status() const;
signals:
    void stateChanged();
private:
    void runAdb(const QStringList &arguments, std::function<void(bool, const QString &)> completion);
    void fail(const QString &message);
    void setState(State state, const QString &message = QString());
    void connectAudio();
    bool startAudioOutput();
    void flushAudio();
    void cleanup();
    State m_state = Stopped;
    QString m_serial, m_error;
    int m_port = 28200;
    quint64 m_generation = 0;
    bool m_ownsForward = false;
    QPointer<qsc::AdbProcess> m_process;
    QPointer<QTcpSocket> m_socket;
    QPointer<QIODevice> m_outputDevice;
    QByteArray m_buffer;
    QTimer m_retry, m_deadline, m_flush;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QAudioOutput *m_audioOutput = nullptr;
#else
    QAudioSink *m_audioSink = nullptr;
#endif
};
#endif
