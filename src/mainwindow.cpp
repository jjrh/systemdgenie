/*******************************************************************************
 * Copyright (C) 2016 Ragnar Thomsen <rthomsen6@gmail.com>                     *
 *                                                                             *
 * This program is free software: you can redistribute it and/or modify it     *
 * under the terms of the GNU General Public License as published by the Free  *
 * Software Foundation, either version 2 of the License, or (at your option)   *
 * any later version.                                                          *
 *                                                                             *
 * This program is distributed in the hope that it will be useful, but WITHOUT *
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or       *
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for    *
 * more details.                                                               *
 *                                                                             *
 * You should have received a copy of the GNU General Public License along     *
 * with this program. If not, see <http://www.gnu.org/licenses/>.              *
 *******************************************************************************/

#include "mainwindow.h"

#include <KActionCollection>
#include <KAuth>
#include <KColorScheme>
#include <KLocalizedString>
#include <KMessageBox>
#include <KXMLGUIFactory>

#include <QDebug>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QtDBus>

#include <unistd.h>

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
{
    ui.setupUi(this);

    ui.leSearchUnit->setFocus();

    // See if systemd is reachable via dbus
    if (getDbusProperty(QStringLiteral("Version"), sysdMgr) != QLatin1String("invalidIface")) {
        systemdVersion = getDbusProperty(QStringLiteral("Version"), sysdMgr).toString().remove(QStringLiteral("systemd ")).toInt();
        qDebug() << "Detected systemd" << systemdVersion;
    } else {
        qDebug() << "Unable to contact systemd daemon!";
        KMessageBox::error(this, i18n("Unable to contact the systemd daemon. Quitting..."), i18n("SystemdGenie"));
        close();
    }

    // Search for user dbus.
    if (QFileInfo::exists(QStringLiteral("/run/user/%1/bus").arg(QString::number(getuid())))) {
        m_userBusPath = QStringLiteral("unix:path=/run/user/%1/bus").arg(QString::number(getuid()));
    } else if (QFileInfo::exists(QStringLiteral("/run/user/%1/dbus/user_bus_socket").arg(QString::number(getuid())))) {
        m_userBusPath = QStringLiteral("unix:path=/run/user/%1/dbus/user_bus_socket").arg(QString::number(getuid()));
    } else {
        qDebug() << "User dbus not found. Support for user units disabled.";
        ui.tabWidget->setTabEnabled(1, false);
        ui.tabWidget->setTabToolTip(1, i18n("User dbus not found. Support for user units disabled."));
        enableUserUnits = false;
    }

    QStringList allowUnitTypes = QStringList{i18n("All"), i18n("Services"), i18n("Automounts"), i18n("Devices"),
                                             i18n("Mounts"), i18n("Paths"), i18n("Scopes"), i18n("Slices"),
                                             i18n("Sockets"), i18n("Swaps"), i18n("Targets"), i18n("Timers")};
    ui.cmbUnitTypes->addItems(allowUnitTypes);
    ui.cmbUserUnitTypes->addItems(allowUnitTypes);

    // Get list of units
    slotRefreshUnitsList(true, sys);
    slotRefreshUnitsList(true, user);

    setupUnitslist();
    setupSessionlist();
    setupTimerlist();
    setupConfFilelist();
    setupActions();

    setupSignalSlots();

    m_lblLog = new QLabel(this);
    m_lblLog->setText(QString());
    statusBar()->addPermanentWidget(m_lblLog, 20);

    setupGUI(Default, QStringLiteral("systemdgenieui.rc"));
}

MainWindow::~MainWindow()
{
}

void MainWindow::quit()
{
    close();
}


QDBusArgument &operator<<(QDBusArgument &argument, const SystemdUnit &unit)
{
    argument.beginStructure();
    argument << unit.id
             << unit.description
             << unit.load_state
             << unit.active_state
             << unit.sub_state
             << unit.following
             << unit.unit_path
             << unit.job_id
             << unit.job_type
             << unit.job_path;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdUnit &unit)
{
    argument.beginStructure();
    argument >> unit.id
            >> unit.description
            >> unit.load_state
            >> unit.active_state
            >> unit.sub_state
            >> unit.following
            >> unit.unit_path
            >> unit.job_id
            >> unit.job_type
            >> unit.job_path;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdSession &session)
{
    argument.beginStructure();
    argument << session.session_id
             << session.user_id
             << session.user_name
             << session.seat_id
             << session.session_path;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdSession &session)
{
    argument.beginStructure();
    argument >> session.session_id
            >> session.user_id
            >> session.user_name
            >> session.seat_id
            >> session.session_path;
    argument.endStructure();
    return argument;
}

void MainWindow::setupSignalSlots()
{
    // Connect signals for unit tabs.
    connect(ui.chkInactiveUnits, &QCheckBox::stateChanged, this, &MainWindow::slotChkShowUnits);
    connect(ui.chkUnloadedUnits, &QCheckBox::stateChanged, this, &MainWindow::slotChkShowUnits);
    connect(ui.chkInactiveUserUnits, &QCheckBox::stateChanged, this, &MainWindow::slotChkShowUnits);
    connect(ui.chkUnloadedUserUnits, &QCheckBox::stateChanged, this, &MainWindow::slotChkShowUnits);
    connect(ui.cmbUnitTypes, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &MainWindow::slotCmbUnitTypes);
    connect(ui.cmbUserUnitTypes,static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &MainWindow::slotCmbUnitTypes);
    connect(ui.tblUnits, &QTableView::customContextMenuRequested, this, &MainWindow::slotUnitContextMenu);
    connect(ui.tblUserUnits, &QTableView::customContextMenuRequested, this, &MainWindow::slotUnitContextMenu);
    connect(ui.tblConfFiles, &QTableView::customContextMenuRequested, this, &MainWindow::slotConfFileContextMenu);
    connect(ui.leSearchUnit, &QLineEdit::textChanged, this, &MainWindow::slotLeSearchUnitChanged);
    connect(ui.leSearchUserUnit, &QLineEdit::textChanged, this, &MainWindow::slotLeSearchUnitChanged);
    connect(ui.tblUnits->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    connect(ui.tblUserUnits->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    connect(ui.tblConfFiles->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    connect(ui.tblSessions->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::updateActions);
    connect(ui.tabWidget, &QTabWidget::currentChanged, this, &MainWindow::updateActions);

    connect(ui.tabWidget, &QTabWidget::currentChanged, [=]{
        if (ui.tabWidget->currentIndex() == 0)
            ui.leSearchUnit->setFocus();
        else if (ui.tabWidget->currentIndex() == 1)
            ui.leSearchUserUnit->setFocus();
    });

    // Connect signals for sessions tab.
    connect(ui.tblSessions, &QTableView::customContextMenuRequested, this, &MainWindow::slotSessionContextMenu);

    // Subscribe to dbus signals from systemd system daemon and connect them to slots.
    callDbusMethod(QStringLiteral("Subscribe"), sysdMgr);
    m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("Reloading"),
                        this,
                        SLOT(slotSystemSystemdReloading(bool)));
    /*m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("UnitNew"),
                        this,
                        SLOT(slotSystemUnitNew(const QString&, const QDBusObjectPath&)));
    m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("UnitRemoved"),
                        this,
                        SLOT(slotSystemUnitRemoved(const QString&, const QDBusObjectPath&)));*/

    m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("JobNew"),
                        this,
                        SLOT(slotSystemJobNew(uint, const QDBusObjectPath&, const QString&)));
    // We need to use the JobRemoved signal, because stopping units does not emit PropertiesChanged signal
    m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("JobRemoved"),
                        this,
                        SLOT(slotSystemJobRemoved(uint, const QDBusObjectPath&, const QString&, const QString&)));

    m_systemBus.connect(connSystemd,
                        pathSysdMgr,
                        ifaceMgr,
                        QStringLiteral("UnitFilesChanged"),
                        this,
                        SLOT(slotSystemUnitFilesChanged()));
    m_systemBus.connect(connSystemd,
                        QString(),
                        ifaceDbusProp,
                        QStringLiteral("PropertiesChanged"),
                        this,
                        SLOT(slotSystemPropertiesChanged(const QString&, const QVariantMap&, const QStringList&)));


    // Subscribe to dbus signals from systemd user daemon and connect them to slots
    callDbusMethod(QStringLiteral("Subscribe"), sysdMgr, user);
    QDBusConnection userbus = QDBusConnection::connectToBus(m_userBusPath, connSystemd);
    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("Reloading"),
                    this,
                    SLOT(slotUserSystemdReloading(bool)));
    /*userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("UnitNew"),
                    this,
                    SLOT(slotUnitNew(const QString&, const QDBusObjectPath&)));
    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("UnitRemoved"),
                    this,
                    SLOT(slotUnitRemoved(const QString&, const QDBusObjectPath&)));*/

    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("JobNew"),
                    this,
                    SLOT(slotUserJobNew(uint, const QDBusObjectPath&, const QString&)));
    // We need to use the JobRemoved signal, because stopping units does not emit PropertiesChanged signal
    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("JobRemoved"),
                    this,
                    SLOT(slotUserJobRemoved(uint, const QDBusObjectPath&, const QString&, const QString&)));

    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("UnitFilesChanged"),
                    this,
                    SLOT(slotUserUnitFilesChanged()));
    userbus.connect(connSystemd,
                    QString(),
                    ifaceDbusProp,
                    QStringLiteral("PropertiesChanged"),
                    this,
                    SLOT(slotUserPropertiesChanged(const QString&, const QVariantMap&, const QStringList&)));

    /*
    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("Reloading"),
                    this,
                    SLOT(slotUserSystemdReloading(bool)));
    userbus.connect(connSystemd,
                    pathSysdMgr,
                    ifaceMgr,
                    QStringLiteral("UnitFilesChanged"),
                    this,
                    SLOT(slotUserUnitsChanged()));
    userbus.connect(connSystemd, QString(), ifaceDbusProp,
                    QStringLiteral("PropertiesChanged"), this, SLOT(slotUserUnitsChanged()));
    userbus.connect(connSystemd, pathSysdMgr, ifaceMgr,
                    QStringLiteral("JobRemoved"), this, SLOT(slotUserUnitsChanged())); */

    // logind.
    m_systemBus.connect(connLogind,
                        QString(),
                        ifaceDbusProp,
                        QStringLiteral("PropertiesChanged"),
                        this,
                        SLOT(slotLogindPropertiesChanged(QString,QVariantMap,QStringList)));
}


