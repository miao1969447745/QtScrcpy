#include "audiooutput.h"
#include <QAudioOutput>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTcpServer>
#include <memory>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QAudioSink>
#include <QAudioDevice>
#include <QMediaDevices>
#endif

AudioOutput::AudioOutput(QObject *parent) : QObject(parent)
{
    m_retry.setInterval(250);
    m_deadline.setSingleShot(true);
    m_flush.setInterval(10);
    connect(&m_retry, &QTimer::timeout, this, &AudioOutput::connectAudio);
    connect(&m_flush, &QTimer::timeout, this, &AudioOutput::flushAudio);
    connect(&m_deadline, &QTimer::timeout, this, [this]() { fail("Audio startup timed out. Approve capture on the phone; no permission is bypassed."); });
}

AudioOutput::~AudioOutput() { cleanup(); }

void AudioOutput::setState(State state, const QString &message)
{
    m_state = state; m_error = message; emit stateChanged();
}

QJsonObject AudioOutput::status() const
{
    const char *names[] = {"stopped", "starting", "running", "installing", "failed"};
    return {{"state", names[static_cast<int>(m_state)]}, {"serial", m_serial}, {"error", m_error},
            {"running", m_state == Running}, {"note", "sndcpy requires Android capture permission; protected audio is not bypassed."}};
}

void AudioOutput::runAdb(const QStringList &arguments, std::function<void(bool, const QString &)> completion)
{
    auto *adb = new qsc::AdbProcess(this);
    m_process = adb;
    const quint64 generation = m_generation;
    auto done = std::make_shared<bool>(false);
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this, adb, done, generation, completion](qsc::AdbProcess::ADB_EXEC_RESULT result) {
        if (result == qsc::AdbProcess::AER_SUCCESS_START || *done) return;
        *done = true;
        if (m_process == adb) m_process = nullptr;
        if (generation == m_generation) completion(result == qsc::AdbProcess::AER_SUCCESS_EXEC, adb->getStdOut() + adb->getErrorOut());
        adb->deleteLater();
    });
    QTimer::singleShot(30000, adb, [this, adb, done, generation, completion]() {
        if (*done) return;
        *done = true;
        if (m_process == adb) m_process = nullptr;
        if (generation == m_generation) completion(false, "ADB audio operation timed out.");
        adb->kill(); adb->deleteLater();
    });
    adb->execute(m_serial, arguments);
}

bool AudioOutput::installonly(const QString &serial, int port)
{
    Q_UNUSED(port);
    if (serial.isEmpty() || (m_state != Stopped && m_state != Failed)) return false;
    const QString apk = QCoreApplication::applicationDirPath() + "/sndcpy.apk";
    if (!QFileInfo(apk).isFile()) { setState(Failed, "Bundled sndcpy.apk is missing."); return false; }
    cleanup(); m_serial = serial; setState(Installing);
    runAdb({"install", "-r", apk}, [this](bool success, const QString &output) {
        setState(success ? Stopped : Failed, success ? QString() : output.left(2048));
    });
    return true;
}

bool AudioOutput::start(const QString &serial, int port)
{
    if (serial.isEmpty() || port < 1024 || port > 65535 || (m_state != Stopped && m_state != Failed)) return false;
    cleanup(); m_serial = serial; m_port = port;
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, static_cast<quint16>(port))) {
        setState(Failed, "Audio port is already in use; another session will not be overwritten."); return false;
    }
    probe.close();
    setState(Starting);
    runAdb({"shell", "pm", "path", "com.rom1v.sndcpy"}, [this](bool success, const QString &output) {
        if (!success || !output.contains("package:")) { fail("Install sndcpy explicitly before starting audio."); return; }
        runAdb({"forward", "--no-rebind", "tcp:" + QString::number(m_port), "localabstract:sndcpy"}, [this](bool forwarded, const QString &message) {
            if (!forwarded) { fail(message.left(2048)); return; }
            m_ownsForward = true;
            runAdb({"shell", "am", "start", "-n", "com.rom1v.sndcpy/.MainActivity"}, [this](bool launched, const QString &launchOutput) {
                if (!launched || launchOutput.contains("Error:")) { fail(launchOutput.left(2048)); return; }
                m_deadline.start(30000);
                m_retry.start();
                connectAudio();
            });
        });
    });
    return true;
}

