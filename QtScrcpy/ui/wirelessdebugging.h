#ifndef WIRELESSDEBUGGING_H
#define WIRELESSDEBUGGING_H

#include <QObject>
#include <QHash>
#include <QStringList>
#include <functional>

// State belongs to a serial, never to the globally selected combo-box row.
class WirelessDebugging final : public QObject
{
    Q_OBJECT
public:
    enum Mode { Unknown, Disabled, Enabled };
    struct State {
        Mode mode = Unknown;
        bool checking = false;
        bool changing = false;
        bool targetEnabled = false;
        int generation = 0;
    };
    using Completion = std::function<void(bool, const QString &)>;
    using Runner = std::function<void(const QString &, const QStringList &, Completion)>;

    explicit WirelessDebugging(Runner runner, QObject *parent = nullptr);
    State state(const QString &serial) const;
    void refresh(const QString &serial);
    bool setEnabled(const QString &serial, bool enabled);
    bool isChanging() const;
    static Mode parsePort(const QString &value);

signals:
    void stateChanged(const QString &serial);
    void operationFinished(const QString &serial, bool enabled, bool success);

private:
    void probe(const QString &serial, int generation, bool persistent = false, int attempt = 0);
    void finishProbe(const QString &serial, int generation, Mode mode, int attempt);
    Runner m_runner;
    QHash<QString, State> m_states;
};

#endif