void MainWindow::setupUnitslist()
{
    // Sets up the units list initially

    // Register the meta type for storing units
    qDBusRegisterMetaType<SystemdUnit>();

    QMap<filterType, QString> filters;
    filters[activeState] = QString();
    filters[unitType] = QString();
    filters[unitName] = QString();

    // QList<SystemdUnit> *ptrUnits;
    // ptrUnits = &m_systemUnitsList;

    // Setup the system unit model
    ui.tblUnits->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_systemUnitModel = new UnitModel(this, &m_systemUnitsList);
    m_systemUnitFilterModel = new SortFilterUnitModel(this);
    m_systemUnitFilterModel->setDynamicSortFilter(false);
    m_systemUnitFilterModel->initFilterMap(filters);
    m_systemUnitFilterModel->setSourceModel(m_systemUnitModel);
    ui.tblUnits->setModel(m_systemUnitFilterModel);
    ui.tblUnits->sortByColumn(0, Qt::AscendingOrder);
    ui.tblUnits->setColumnWidth(0, 400);

    // Setup the user unit model
    ui.tblUserUnits->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_userUnitModel = new UnitModel(this, &m_userUnitsList, m_userBusPath);
    m_userUnitFilterModel = new SortFilterUnitModel(this);
    m_userUnitFilterModel->setDynamicSortFilter(false);
    m_userUnitFilterModel->initFilterMap(filters);
    m_userUnitFilterModel->setSourceModel(m_userUnitModel);
    ui.tblUserUnits->setModel(m_userUnitFilterModel);
    ui.tblUserUnits->sortByColumn(0, Qt::AscendingOrder);
    ui.tblUserUnits->setColumnWidth(0, 400);

    slotChkShowUnits(-1);
}

void MainWindow::setupSessionlist()
{
    // Sets up the session list initially

    // Register the meta type for storing units
    qDBusRegisterMetaType<SystemdSession>();

    // Setup model for session list
    m_sessionModel = new QStandardItemModel(this);

    // Install eventfilter to capture mouse move events
    ui.tblSessions->viewport()->installEventFilter(this);

    // Set header row
    m_sessionModel->setHorizontalHeaderItem(0, new QStandardItem(i18n("Session ID")));
    m_sessionModel->setHorizontalHeaderItem(1, new QStandardItem(i18n("Session Object Path"))); // This column is hidden
    m_sessionModel->setHorizontalHeaderItem(2, new QStandardItem(i18n("State")));
    m_sessionModel->setHorizontalHeaderItem(3, new QStandardItem(i18n("User ID")));
    m_sessionModel->setHorizontalHeaderItem(4, new QStandardItem(i18n("User Name")));
    m_sessionModel->setHorizontalHeaderItem(5, new QStandardItem(i18n("Seat ID")));
    ui.tblSessions->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Set model for QTableView (should be called after headers are set)
    ui.tblSessions->setModel(m_sessionModel);
    ui.tblSessions->setColumnHidden(1, true);

    // Add all the sessions
    slotRefreshSessionList();
}

void MainWindow::setupTimerlist()
{
    // Sets up the timer list initially

    // Setup model for timer list
    m_timerModel = new QStandardItemModel(this);

    // Install eventfilter to capture mouse move events
    // ui.tblTimers->viewport()->installEventFilter(this);

    // Set header row
    m_timerModel->setHorizontalHeaderItem(0, new QStandardItem(i18n("Timer")));
    m_timerModel->setHorizontalHeaderItem(1, new QStandardItem(i18n("Next")));
    m_timerModel->setHorizontalHeaderItem(2, new QStandardItem(i18n("Left")));
    m_timerModel->setHorizontalHeaderItem(3, new QStandardItem(i18n("Last")));
    m_timerModel->setHorizontalHeaderItem(4, new QStandardItem(i18n("Passed")));
    m_timerModel->setHorizontalHeaderItem(5, new QStandardItem(i18n("Activates")));
    ui.tblTimers->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Set model for QTableView (should be called after headers are set)
    ui.tblTimers->setModel(m_timerModel);
    ui.tblTimers->sortByColumn(1, Qt::AscendingOrder);

    // Setup a timer that updates the left and passed columns every 5secs
    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(slotUpdateTimers()));
    timer->start(1000);

    slotRefreshTimerList();
}

void MainWindow::setupConfFilelist()
{
    m_confFileModel = new QStandardItemModel(this);
    m_confFileModel->setHorizontalHeaderItem(0, new QStandardItem(i18n("File")));
    m_confFileModel->setHorizontalHeaderItem(1, new QStandardItem(i18n("Modified")));
    m_confFileModel->setHorizontalHeaderItem(2, new QStandardItem(i18n("Description")));

    ui.tblConfFiles->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui.tblConfFiles->setModel(m_confFileModel);
    ui.tblConfFiles->sortByColumn(1, Qt::AscendingOrder);

    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/coredump.conf"),
                                   QStringLiteral("coredump.conf"),
                                   i18n("Coredump generation and storage")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/journal-upload.conf"),
                                   QStringLiteral("journal-upload.conf"),
                                   i18n("Send journal messages over network")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/journald.conf"),
                                   QStringLiteral("journald.conf"),
                                   i18n("Journal manager settings")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/logind.conf"),
                                   QStringLiteral("logind.conf"),
                                   i18n("Login manager configuration")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/resolved.conf"),
                                   QStringLiteral("resolved.conf"),
                                   i18n("Network name resolution configuration")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/system.conf"),
                                   QStringLiteral("systemd-system.conf"),
                                   i18n("Systemd daemon configuration")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/timesyncd.conf"),
                                   QStringLiteral("timesyncd.conf"),
                                   i18n("Time synchronization settings")));
    m_confFileList.append(conffile(QStringLiteral("/etc/systemd/user.conf"),
                                   QStringLiteral("systemd-system.conf"),
                                   i18n("Systemd user daemon configuration")));

    slotRefreshConfFileList();

    QFileSystemWatcher *m_fileWatcher = new QFileSystemWatcher;
    connect(m_fileWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::slotRefreshConfFileList);
    foreach (const conffile &f, m_confFileList) {
        m_fileWatcher->addPath(f.filePath);
    }
}

