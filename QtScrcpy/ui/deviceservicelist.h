#ifndef DEVICESERVICELIST_H
#define DEVICESERVICELIST_H

#include <QTreeWidget>
#include <QHash>

// Each row owns a serial identity; actions never depend on row indices.
class DeviceServiceList final : public QTreeWidget
{
    Q_OBJECT
public:
    enum State { Stopped, Starting, Running, Stopping, Failed };
    explicit DeviceServiceList(QWidget *parent = nullptr);
    void setDevices(const QStringList &serials);
    void setDeviceName(const QString &serial, const QString &name);
    void selectSerial(const QString &serial);
    State state(const QString &serial) const;
    quint64 generation(const QString &serial) const;
    bool isCurrentStart(const QString &serial, quint64 generation) const;
    void setState(const QString &serial, State state);
    void requestStart(const QString &serial);
    void requestStop(const QString &serial);
    void requestStopAll();

signals:
    void startRequested(const QString &serial);
    void stopRequested(const QString &serial);
    void serialSelected(const QString &serial);

private:
    struct Entry {
        State state = Stopped;
        bool available = false;
        quint64 generation = 0;
        QString name;
    };
    void updateRow(const QString &serial);
    QHash<QString, Entry> m_entries;
    QHash<QString, QTreeWidgetItem *> m_rows;
    quint64 m_generation = 0;
};

#endif
