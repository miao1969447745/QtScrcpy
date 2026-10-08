#include "wirelessdebugging.h"
#include <QTimer>
#include <QPointer>

WirelessDebugging::WirelessDebugging(Runner runner, QObject *parent)
    : QObject(parent), m_runner(std::move(runner)) {}

WirelessDebugging::State WirelessDebugging::state(const QString &serial) const
{
    return m_states.value(serial);
}

bool WirelessDebugging::isChanging() const
{
    for (const State &value : m_states) if (value.changing) return true;
    return false;
}

WirelessDebugging::Mode WirelessDebugging::parsePort(const QString &value)
{
    const QString portText = value.trimmed();
    if (portText.isEmpty()) return Disabled;
    bool valid = false;
    const int port = portText.toInt(&valid);
    if (!valid || port < -1 || port > 65535) return Unknown;
    return port > 0 ? Enabled : Disabled;
}

void WirelessDebugging::refresh(const QString &serial)
{
    if (serial.isEmpty()) return;
    State &current = m_states[serial];
    if (current.checking || current.changing) return;
    current.checking = true;
    const int generation = ++current.generation;
    emit stateChanged(serial);
    probe(serial, generation);
}

bool WirelessDebugging::setEnabled(const QString &serial, bool enabled)
{
    if (serial.isEmpty()) return false;
    State &current = m_states[serial];
    // USB and IP serials can be aliases for one physical phone. Serialize
    // wireless-mode changes globally while still allowing read-only probes.
    if (isChanging()) return false;
    // One-click Wi-Fi setup explicitly enables; it must never toggle off.
    if (!current.checking && current.mode == (enabled ? Enabled : Disabled)) return true;
    current.changing = true;
    current.checking = false;
    current.targetEnabled = enabled;
    const int generation = ++current.generation;
    emit stateChanged(serial);
    const QStringList arguments = enabled ? (QStringList() << "tcpip" << "5555")
                                          : (QStringList() << "usb");
    const QPointer<WirelessDebugging> self(this);
    m_runner(serial, arguments, [this, self, serial, generation, enabled](bool success, const QString &) {
        if (!self) return;
        if (m_states.value(serial).generation != generation) return;
        if (!success) {
            State &current = m_states[serial];
            current.changing = false;
            current.mode = Unknown;
            emit stateChanged(serial);
            emit operationFinished(serial, enabled, false);
            return;
        }
        // adb usb intentionally disconnects a network transport. Its successful
        // acknowledgement is the last reply available on that transport.
        if (!enabled && serial.contains(':')) {
            State &current = m_states[serial];
            current.mode = Disabled;
            current.changing = false;
            emit stateChanged(serial);
            emit operationFinished(serial, false, true);
            return;
        }
        // Confirm USB-connected devices after adbd restarts; never resend a
        // mutation if the transport is temporarily unavailable.
        probe(serial, generation);
    });
    return true;
}

void WirelessDebugging::probe(const QString &serial, int generation, bool persistent, int attempt)
{
    const QString property = persistent ? QStringLiteral("persist.adb.tcp.port")
                                        : QStringLiteral("service.adb.tcp.port");
    const QPointer<WirelessDebugging> self(this);
    m_runner(serial, QStringList() << "shell" << "getprop" << property,
             [this, self, serial, generation, persistent, attempt](bool success, const QString &output) {
        if (!self) return;
        if (m_states.value(serial).generation != generation) return;
        if (!success) {
            finishProbe(serial, generation, Unknown, attempt);
            return;
        }
        // AOSP uses the persistent port only when the service port is empty.
        if (!persistent && output.trimmed().isEmpty()) {
            probe(serial, generation, true, attempt);
            return;
        }
        finishProbe(serial, generation, parsePort(output), attempt);
    });
}

void WirelessDebugging::finishProbe(const QString &serial, int generation, Mode mode, int attempt)
{
    State &current = m_states[serial];
    if (current.generation != generation) return;
    if (current.changing && mode != (current.targetEnabled ? Enabled : Disabled) && attempt < 3) {
        QTimer::singleShot(250, this, [this, serial, generation, attempt]() {
            if (m_states.value(serial).generation == generation) probe(serial, generation, false, attempt + 1);
        });
        return;
    }
    const bool changed = current.changing;
    const bool target = current.targetEnabled;
    const bool success = mode == (target ? Enabled : Disabled);
    current.mode = mode;
    current.checking = false;
    current.changing = false;
    emit stateChanged(serial);
    if (changed) emit operationFinished(serial, target, success);
}