void MainWindow::slotRefreshConfFileList()
{
    foreach (const conffile &f, m_confFileList) {
        if (!QFileInfo::exists(f.filePath)) {
            continue;
        }
        QStandardItem *item = new QStandardItem(QIcon::fromTheme(QStringLiteral("text-plain")), QStringLiteral("%1").arg(f.filePath));
        QStandardItem *item2 = new QStandardItem(QFileInfo(f.filePath).lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        QStandardItem *item3 = new QStandardItem(f.description);
        m_confFileModel->setItem(m_confFileList.indexOf(f), 0, item);
        m_confFileModel->setItem(m_confFileList.indexOf(f), 1, item2);
        m_confFileModel->setItem(m_confFileList.indexOf(f), 2, item3);
    }
    ui.tblConfFiles->resizeColumnsToContents();
    ui.tblConfFiles->resizeRowsToContents();
}

void MainWindow::slotChkShowUnits(int state)
{
    if (state == -1 ||
        QObject::sender()->objectName() == QLatin1String("chkInactiveUnits") ||
        QObject::sender()->objectName() == QLatin1String("chkUnloadedUnits"))
    {
        // System units
        if (ui.chkInactiveUnits->isChecked())
        {
            ui.chkUnloadedUnits->setEnabled(true);
            if (ui.chkUnloadedUnits->isChecked())
                m_systemUnitFilterModel->addFilterRegExp(activeState, QString());
            else
                m_systemUnitFilterModel->addFilterRegExp(activeState, QStringLiteral("active"));
        }
        else
        {
            ui.chkUnloadedUnits->setEnabled(false);
            m_systemUnitFilterModel->addFilterRegExp(activeState, QStringLiteral("^(active)"));
        }
        m_systemUnitFilterModel->invalidate();
        ui.tblUnits->sortByColumn(ui.tblUnits->horizontalHeader()->sortIndicatorSection(),
                                  ui.tblUnits->horizontalHeader()->sortIndicatorOrder());
    }
    if (state == -1 ||
        QObject::sender()->objectName() == QLatin1String("chkInactiveUserUnits") ||
        QObject::sender()->objectName() == QLatin1String("chkUnloadedUserUnits"))
    {
        // User units
        if (ui.chkInactiveUserUnits->isChecked())
        {
            ui.chkUnloadedUserUnits->setEnabled(true);
            if (ui.chkUnloadedUserUnits->isChecked())
                m_userUnitFilterModel->addFilterRegExp(activeState, QString());
            else
                m_userUnitFilterModel->addFilterRegExp(activeState, QStringLiteral("active"));
        }
        else
        {
            ui.chkUnloadedUserUnits->setEnabled(false);
            m_userUnitFilterModel->addFilterRegExp(activeState, QStringLiteral("^(active)"));
        }
        m_userUnitFilterModel->invalidate();
        ui.tblUserUnits->sortByColumn(ui.tblUserUnits->horizontalHeader()->sortIndicatorSection(),
                                      ui.tblUserUnits->horizontalHeader()->sortIndicatorOrder());
    }
    updateUnitCount();
}

void MainWindow::slotCmbUnitTypes(int index)
{
    // Filter unit list for a selected unit type

    if (QObject::sender()->objectName() == QLatin1String("cmbUnitTypes"))
    {
        m_systemUnitFilterModel->addFilterRegExp(unitType,  QStringLiteral("(%1)$").arg(unitTypeSufx.at(index)));
        m_systemUnitFilterModel->invalidate();
        ui.tblUnits->sortByColumn(ui.tblUnits->horizontalHeader()->sortIndicatorSection(),
                                  ui.tblUnits->horizontalHeader()->sortIndicatorOrder());
    }
    else if (QObject::sender()->objectName() == QLatin1String("cmbUserUnitTypes"))
    {
        m_userUnitFilterModel->addFilterRegExp(unitType, QStringLiteral("(%1)$").arg(unitTypeSufx.at(index)));
        m_userUnitFilterModel->invalidate();
        ui.tblUserUnits->sortByColumn(ui.tblUserUnits->horizontalHeader()->sortIndicatorSection(),
                                      ui.tblUserUnits->horizontalHeader()->sortIndicatorOrder());
    }
    updateUnitCount();
}

void MainWindow::slotRefreshUnitsList(bool initial, dbusBus bus)
{
    // Updates the unit lists

    if (bus == sys) {

        qDebug() << "Refreshing system units...";

        // get an updated list of system units via dbus
        m_systemUnitsList.clear();
        m_systemUnitsList = getUnitsFromDbus(sys);
        m_noActSystemUnits = 0;
        foreach (const SystemdUnit &unit, m_systemUnitsList)
        {
            if (unit.active_state == QLatin1String("active"))
                m_noActSystemUnits++;
        }
        if (!initial) {
            m_systemUnitModel->dataChanged(m_systemUnitModel->index(0, 0), m_systemUnitModel->index(m_systemUnitModel->rowCount(), 0));
            m_systemUnitFilterModel->invalidate();
            updateUnitCount();
            slotRefreshTimerList();
            updateActions();
        }

    } else if (enableUserUnits && bus == user) {

        qDebug() << "Refreshing user units...";

        // get an updated list of user units via dbus
        m_userUnitsList.clear();
        m_userUnitsList = getUnitsFromDbus(user);
        m_noActUserUnits = 0;
        foreach (const SystemdUnit &unit, m_userUnitsList)
        {
            if (unit.active_state == QLatin1String("active"))
                m_noActUserUnits++;
        }
        if (!initial) {
            m_userUnitModel->dataChanged(m_userUnitModel->index(0, 0), m_userUnitModel->index(m_userUnitModel->rowCount(), 0));
            m_userUnitFilterModel->invalidate();
            updateUnitCount();
            slotRefreshTimerList();
            updateActions();
        }
    }
}

void MainWindow::slotRefreshSessionList()
{
    // Updates the session list
    qDebug() << "Refreshing session list...";

    // clear list
    m_sessionList.clear();

    // get an updated list of sessions via dbus
    QDBusMessage dbusreply = callDbusMethod(QStringLiteral("ListSessions"), logdMgr);

    // extract the list of sessions from the reply
    const QDBusArgument arg = dbusreply.arguments().at(0).value<QDBusArgument>();
    if (arg.currentType() == QDBusArgument::ArrayType)
    {
        arg.beginArray();
        while (!arg.atEnd())
        {
            SystemdSession session;
            arg >> session;
            m_sessionList.append(session);
        }
        arg.endArray();
    }

    // Iterate through the new list and compare to model
    foreach (const SystemdSession &s, m_sessionList) {
        // This is needed to get the "State" property

        QList<QStandardItem *> items = m_sessionModel->findItems(s.session_id, Qt::MatchExactly, 0);

        if (items.isEmpty())
        {
            // New session discovered so add it to the model
            QList<QStandardItem *> row;
            row <<
                   new QStandardItem(s.session_id) <<
                   new QStandardItem(s.session_path.path()) <<
                   new QStandardItem(getDbusProperty(QStringLiteral("State"), logdSession, s.session_path).toString()) <<
                   new QStandardItem(QString::number(s.user_id)) <<
                   new QStandardItem(s.user_name) <<
                   new QStandardItem(s.seat_id);
            m_sessionModel->appendRow(row);
        } else {
            m_sessionModel->item(items.at(0)->row(), 2)->setData(getDbusProperty(QStringLiteral("State"), logdSession, s.session_path).toString(), Qt::DisplayRole);
        }
    }

    // Check to see if any sessions were removed
    if (m_sessionModel->rowCount() != m_sessionList.size())
    {
        QList<QPersistentModelIndex> indexes;
        // Loop through model and compare to retrieved m_sessionList
        for (int row = 0; row < m_sessionModel->rowCount(); ++row)
        {
            SystemdSession session;
            session.session_id = m_sessionModel->index(row,0).data().toString();
            if (!m_sessionList.contains(session))
            {
                // Add removed units to list for deletion
                // qDebug() << "Unit removed: " << systemUnitModel->index(row,0).data().toString();
                indexes << m_sessionModel->index(row,0);
            }
        }
        // Delete the identified units from model
        foreach (const QPersistentModelIndex &i, indexes)
            m_sessionModel->removeRow(i.row());
    }

    // Update the text color in model
    QColor newcolor;
    for (int row = 0; row < m_sessionModel->rowCount(); ++row)
    {
        QBrush newcolor;
        const KColorScheme scheme(QPalette::Normal);
        if (m_sessionModel->data(m_sessionModel->index(row,2), Qt::DisplayRole) == QLatin1String("active"))
            newcolor = scheme.foreground(KColorScheme::PositiveText);
        else if (m_sessionModel->data(m_sessionModel->index(row,2), Qt::DisplayRole) == QLatin1String("closing"))
            newcolor = scheme.foreground(KColorScheme::InactiveText);
        else
            newcolor = scheme.foreground(KColorScheme::NormalText);

        for (int col = 0; col < m_sessionModel->columnCount(); ++col)
            m_sessionModel->setData(m_sessionModel->index(row,col), QVariant(newcolor), Qt::ForegroundRole);
    }
}

void MainWindow::slotRefreshTimerList()
{
    // Updates the timer list
    // qDebug() << "Refreshing timer list...";

    m_timerModel->removeRows(0, m_timerModel->rowCount());

    // Iterate through system unitlist and add timers to the model
    foreach (const SystemdUnit &unit, m_systemUnitsList)
    {
        if (unit.id.endsWith(QLatin1String(".timer")) &&
                unit.load_state != QLatin1String("unloaded")) {
            m_timerModel->appendRow(buildTimerListRow(unit, m_systemUnitsList, sys));
        }
    }

    // Iterate through user unitlist and add timers to the model
    foreach (const SystemdUnit &unit, m_userUnitsList)
    {
        if (unit.id.endsWith(QLatin1String(".timer")) &&
                unit.load_state != QLatin1String("unloaded")) {
            m_timerModel->appendRow(buildTimerListRow(unit, m_userUnitsList, user));
        }
    }

    // Update the left and passed columns
    slotUpdateTimers();

    ui.tblTimers->resizeColumnsToContents();
    ui.tblTimers->sortByColumn(ui.tblTimers->horizontalHeader()->sortIndicatorSection(),
                               ui.tblTimers->horizontalHeader()->sortIndicatorOrder());
}

QList<QStandardItem *> MainWindow::buildTimerListRow(const SystemdUnit &unit, const QVector<SystemdUnit> &list, dbusBus bus)
{
    // Builds a row for the timers list

    QDBusObjectPath path = unit.unit_path;
    QString unitToActivate = getDbusProperty(QStringLiteral("Unit"), sysdTimer, path, bus).toString();

    QDateTime time;
    QIcon icon;
    if (bus == sys)
        icon = QIcon::fromTheme(QStringLiteral("applications-system"));
    else
        icon = QIcon::fromTheme(QStringLiteral("user-identity"));

    // Add the next elapsation point
    qlonglong nextElapseMonotonicMsec = getDbusProperty(QStringLiteral("NextElapseUSecMonotonic"), sysdTimer, path, bus).toULongLong() / 1000;
    qlonglong nextElapseRealtimeMsec = getDbusProperty(QStringLiteral("NextElapseUSecRealtime"), sysdTimer, path, bus).toULongLong() / 1000;
    qlonglong lastTriggerMSec = getDbusProperty(QStringLiteral("LastTriggerUSec"), sysdTimer, path, bus).toULongLong() / 1000;

    if (nextElapseMonotonicMsec == 0)
    {
        // Timer is calendar-based
        time.setMSecsSinceEpoch(nextElapseRealtimeMsec);
    }
    else
    {
        // Timer is monotonic
        time = QDateTime().currentDateTime();
        time = time.addMSecs(nextElapseMonotonicMsec);

        // Get the monotonic system clock
        struct timespec ts;
        if (clock_gettime( CLOCK_MONOTONIC, &ts ) != 0)
            qDebug() << "Failed to get the monotonic system clock!";

        // Convert the monotonic system clock to microseconds
        qlonglong now_mono_usec = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

        // And subtract it.
        time = time.addMSecs(-now_mono_usec/1000);
    }

    QString next = time.toString(QStringLiteral("yyyy.MM.dd hh:mm:ss"));

    QString last;

    // use unit object to get last time for activated service
    int index = list.indexOf(SystemdUnit(unitToActivate));
    if (index != -1)
    {
        qlonglong inactivateExitTimestampMsec =
                getDbusProperty(QStringLiteral("InactiveExitTimestamp"), sysdUnit, list.at(index).unit_path, bus).toULongLong() / 1000;

        if (inactivateExitTimestampMsec == 0)
        {
            // The unit has not run in this boot
            // Use LastTrigger to see if the timer is persistent
            if (lastTriggerMSec == 0)
                last = QStringLiteral("n/a");
            else
            {
                time.setMSecsSinceEpoch(lastTriggerMSec);
                last = time.toString(QStringLiteral("yyyy.MM.dd hh:mm:ss"));
            }
        }
        else
        {
            QDateTime time;
            time.setMSecsSinceEpoch(inactivateExitTimestampMsec);
            last = time.toString(QStringLiteral("yyyy.MM.dd hh:mm:ss"));
        }
    }

    // Set icon for id column
    QStandardItem *id = new QStandardItem(unit.id);
    id->setData(icon, Qt::DecorationRole);

    // Build a row from QStandardItems
    QList<QStandardItem *> row;
    row << id <<
           new QStandardItem(next) <<
           new QStandardItem() <<
           new QStandardItem(last) <<
           new QStandardItem() <<
           new QStandardItem(unitToActivate);

    return row;
}

void MainWindow::updateUnitCount()
{
    QString systemUnits = i18ncp("First part of 'Total: %1, %2, %3'",
                                 "1 unit", "%1 units", m_systemUnitModel->rowCount());
    QString systemActive = i18ncp("Second part of 'Total: %1, %2, %3'",
                                  "1 active", "%1 active", m_noActSystemUnits);
    QString systemDisplayed = i18ncp("Third part of 'Total: %1, %2, %3'",
                                     "1 displayed", "%1 displayed", m_systemUnitFilterModel->rowCount());
    ui.lblUnitCount->setText(i18nc("%1 is '%1 units' and %2 is '%2 active' and %3 is '%3 displayed'",
                                   "Total: %1, %2, %3", systemUnits, systemActive, systemDisplayed));

    QString userUnits = i18ncp("First part of 'Total: %1, %2, %3'",
                               "1 unit", "%1 units", m_userUnitModel->rowCount());
    QString userActive = i18ncp("Second part of 'Total: %1, %2, %3'",
                                "1 active", "%1 active", m_noActUserUnits);
    QString userDisplayed = i18ncp("Third part of 'Total: %1, %2, %3'",
                                   "1 displayed", "%1 displayed", m_userUnitFilterModel->rowCount());
    ui.lblUserUnitCount->setText(i18nc("%1 is '%1 units' and %2 is '%2 active' and %3 is '%3 displayed'",
                                       "Total: %1, %2, %3", userUnits, userActive, userDisplayed));
}

void MainWindow::authServiceAction(const QString &service, const QString &path, const QString &iface, const QString &method, const QList<QVariant> &args)
{
    // Function to call the helper to authenticate a call to systemd over the system DBus

    // Declare a QVariantMap with arguments for the helper
    QVariantMap helperArgs;
    helperArgs[QStringLiteral("service")] = service;
    helperArgs[QStringLiteral("path")] = path;
    helperArgs[QStringLiteral("interface")] = iface;
    helperArgs[QStringLiteral("method")] = method;
    helperArgs[QStringLiteral("argsForCall")] = args;

    // Call the helper. This call causes the debug output: "QDBusArgument: read from a write-only object"
    KAuth::Action serviceAction(QStringLiteral("org.kde.kcontrol.systemdgenie.dbusaction"));
    serviceAction.setHelperId(QStringLiteral("org.kde.kcontrol.systemdgenie"));
    serviceAction.setArguments(helperArgs);

    KAuth::ExecuteJob *job = serviceAction.execute();
    job->exec();

    if (!job->exec())
        displayMsgWidget(KMessageWidget::Error,
                         i18n("Unable to authenticate/execute the action: %1", job->error()));
    else
    {
        qDebug() << "DBus action successful.";
        // KMessageBox::information(this, i18n("DBus action successful."));
    }
}

void MainWindow::slotConfFileContextMenu(const QPoint &pos)
{
    Q_UNUSED(pos)

    // Slot for creating the right-click menu in unitlists
    if (!factory()) {
        return;
    }

    QMenu *popup = static_cast<QMenu *>(factory()->container(QStringLiteral("context_menu_conf"), this));
    popup->popup(QCursor::pos());
}

void MainWindow::slotUnitContextMenu(const QPoint &pos)
{
    Q_UNUSED(pos)

    // Slot for creating the right-click menu in unitlists
    if (!factory()) {
        return;
    }

    QMenu *popup = static_cast<QMenu *>(factory()->container(QStringLiteral("context_menu_units"), this));
    popup->popup(QCursor::pos());
}

void MainWindow::updateActions()
{
    //qDebug() << "Updating actions...";

    if ((ui.tabWidget->currentIndex() == 0 && !ui.tblUnits->selectionModel()->selectedRows(0).isEmpty()) ||
        (ui.tabWidget->currentIndex() == 1 && !ui.tblUserUnits->selectionModel()->selectedRows(0).isEmpty()))
    {
        QTableView *tblView;
        QVector<SystemdUnit> *list;
        dbusBus bus;
        if (ui.tabWidget->currentIndex() == 0) {
            list = &m_systemUnitsList;
            tblView = ui.tblUnits;
            bus = sys;
        } else if (ui.tabWidget->currentIndex() == 1) {
            list = &m_userUnitsList;
            tblView = ui.tblUserUnits;
            bus = user;
        } else {
            m_startUnitAction->setEnabled(false);
            m_stopUnitAction->setEnabled(false);
            m_restartUnitAction->setEnabled(false);
            m_reloadUnitAction->setEnabled(false);
            m_enableUnitAction->setEnabled(false);
            m_disableUnitAction->setEnabled(false);
            m_maskUnitAction->setEnabled(false);
            m_unmaskUnitAction->setEnabled(false);
            m_editUnitFileAction->setEnabled(false);
            return;
        }


        // Find name and object path of unit
        const QString unit = tblView->selectionModel()->selectedRows(0).at(0).data().toString();
        Q_ASSERT(!unit.isEmpty());
        Q_ASSERT(list->contains(SystemdUnit(unit)));
        const QDBusObjectPath pathUnit = list->at(list->indexOf(SystemdUnit(unit))).unit_path;

        // Check capabilities of unit
        bool CanStart = false;
        bool CanStop = false;
        bool CanReload = false;
        //bool CanIsolate = false;
        QString LoadState;
        QString ActiveState;
        QString unitFileState;
        if (!pathUnit.path().isEmpty() &&
            getDbusProperty(QStringLiteral("Test"), sysdUnit, pathUnit, bus).toString() != QLatin1String("invalidIface"))
        {
            // Unit has a Unit DBus object, fetch properties
            LoadState = getDbusProperty(QStringLiteral("LoadState"), sysdUnit, pathUnit, bus).toString();
            ActiveState = getDbusProperty(QStringLiteral("ActiveState"), sysdUnit, pathUnit, bus).toString();
            CanStart = getDbusProperty(QStringLiteral("CanStart"), sysdUnit, pathUnit, bus).toBool();
            CanStop = getDbusProperty(QStringLiteral("CanStop"), sysdUnit, pathUnit, bus).toBool();
            CanReload = getDbusProperty(QStringLiteral("CanReload"), sysdUnit, pathUnit, bus).toBool();
            //CanIsolate = getDbusProperty(QStringLiteral("CanIsolate"), sysdUnit, pathUnit, bus).toBool();

            unitFileState = getDbusProperty(QStringLiteral("UnitFileState"), sysdUnit, pathUnit, bus).toString();
        } else {
            // Get UnitFileState from Manager object.
            unitFileState = callDbusMethod(QStringLiteral("GetUnitFileState"), sysdMgr, bus, QVariantList{unit}).arguments().at(0).toString();
        }

        // Check if unit has a unit file, if not disable editing.
        QString frpath;
        int index = list->indexOf(SystemdUnit(unit));
        if (index != -1) {
            frpath = list->at(index).unit_file;
        }

        bool isUnitSelected = !unit.isEmpty();

        m_startUnitAction->setEnabled(isUnitSelected &&
                                      CanStart &&
                                      ActiveState != QLatin1String("active"));

        m_stopUnitAction->setEnabled(isUnitSelected &&
                                     CanStop &&
                                     ActiveState != QLatin1String("inactive") &&
                                     ActiveState != QLatin1String("failed"));

        m_restartUnitAction->setEnabled(isUnitSelected &&
                                        CanStart &&
                                        ActiveState != QLatin1String("inactive") &&
                                        ActiveState != QLatin1String("failed") &&
                                        !LoadState.isEmpty());

        m_reloadUnitAction->setEnabled(isUnitSelected &&
                                       CanReload &&
                                       ActiveState != QLatin1String("inactive") &&
                                       ActiveState != QLatin1String("failed"));

        m_enableUnitAction->setEnabled(isUnitSelected &&
                                       unitFileState == QLatin1String("disabled"));

        m_disableUnitAction->setEnabled(isUnitSelected &&
                                        unitFileState == QLatin1String("enabled"));

        m_maskUnitAction->setEnabled(isUnitSelected &&
                                     LoadState != QLatin1String("masked"));

        m_unmaskUnitAction->setEnabled(isUnitSelected &&
                                       LoadState == QLatin1String("masked"));

        m_editUnitFileAction->setEnabled(isUnitSelected &&
                                         !frpath.isEmpty());
    } else {
        m_startUnitAction->setEnabled(false);
        m_stopUnitAction->setEnabled(false);
        m_restartUnitAction->setEnabled(false);
        m_reloadUnitAction->setEnabled(false);
        m_enableUnitAction->setEnabled(false);
        m_disableUnitAction->setEnabled(false);
        m_maskUnitAction->setEnabled(false);
        m_unmaskUnitAction->setEnabled(false);
        m_editUnitFileAction->setEnabled(false);
    }

    m_editConfFileAction->setEnabled(ui.tabWidget->currentIndex() == 2 &&
                                     !ui.tblConfFiles->selectionModel()->selectedRows(0).isEmpty());
    m_openManPageAction->setEnabled(ui.tabWidget->currentIndex() == 2 &&
                                   !ui.tblConfFiles->selectionModel()->selectedRows(0).isEmpty());

    m_activateSessionAction->setEnabled(ui.tabWidget->currentIndex() == 3 &&
                                        !ui.tblSessions->selectionModel()->selectedRows(0).isEmpty() &&
                                        ui.tblSessions->selectionModel()->selectedRows(2).at(0).data().toString() != QLatin1String("active"));

    m_terminateSessionAction->setEnabled(ui.tabWidget->currentIndex() == 3 &&
                                         !ui.tblSessions->selectionModel()->selectedRows(0).isEmpty());

    QDBusObjectPath pathSession;
    if (!ui.tblSessions->selectionModel()->selectedRows(0).isEmpty()) {
        pathSession = QDBusObjectPath(ui.tblSessions->selectionModel()->selectedRows(1).at(0).data().toString());
    }
    m_lockSessionAction->setEnabled(ui.tabWidget->currentIndex() == 3 &&
                                   !ui.tblSessions->selectionModel()->selectedRows(0).isEmpty() &&
                                   getDbusProperty(QStringLiteral("Type"), logdSession, pathSession).toString() != QLatin1String("tty"));
}

void MainWindow::setupActions()
{
    KStandardAction::quit(qApp, SLOT(quit()), actionCollection());

    m_refreshAction = new QAction(this);
    m_refreshAction->setText(i18n("&Refresh"));
    m_refreshAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    m_refreshAction->setToolTip(i18nc("@info:tooltip", "Refresh"));
    actionCollection()->setDefaultShortcut(m_refreshAction, Qt::Key_F5);
    actionCollection()->addAction(QStringLiteral("refresh"), m_refreshAction);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::slotRefreshAll);

    QSignalMapper *unitActionMapper = new QSignalMapper(this);

    m_startUnitAction = new QAction(this);
    m_startUnitAction->setText(i18n("Start Unit"));
    m_startUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("kt-start")));
    actionCollection()->addAction(QStringLiteral("start-unit"), m_startUnitAction);
    connect(m_startUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_startUnitAction, QStringLiteral("StartUnit"));

    m_stopUnitAction = new QAction(this);
    m_stopUnitAction->setText(i18n("Stop Unit"));
    m_stopUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("kt-stop")));
    actionCollection()->addAction(QStringLiteral("stop-unit"), m_stopUnitAction);
    connect(m_stopUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_stopUnitAction, QStringLiteral("StopUnit"));

    m_reloadUnitAction = new QAction(this);
    m_reloadUnitAction->setText(i18n("Reload Unit"));
    m_reloadUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    actionCollection()->addAction(QStringLiteral("reload-unit"), m_reloadUnitAction);
    connect(m_reloadUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_reloadUnitAction, QStringLiteral("ReloadUnit"));

    m_restartUnitAction = new QAction(this);
    m_restartUnitAction->setText(i18n("Restart Unit"));
    m_restartUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("start-over")));
    actionCollection()->addAction(QStringLiteral("restart-unit"), m_restartUnitAction);
    connect(m_restartUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_restartUnitAction, QStringLiteral("RestartUnit"));

    m_enableUnitAction = new QAction(this);
    m_enableUnitAction->setText(i18n("Enable Unit"));
    m_enableUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    actionCollection()->addAction(QStringLiteral("enable-unit"), m_enableUnitAction);
    connect(m_enableUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_enableUnitAction, QStringLiteral("EnableUnitFiles"));

    m_disableUnitAction = new QAction(this);
    m_disableUnitAction->setText(i18n("Disable Unit"));
    m_disableUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("document-close")));
    actionCollection()->addAction(QStringLiteral("disable-unit"), m_disableUnitAction);
    connect(m_disableUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_disableUnitAction, QStringLiteral("DisableUnitFiles"));

    m_maskUnitAction = new QAction(this);
    m_maskUnitAction->setText(i18n("Mask Unit"));
    m_maskUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("password-show-off")));
    actionCollection()->addAction(QStringLiteral("mask-unit"), m_maskUnitAction);
    connect(m_maskUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_maskUnitAction, QStringLiteral("MaskUnitFiles"));

    m_unmaskUnitAction = new QAction(this);
    m_unmaskUnitAction->setText(i18n("Unmask Unit"));
    m_unmaskUnitAction->setIcon(QIcon::fromTheme(QStringLiteral("password-show-on")));
    actionCollection()->addAction(QStringLiteral("unmask-unit"), m_unmaskUnitAction);
    connect(m_unmaskUnitAction, &QAction::triggered, unitActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    unitActionMapper->setMapping(m_unmaskUnitAction, QStringLiteral("UnmaskUnitFiles"));

    connect(unitActionMapper, static_cast<void (QSignalMapper::*)(const QString&)>(&QSignalMapper::mapped),
            this, &MainWindow::slotExecuteUnitAction);

    QSignalMapper *systemDaemonMapper = new QSignalMapper(this);

    m_reloadSystemDaemonAction = new QAction(this);
    m_reloadSystemDaemonAction->setText(i18n("Re&load systemd"));
    m_reloadSystemDaemonAction->setToolTip(i18nc("@info:tooltip", "Click to reload all unit files"));
    m_reloadSystemDaemonAction->setIcon(QIcon::fromTheme(QStringLiteral("configure-shortcuts")));
    actionCollection()->addAction(QStringLiteral("reload-daemon-system"), m_reloadSystemDaemonAction);
    connect(m_reloadSystemDaemonAction, &QAction::triggered, systemDaemonMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    systemDaemonMapper->setMapping(m_reloadSystemDaemonAction, QStringLiteral("Reload"));

    m_reexecSystemDaemonAction = new QAction(this);
    m_reexecSystemDaemonAction->setText(i18n("Re-e&xecute systemd"));
    m_reexecSystemDaemonAction->setToolTip(i18nc("@info:tooltip", "Click to re-execute the systemd daemon"));
    m_reexecSystemDaemonAction->setIcon(QIcon::fromTheme(QStringLiteral("configure-shortcuts")));
    actionCollection()->addAction(QStringLiteral("reexec-daemon-system"), m_reexecSystemDaemonAction);
    connect(m_reexecSystemDaemonAction, &QAction::triggered, systemDaemonMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    systemDaemonMapper->setMapping(m_reexecSystemDaemonAction, QStringLiteral("Reexecute"));

    connect(systemDaemonMapper, static_cast<void (QSignalMapper::*)(const QString&)>(&QSignalMapper::mapped),
            this, &MainWindow::slotExecuteSystemDaemonAction);

    QSignalMapper *userDaemonMapper = new QSignalMapper(this);

    m_reloadUserDaemonAction = new QAction(this);
    m_reloadUserDaemonAction->setText(i18n("Re&load user systemd"));
    m_reloadUserDaemonAction->setToolTip(i18nc("@info:tooltip", "Click to reload all user unit files"));
    m_reloadUserDaemonAction->setIcon(QIcon::fromTheme(QStringLiteral("user")));
    actionCollection()->addAction(QStringLiteral("reload-daemon-user"), m_reloadUserDaemonAction);
    connect(m_reloadUserDaemonAction, &QAction::triggered, userDaemonMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    userDaemonMapper->setMapping(m_reloadUserDaemonAction, QStringLiteral("Reload"));

    m_reexecUserDaemonAction = new QAction(this);
    m_reexecUserDaemonAction->setText(i18n("Re-e&xecute user systemd"));
    m_reexecUserDaemonAction->setToolTip(i18nc("@info:tooltip", "Click to re-execute the user systemd daemon"));
    m_reexecUserDaemonAction->setIcon(QIcon::fromTheme(QStringLiteral("user")));
    actionCollection()->addAction(QStringLiteral("reexec-daemon-user"), m_reexecUserDaemonAction);
    connect(m_reexecUserDaemonAction, &QAction::triggered, userDaemonMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    userDaemonMapper->setMapping(m_reexecUserDaemonAction, QStringLiteral("Reexecute"));

    connect(userDaemonMapper, static_cast<void (QSignalMapper::*)(const QString&)>(&QSignalMapper::mapped),
            this, &MainWindow::slotExecuteUserDaemonAction);

    m_editUnitFileAction = new QAction(this);
    m_editUnitFileAction->setText(i18n("Edit Unit File"));
    m_editUnitFileAction->setIcon(QIcon::fromTheme(QStringLiteral("editor")));
    actionCollection()->addAction(QStringLiteral("edit-unitfile"), m_editUnitFileAction);
    connect(m_editUnitFileAction, &QAction::triggered, this, &MainWindow::slotEditUnitFile);

    m_editConfFileAction = new QAction(this);
    m_editConfFileAction->setText(i18n("Edit Configuration File"));
    m_editConfFileAction->setIcon(QIcon::fromTheme(QStringLiteral("editor")));
    actionCollection()->addAction(QStringLiteral("edit-conffile"), m_editConfFileAction);
    connect(m_editConfFileAction, &QAction::triggered, this, &MainWindow::slotEditConfFile);

    m_openManPageAction = new QAction(this);
    m_openManPageAction->setText(i18n("Open Man Page"));
    m_openManPageAction->setIcon(QIcon::fromTheme(QStringLiteral("help-contents")));
    actionCollection()->addAction(QStringLiteral("open-manpage"), m_openManPageAction);
    connect(m_openManPageAction, &QAction::triggered, this, &MainWindow::slotOpenManPage);

    QSignalMapper *sessionActionMapper = new QSignalMapper(this);

    m_activateSessionAction = new QAction(this);
    m_activateSessionAction->setText(i18n("Activate Session"));
    m_activateSessionAction->setIcon(QIcon::fromTheme(QStringLiteral("kt-start")));
    actionCollection()->addAction(QStringLiteral("activate-session"), m_activateSessionAction);
    connect(m_activateSessionAction, &QAction::triggered, sessionActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    sessionActionMapper->setMapping(m_activateSessionAction, QStringLiteral("Activate"));

    m_terminateSessionAction = new QAction(this);
    m_terminateSessionAction->setText(i18n("Terminate Session"));
    m_terminateSessionAction->setIcon(QIcon::fromTheme(QStringLiteral("kt-remove")));
    actionCollection()->addAction(QStringLiteral("terminate-session"), m_terminateSessionAction);
    connect(m_terminateSessionAction, &QAction::triggered, sessionActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    sessionActionMapper->setMapping(m_terminateSessionAction, QStringLiteral("Terminate"));

    m_lockSessionAction = new QAction(this);
    m_lockSessionAction->setText(i18n("Lock Session"));
    m_lockSessionAction->setIcon(QIcon::fromTheme(QStringLiteral("lock")));
    actionCollection()->addAction(QStringLiteral("lock-session"), m_lockSessionAction);
    connect(m_lockSessionAction, &QAction::triggered, sessionActionMapper, static_cast<void (QSignalMapper::*)()>(&QSignalMapper::map));
    sessionActionMapper->setMapping(m_lockSessionAction, QStringLiteral("Lock"));

    connect(sessionActionMapper, static_cast<void (QSignalMapper::*)(const QString&)>(&QSignalMapper::mapped),
            this, &MainWindow::slotExecuteSessionAction);

    updateActions();
}

void MainWindow::slotOpenManPage()
{
    if (ui.tblConfFiles->selectionModel()->selectedRows(0).isEmpty()) {
        return;
    }

    QModelIndex index = ui.tblConfFiles->selectionModel()->selectedRows(0).at(0);
    Q_ASSERT(index.isValid());

    const QString manPage = m_confFileList.at(m_confFileList.indexOf(conffile(index.data().toString()))).manPage;

    const QString KHelpCenterExec = QStandardPaths::findExecutable(QStringLiteral("khelpcenter"));
    if (KHelpCenterExec.isEmpty()) {
        KMessageBox::error(this, i18n("KHelpCenter executable not found in path. Please install KHelpCenter to view man pages."));
        return;
    }

    QProcess *helpcenter = new QProcess(this);
    helpcenter->start(KHelpCenterExec, QStringList{QStringLiteral("man:/%1").arg(manPage)});
}

void MainWindow::slotExecuteUnitAction(const QString &method)
{
    QTableView *tblView;
    if (ui.tabWidget->currentIndex() == 0) {
         tblView = ui.tblUnits;
    } else {
        tblView = ui.tblUserUnits;
    }

    if (tblView->selectionModel()->selectedRows(0).isEmpty()) {
        return;
    }

    QModelIndex index = tblView->selectionModel()->selectedRows(0).at(0);
    Q_ASSERT(index.isValid());

    QString unit = index.data().toString();
    //qDebug() << "unit:" << unit;

    QVariantList args;
    if (method == QLatin1String("EnableUnitFiles") ||
        method == QLatin1String("MaskUnitFiles")) {
        args << QVariant(QStringList{unit}) << false << true;
    } else if (method == QLatin1String("DisableUnitFiles") ||
               method == QLatin1String("UnmaskUnitFiles")) {
        args << QVariant(QStringList{unit}) << false;
    } else {
        args = QVariantList{unit, QStringLiteral("replace")};
    }

    if (ui.tabWidget->currentIndex() == 0) {
        authServiceAction(connSystemd, pathSysdMgr, ifaceMgr, method, args);
    } else if (ui.tabWidget->currentIndex() == 1) {
        // user unit
        callDbusMethod(method, sysdMgr, user, args);
    }
}

void MainWindow::slotExecuteSessionAction(const QString &method)
{
    if (ui.tblSessions->selectionModel()->selectedRows(0).isEmpty()) {
        return;
    }

    QModelIndex index = ui.tblSessions->selectionModel()->selectedRows(0).at(0);
    Q_ASSERT(index.isValid());

    QDBusObjectPath pathSession = QDBusObjectPath(ui.tblSessions->selectionModel()->selectedRows(1).at(0).data().toString());

    authServiceAction(connLogind, pathSession.path(), ifaceSession, method, QVariantList());
}

void MainWindow::slotExecuteSystemDaemonAction(const QString &method)
{
    // Execute the DBus actions
    if (!method.isEmpty()) {
        authServiceAction(connSystemd, pathSysdMgr, ifaceMgr, method, QVariantList());
    }
}

void MainWindow::slotExecuteUserDaemonAction(const QString &method)
{
    if (!method.isEmpty())
    {
        callDbusMethod(method, sysdMgr, user, QVariantList());
        if (QRegularExpression(QStringLiteral("EnableUnitFiles|DisableUnitFiles|MaskUnitFiles|UnmaskUnitFiles")).match(method).hasMatch()) {
            callDbusMethod(QStringLiteral("Reload"), sysdMgr, user);
        }
    }
}

void MainWindow::slotRefreshAll()
{
    qDebug() << "Refresh";

    foreach (const SystemdUnit &u, m_systemUnitsList) {
        if (u.unit_file.isEmpty())
            qDebug() << "empty:" << u.id;
    }
}

void MainWindow::slotSessionContextMenu(const QPoint &pos)
{
    // Slot for creating the right-click menu in the session list

    Q_UNUSED(pos)

    if (!factory()) {
        return;
    }

    QMenu *popup = static_cast<QMenu *>(factory()->container(QStringLiteral("context_menu_session"), this));
    popup->popup(QCursor::pos());
}


bool MainWindow::eventFilter(QObject *obj, QEvent* event)
{
    // Eventfilter for catching mouse move events over session list
    // Used for dynamically generating tooltips

    if (event->type() == QEvent::MouseMove && obj->parent()->objectName() == QLatin1String("tblSessions"))
    {
        // Session list
        QMouseEvent *me = static_cast<QMouseEvent*>(event);
        QModelIndex inSessionModel = ui.tblSessions->indexAt(me->pos());
        if (!inSessionModel.isValid())
            return false;

        if (m_sessionModel->itemFromIndex(inSessionModel)->row() != lastSessionRowChecked)
        {
            // Cursor moved to a different row. Only build tooltips when moving
            // cursor to a new row to avoid excessive DBus calls.

            QString selSession = ui.tblSessions->model()->index(ui.tblSessions->indexAt(me->pos()).row(),0).data().toString();
            QDBusObjectPath spath = QDBusObjectPath(ui.tblSessions->model()->index(ui.tblSessions->indexAt(me->pos()).row(),1).data().toString());

            QString toolTipText;
            toolTipText.append(QStringLiteral("<FONT COLOR=white>"));
            toolTipText.append(QStringLiteral("<b>%1</b><hr>").arg(selSession));

            // Use the DBus interface to get session properties
            if (getDbusProperty(QStringLiteral("test"), logdSession, spath) != QLatin1String("invalidIface"))
            {
                // Session has a valid session DBus object
                toolTipText.append(i18n("<b>VT:</b> %1", getDbusProperty(QStringLiteral("VTNr"), logdSession, spath).toString()));

                QString remoteHost = getDbusProperty(QStringLiteral("RemoteHost"), logdSession, spath).toString();
                if (getDbusProperty(QStringLiteral("Remote"), logdSession, spath).toBool())
                {
                    toolTipText.append(i18n("<br><b>Remote host:</b> %1", remoteHost));
                    toolTipText.append(i18n("<br><b>Remote user:</b> %1", getDbusProperty(QStringLiteral("RemoteUser"), logdSession, spath).toString()));
                }
                toolTipText.append(i18n("<br><b>Service:</b> %1", getDbusProperty(QStringLiteral("Service"), logdSession, spath).toString()));
                toolTipText.append(i18n("<br><b>Leader (PID):</b> %1", getDbusProperty(QStringLiteral("Leader"), logdSession, spath).toString()));

                QString type = getDbusProperty(QStringLiteral("Type"), logdSession, spath).toString();
                toolTipText.append(i18n("<br><b>Type:</b> %1", type));
                if (type == QLatin1String("x11"))
                    toolTipText.append(i18n(" (display %1)", getDbusProperty(QStringLiteral("Display"), logdSession, spath).toString()));
                else if (type == QLatin1String("tty"))
                {
                    QString path, tty = getDbusProperty(QStringLiteral("TTY"), logdSession, spath).toString();
                    if (!tty.isEmpty())
                        path = tty;
                    else if (!remoteHost.isEmpty())
                        path = QStringLiteral("%1@%2").arg(getDbusProperty(QStringLiteral("Name"), logdSession, spath).toString()).arg(remoteHost);
                    toolTipText.append(QStringLiteral(" (%1)").arg(path));
                }
                toolTipText.append(i18n("<br><b>Class:</b> %1", getDbusProperty(QStringLiteral("Class"), logdSession, spath).toString()));
                toolTipText.append(i18n("<br><b>State:</b> %1", getDbusProperty(QStringLiteral("State"), logdSession, spath).toString()));
                toolTipText.append(i18n("<br><b>Scope:</b> %1", getDbusProperty(QStringLiteral("Scope"), logdSession, spath).toString()));


                toolTipText.append(i18n("<br><b>Created: </b>"));
                if (getDbusProperty(QStringLiteral("Timestamp"), logdSession, spath).toULongLong() == 0)
                    toolTipText.append(QStringLiteral("n/a"));
                else
                {
                    QDateTime time;
                    time.setMSecsSinceEpoch(getDbusProperty(QStringLiteral("Timestamp"), logdSession, spath).toULongLong()/1000);
                    toolTipText.append(time.toString());
                }
            }

            toolTipText.append(QStringLiteral("</FONT"));
            m_sessionModel->itemFromIndex(inSessionModel)->setToolTip(toolTipText);

            lastSessionRowChecked = m_sessionModel->itemFromIndex(inSessionModel)->row();
            return true;

        } // Row was different
    }
    return false;
    // return true;
}

void MainWindow::slotSystemSystemdReloading(bool status)
{
    if (status) {
        qDebug() << "System daemon reloading...";
    } else {
        qDebug() << "System daemon reloaded";
        m_lblLog->setText(i18n("%1: Systemd daemon reloaded...", QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))));
        slotRefreshUnitsList(false, sys);
    }
}

void MainWindow::slotUserSystemdReloading(bool status)
{
    if (status) {
        qDebug() << "User daemon reloading...";
    } else {
        qDebug() << "User daemon reloaded";
        m_lblLog->setText(i18n("%1: User daemon reloaded...", QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))));
        slotRefreshUnitsList(false, user);
    }
}

/*
void MainWindow::slotSystemUnitNew(const QString &id, const QDBusObjectPath &path)
{
    //qDebug() << "SystemUnitNew: " << id << path.path();
}

void MainWindow::slotSystemUnitRemoved(const QString &id, const QDBusObjectPath &path)
{
    //qDebug() << "SystemUnitRemoved: " << id << path.path();
}
*/

void MainWindow::slotSystemJobNew(uint id, const QDBusObjectPath &path, const QString &unit)
{
    qDebug() << "SystemJobNew: " << id << path.path() << unit;
}

void MainWindow::slotSystemJobRemoved(uint id, const QDBusObjectPath &path, const QString &unit, const QString &result)
{
    qDebug() << "SystmJobRemoved: " << id << path.path() << unit << result;
    slotRefreshUnitsList(false, sys);
}

void MainWindow::slotUserJobNew(uint id, const QDBusObjectPath &path, const QString &unit)
{
    qDebug() << "UserJobNew: " << id << path.path() << unit;
}

void MainWindow::slotUserJobRemoved(uint id, const QDBusObjectPath &path, const QString &unit, const QString &result)
{
    qDebug() << "UserJobRemoved: " << id << path.path() << unit << result;
    slotRefreshUnitsList(false, user);
}

void MainWindow::slotSystemUnitFilesChanged()
{
    //qDebug() << "System unitFilesChanged";
    slotRefreshUnitsList(false, sys);
    m_lblLog->setText(i18n("%1: System units changed...", QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))));
}

