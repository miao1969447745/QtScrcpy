#include "deviceservicelist.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>

DeviceServiceList::DeviceServiceList(QWidget *parent) : QTreeWidget(parent)
{
    setColumnCount(3);
    setHeaderLabels({tr("Device name"), tr("Service status"), tr("Actions")});
    setRootIsDecorated(false);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setMinimumWidth(480);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
    header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (item) emit serialSelected(item->data(0, Qt::UserRole).toString());
    });
    connect(this, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int column) {
        if (column == 0) requestStart(item->data(0, Qt::UserRole).toString());
    });
}

void DeviceServiceList::setDevices(const QStringList &serials)
{
    const QString selected = currentItem() ? currentItem()->data(0, Qt::UserRole).toString() : QString();
    const QSignalBlocker blockedSignals(this);
    QStringList rows = serials;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        it->available = serials.contains(it.key());
        if (!it->available && (it->state == Starting || it->state == Running || it->state == Stopping))
            rows.append(it.key());
    }
    clear();
    m_rows.clear();
    rows.removeDuplicates();
    for (const QString &serial : rows) {
        m_entries[serial].available = serials.contains(serial);
        auto *item = new QTreeWidgetItem(this);
        item->setData(0, Qt::UserRole, serial);
        item->setSizeHint(0, QSize(0, 38));
        m_rows.insert(serial, item);
        auto *actions = new QWidget(this);
        auto *layout = new QHBoxLayout(actions);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(4);
        auto *start = new QPushButton(tr("Start service"), actions);
        auto *stop = new QPushButton(tr("Stop service"), actions);
        start->setObjectName("startService");
        stop->setObjectName("stopService");
        start->setProperty("serial", serial);
        stop->setProperty("serial", serial);
        layout->addWidget(start);
        layout->addWidget(stop);
        item->setSizeHint(2, actions->sizeHint());
        setItemWidget(item, 2, actions);
        connect(start, &QPushButton::clicked, this, [this, serial]() { requestStart(serial); });
        connect(stop, &QPushButton::clicked, this, [this, serial]() { requestStop(serial); });
        updateRow(serial);
    }
    selectSerial(selected);
}

void DeviceServiceList::setDeviceName(const QString &serial, const QString &name)
{
    m_entries[serial].name = name;
    updateRow(serial);
}

void DeviceServiceList::selectSerial(const QString &serial)
{
    if (auto *item = m_rows.value(serial)) setCurrentItem(item);
}

DeviceServiceList::State DeviceServiceList::state(const QString &serial) const
{
    return m_entries.value(serial).state;
}

quint64 DeviceServiceList::generation(const QString &serial) const
{
    return m_entries.value(serial).generation;
}

bool DeviceServiceList::isCurrentStart(const QString &serial, quint64 requestGeneration) const
{
    return state(serial) == Starting && generation(serial) == requestGeneration;
}

void DeviceServiceList::setState(const QString &serial, State next)
{
    auto &entry = m_entries[serial];
    entry.state = next;
    if (next != Starting && next != Running) entry.generation = ++m_generation;
    updateRow(serial);
}

void DeviceServiceList::requestStart(const QString &serial)
{
    auto &entry = m_entries[serial];
    if (!entry.available || (entry.state != Stopped && entry.state != Failed)) return;
    entry.generation = ++m_generation;
    setState(serial, Starting);
    emit startRequested(serial);
}

void DeviceServiceList::requestStop(const QString &serial)
{
    if (state(serial) != Starting && state(serial) != Running) return;
    setState(serial, Stopping);
    emit stopRequested(serial);
}

void DeviceServiceList::requestStopAll()
{
    const QStringList serials = m_entries.keys();
    for (const QString &serial : serials) requestStop(serial);
}

void DeviceServiceList::updateRow(const QString &serial)
{
    auto *row = m_rows.value(serial);
    if (!row) return;
    const auto entry = m_entries.value(serial);
    row->setText(0, entry.name.isEmpty() ? serial : entry.name);
    row->setToolTip(0, serial);
    QString status;
    switch (entry.state) {
    case Stopped: status = entry.available ? tr("Stopped") : tr("Offline"); break;
    case Starting: status = tr("Starting..."); break;
    case Running: status = tr("Running"); break;
    case Stopping: status = tr("Stopping..."); break;
    case Failed: status = entry.available ? tr("Start failed") : tr("Offline"); break;
    }
    row->setText(1, status);
    auto *actions = itemWidget(row, 2);
    actions->findChild<QPushButton *>("startService")->setEnabled(
        entry.available && (entry.state == Stopped || entry.state == Failed));
    actions->findChild<QPushButton *>("stopService")->setEnabled(
        entry.state == Starting || entry.state == Running);
}