void AudioOutput::connectAudio()
{
    if (m_state != Starting || m_socket) return;
    auto *socket = new QTcpSocket(this);
    m_socket = socket;
    socket->setReadBufferSize(256 * 1024);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (m_socket != socket || (m_state != Starting && m_state != Running)) return;
        m_buffer += socket->readAll();
        if (m_buffer.size() > 512 * 1024) { fail("Audio playback buffer overflow."); return; }
        if (m_state == Starting && !m_buffer.isEmpty()) {
            if (!startAudioOutput()) { fail("No compatible computer audio output device."); return; }
            m_retry.stop(); m_deadline.stop(); m_flush.start(); setState(Running);
        }
        flushAudio();
    });
    const auto disconnected = [this, socket]() {
        if (m_socket != socket) return;
        m_socket = nullptr; socket->deleteLater();
        if (m_state == Running) fail("Phone audio stream disconnected.");
    };
    connect(socket, &QTcpSocket::disconnected, this, disconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QTcpSocket::errorOccurred, this, [disconnected](QAbstractSocket::SocketError) { disconnected(); });
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [disconnected](QAbstractSocket::SocketError) { disconnected(); });
#endif
    socket->connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_port));
}

bool AudioOutput::startAudioOutput()
{
    QAudioFormat format;
    format.setSampleRate(48000); format.setChannelCount(2);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    format.setSampleSize(16); format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian); format.setSampleType(QAudioFormat::SignedInt);
    const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
    if (!device.isFormatSupported(format)) return false;
    m_audioOutput = new QAudioOutput(format, this);
    m_outputDevice = m_audioOutput->start();
    connect(m_audioOutput, &QAudioOutput::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && m_state == Running && m_audioOutput->error() != QAudio::NoError)
            fail("Computer audio output stopped.");
    });
#else
    format.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(format)) return false;
    m_audioSink = new QAudioSink(device, format, this);
    m_outputDevice = m_audioSink->start();
    connect(m_audioSink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && m_state == Running && m_audioSink->error() != QAudio::NoError)
            fail("Computer audio output stopped.");
    });
#endif
    return m_outputDevice;
}

void AudioOutput::flushAudio()
{
    if (m_state != Running || !m_outputDevice || m_buffer.isEmpty()) return;
    const qint64 written = m_outputDevice->write(m_buffer.constData(), m_buffer.size());
    if (written < 0) { fail("Could not write to the audio output device."); return; }
    if (written > 0) m_buffer.remove(0, static_cast<int>(written));
}

void AudioOutput::cleanup()
{
    ++m_generation;
    m_retry.stop(); m_deadline.stop(); m_flush.stop();
    if (m_process) { m_process->kill(); m_process->deleteLater(); m_process = nullptr; }
    if (m_socket) {
        auto *socket = m_socket.data(); m_socket = nullptr;
        socket->disconnect(this); socket->abort(); socket->deleteLater();
    }
    m_buffer.clear(); m_outputDevice = nullptr;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    if (m_audioOutput) { m_audioOutput->disconnect(this); m_audioOutput->stop(); m_audioOutput->deleteLater(); m_audioOutput = nullptr; }
#else
    if (m_audioSink) { m_audioSink->disconnect(this); m_audioSink->stop(); m_audioSink->deleteLater(); m_audioSink = nullptr; }
#endif
    if (m_ownsForward) {
        m_ownsForward = false;
        auto *remove = new qsc::AdbProcess(QCoreApplication::instance());
        connect(remove, &qsc::AdbProcess::adbProcessResult, remove, [remove](qsc::AdbProcess::ADB_EXEC_RESULT result) {
            if (result != qsc::AdbProcess::AER_SUCCESS_START) remove->deleteLater();
        });
        QTimer::singleShot(5000, remove, [remove]() { remove->kill(); remove->deleteLater(); });
        remove->execute(m_serial, {"forward", "--remove", "tcp:" + QString::number(m_port)});
    }
}

void AudioOutput::fail(const QString &message) { cleanup(); setState(Failed, message); }
void AudioOutput::stop() { cleanup(); setState(Stopped); }