void MainWindow::slotUserUnitFilesChanged()
{
    //qDebug() << "User unitFilesChanged";
    slotRefreshUnitsList(false, user);
}

void MainWindow::slotSystemPropertiesChanged(const QString &iface, const QVariantMap &changedProps, const QStringList &invalidatedProps)
{
    Q_UNUSED(changedProps)
    Q_UNUSED(invalidatedProps)

    qDebug() << "systemPropertiesChanged:" << iface; // << changedProps << invalidatedProps;
    slotRefreshUnitsList(false, sys);
}

void MainWindow::slotUserPropertiesChanged(const QString &iface, const QVariantMap &changedProps, const QStringList &invalidatedProps)
{
    Q_UNUSED(changedProps)
    Q_UNUSED(invalidatedProps)

    qDebug() << "userPropertiesChanged:" << iface; // << changedProps << invalidatedProps;
    slotRefreshUnitsList(false, user);
}

void MainWindow::slotLogindPropertiesChanged(const QString &iface, const QVariantMap &changedProps, const QStringList &invalidatedProps)
{
    Q_UNUSED(iface)
    Q_UNUSED(changedProps)
    Q_UNUSED(invalidatedProps)

    // qDebug() << "Logind properties changed on iface " << iface_name;
    slotRefreshSessionList();
}

void MainWindow::slotLeSearchUnitChanged(QString term)
{
    if (QObject::sender()->objectName() == QLatin1String("leSearchUnit"))
    {
        m_systemUnitFilterModel->addFilterRegExp(unitName, term);
        m_systemUnitFilterModel->invalidate();
        ui.tblUnits->sortByColumn(ui.tblUnits->horizontalHeader()->sortIndicatorSection(),
                                  ui.tblUnits->horizontalHeader()->sortIndicatorOrder());
    }
    else if (QObject::sender()->objectName() == QLatin1String("leSearchUserUnit"))
    {
        m_userUnitFilterModel->addFilterRegExp(unitName, term);
        m_userUnitFilterModel->invalidate();
        ui.tblUserUnits->sortByColumn(ui.tblUserUnits->horizontalHeader()->sortIndicatorSection(),
                                      ui.tblUserUnits->horizontalHeader()->sortIndicatorOrder());
    }
    updateUnitCount();
}

void MainWindow::slotUpdateTimers()
{
    // Updates the left and passed columns in the timers list
    for (int row = 0; row < m_timerModel->rowCount(); ++row)
    {
        QDateTime next = m_timerModel->index(row, 1).data().toDateTime();
        QDateTime last = m_timerModel->index(row, 3).data().toDateTime();
        QDateTime current = QDateTime().currentDateTime();
        qlonglong leftSecs = current.secsTo(next);
        qlonglong passedSecs = last.secsTo(current);

        QString left;
        if (leftSecs >= 31536000)
            left = QStringLiteral("%1 years").arg(leftSecs / 31536000);
        else if (leftSecs >= 604800)
            left = QStringLiteral("%1 weeks").arg(leftSecs / 604800);
        else if (leftSecs >= 86400)
            left = QStringLiteral("%1 days").arg(leftSecs / 86400);
        else if (leftSecs >= 3600)
            left = QStringLiteral("%1 hr").arg(leftSecs / 3600);
        else if (leftSecs >= 60)
            left = QStringLiteral("%1 min").arg(leftSecs / 60);
        else if (leftSecs < 0)
            left = QStringLiteral("0 s");
        else
            left = QStringLiteral("%1 s").arg(leftSecs);
        m_timerModel->setData(m_timerModel->index(row, 2), left);

        QString passed;
        if (m_timerModel->index(row, 3).data() == QStringLiteral("n/a"))
            passed = QStringLiteral("n/a");
        else if (passedSecs >= 31536000)
            passed = QStringLiteral("%1 years").arg(passedSecs / 31536000);
        else if (passedSecs >= 604800)
            passed = QStringLiteral("%1 weeks").arg(passedSecs / 604800);
        else if (passedSecs >= 86400)
            passed = QStringLiteral("%1 days").arg(passedSecs / 86400);
        else if (passedSecs >= 3600)
            passed = QStringLiteral("%1 hr").arg(passedSecs / 3600);
        else if (passedSecs >= 60)
            passed = QStringLiteral("%1 min").arg(passedSecs / 60);
        else if (passedSecs < 0)
            passed = QStringLiteral("0 s");
        else
            passed = QStringLiteral("%1 s").arg(passedSecs);
        m_timerModel->setData(m_timerModel->index(row, 4), passed);
    }
}

void MainWindow::slotEditUnitFile()
{
    QTableView *tblView;
    QVector<SystemdUnit> *list;
    if (ui.tabWidget->currentIndex() == 0) {
         tblView = ui.tblUnits;
         list = &m_systemUnitsList;
    } else {
        tblView = ui.tblUserUnits;
        list = &m_userUnitsList;
    }

    if (tblView->selectionModel()->selectedRows(0).isEmpty()) {
        return;
    }

    QModelIndex index = tblView->selectionModel()->selectedRows(0).at(0);
    Q_ASSERT(index.isValid());
    QString file = list->at(list->indexOf(SystemdUnit(index.data().toString()))).unit_file;

    openEditor(file);
}

void MainWindow::slotEditConfFile()
{
    if (ui.tblConfFiles->selectionModel()->selectedRows(0).isEmpty()) {
        return;
    }

    QModelIndex index = ui.tblConfFiles->selectionModel()->selectedRows(0).at(0);
    Q_ASSERT(index.isValid());

    QString file = index.data().toString();

    openEditor(file);
}

void MainWindow::openEditor(const QString &file)
{
    // Using a QPointer is safer for modal dialogs.
    // See: https://blogs.kde.org/node/3919
    QPointer<QDialog> dlgEditor = new QDialog(this);
    dlgEditor->setWindowTitle(i18n("Editing %1", file.section(QLatin1Char('/'), -1)));

    QPlainTextEdit *textEdit = new QPlainTextEdit(dlgEditor);
    textEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Save |
                                                       QDialogButtonBox::Cancel,
                                                       dlgEditor);
    connect(buttonBox, SIGNAL(accepted()), dlgEditor, SLOT(accept()));
    connect(buttonBox, SIGNAL(rejected()), dlgEditor, SLOT(reject()));
    buttonBox->button(QDialogButtonBox::Save)->setEnabled(false);

    QLabel *lblFilepath = new QLabel(QStringLiteral("Editing file: %1").arg(file));
    QVBoxLayout *vlayout = new QVBoxLayout(dlgEditor);
    vlayout->addWidget(lblFilepath);
    vlayout->addWidget(textEdit);
    vlayout->addWidget(buttonBox);

    // Read contents of unit file.
    QFile f(file);
    if (!f.open(QFile::ReadOnly | QFile::Text)) {
        displayMsgWidget(KMessageWidget::Error,
                         i18n("Failed to open the unit file:\n%1", file));
        return;
    }
    QTextStream in(&f);
    textEdit->setPlainText(in.readAll());
    textEdit->setMinimumSize(500,300);

    connect(textEdit, &QPlainTextEdit::textChanged, [=]{
        buttonBox->button(QDialogButtonBox::Save)->setEnabled(true);
    });

    if (dlgEditor->exec() == QDialog::Accepted) {

        // Declare a QVariantMap with arguments for the helper.
        QVariantMap helperArgs;
        helperArgs[QStringLiteral("file")] = file;
        helperArgs[QStringLiteral("contents")] = textEdit->toPlainText();

        // Create the action.
        KAuth::Action action(QStringLiteral("org.kde.kcontrol.systemdgenie.saveunitfile"));
        action.setHelperId(QStringLiteral("org.kde.kcontrol.systemdgenie"));
        action.setArguments(helperArgs);

        KAuth::ExecuteJob *job = action.execute();
        if (!job->exec()) {
            displayMsgWidget(KMessageWidget::Error,
                             i18n("Unable to authenticate/execute the action: %1", job->error()));
        } else {
            //displayMsgWidget(KMessageWidget::Positive,
            //                 i18n("Unit file successfully written."));
            m_lblLog->setText(i18n("%1: %2 successfully written...", QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), file.section(QLatin1Char('/'), -1)));
        }
    }
    delete dlgEditor;
}

QVector<SystemdUnit> MainWindow::getUnitsFromDbus(dbusBus bus)
{
    // get an updated list of units via dbus

    QVector<SystemdUnit> list;
    QVector<unitfile> unitfileslist;
    QDBusMessage dbusreply;

    dbusreply = callDbusMethod(QStringLiteral("ListUnits"), sysdMgr, bus);

    if (dbusreply.type() == QDBusMessage::ReplyMessage)
    {

        const QDBusArgument argUnits = dbusreply.arguments().at(0).value<QDBusArgument>();
        if (argUnits.currentType() == QDBusArgument::ArrayType)
        {
            argUnits.beginArray();
            while (!argUnits.atEnd())
            {
                SystemdUnit unit;
                argUnits >> unit;
                if (unit.following.isEmpty()) {
                    list.append(unit);
                }
            }
            argUnits.endArray();
        }
        //qDebug() << "Added " << list.size() << " units on bus " << bus;

        // Get a list of unit files
        dbusreply = callDbusMethod(QStringLiteral("ListUnitFiles"), sysdMgr, bus);
        const QDBusArgument argUnitFiles = dbusreply.arguments().at(0).value<QDBusArgument>();
        argUnitFiles.beginArray();
        while (!argUnitFiles.atEnd())
        {
            unitfile u;
            argUnitFiles.beginStructure();
            argUnitFiles >> u.name >> u.status;
            argUnitFiles.endStructure();
            unitfileslist.append(u);
        }
        argUnitFiles.endArray();

        // Add unloaded units to the list
        foreach (const unitfile &f, unitfileslist)
        {
            int index = list.indexOf(SystemdUnit(f.name.section(QLatin1Char('/'), -1)));
            if (index > -1)
            {
                // The unit was already in the list, add unit file and its status
                list[index].unit_file = f.name;
                list[index].unit_file_status = f.status;
            }
            else
            {
                // Unit not in the list, add it.
                QFile unitfile(f.name);
                if (unitfile.symLinkTarget().isEmpty())
                {
                    SystemdUnit unit;
                    unit.id = f.name.section(QLatin1Char('/'), -1);
                    unit.load_state = QStringLiteral("unloaded");
                    unit.active_state = QLatin1Char('-');
                    unit.sub_state = QLatin1Char('-');
                    unit.unit_file = f.name;
                    unit.unit_file_status= f.status;
                    list.append(unit);
                }
            }
        }
        // qDebug() << "Added " << tal << " units from files on bus " << bus;

    }

    return list;
}

QVariant MainWindow::getDbusProperty(QString prop, dbusIface ifaceName, QDBusObjectPath path, dbusBus bus)
{
    // qDebug() << "Fetching property" << prop << ifaceName << path.path() << "on bus" << bus;
    QString conn, ifc;
    QDBusConnection abus(m_systemBus);
    if (bus == user)
        abus = QDBusConnection::connectToBus(m_userBusPath, connSystemd);

    if (ifaceName == sysdMgr)
    {
        conn = connSystemd;
        ifc = ifaceMgr;
    }
    else if (ifaceName == sysdUnit)
    {
        conn = connSystemd;
        ifc = ifaceUnit;
    }
    else if (ifaceName == sysdTimer)
    {
        conn = connSystemd;
        ifc = ifaceTimer;
    }
    else if (ifaceName == logdSession)
    {
        conn = connLogind;
        ifc = ifaceSession;
    }
    QVariant r;
    QDBusInterface *iface = new QDBusInterface (conn, path.path(), ifc, abus, this);
    if (iface->isValid())
    {
        r = iface->property(prop.toLatin1());
        delete iface;
        return r;
    }
    qDebug() << "Interface" << ifc << "invalid for" << path.path();
    return QVariant(QStringLiteral("invalidIface"));
}

QDBusMessage MainWindow::callDbusMethod(QString method, dbusIface ifaceName, dbusBus bus, const QList<QVariant> &args)
{
    //qDebug() << "Calling method" << method << "with iface" << ifaceName << "on bus" << bus;

    QDBusConnection abus(m_systemBus);
    if (bus == user)
        abus = QDBusConnection::connectToBus(m_userBusPath, connSystemd);

    QDBusInterface *iface;
    if (ifaceName == sysdMgr)
        iface = new QDBusInterface(connSystemd, pathSysdMgr, ifaceMgr, abus, this);
    else if (ifaceName == logdMgr)
        iface = new QDBusInterface(connLogind, pathLogdMgr, ifaceLogdMgr, abus, this);

    QDBusMessage msg;
    if (iface->isValid())
    {
        if (args.isEmpty())
            msg = iface->call(QDBus::AutoDetect, method);
        else
            msg = iface->callWithArgumentList(QDBus::AutoDetect, method, args);
        delete iface;
        if (msg.type() == QDBusMessage::ErrorMessage)
            qDebug() << "DBus method call failed: " << msg.errorMessage();
    }
    else
    {
        qDebug() << "Invalid DBus interface on bus" << bus;
        delete iface;
    }
    return msg;
}

void MainWindow::displayMsgWidget(KMessageWidget::MessageType type, QString msg)
{
    KMessageWidget *msgWidget = new KMessageWidget;
    msgWidget->setText(msg);
    msgWidget->setMessageType(type);
    ui.verticalLayoutMsg->insertWidget(0, msgWidget);
    msgWidget->animatedShow();
}

