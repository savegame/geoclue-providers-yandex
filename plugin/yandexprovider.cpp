/*
    Copyright (C) 2016 Jolla Ltd.
    Contact: Chris Adams <chris.adams@jollamobile.com>
    Copyright (C) 2020 Chupligin Sergey <neochapay@gmail.com>

    This file is part of geoclue-yandex based on geoclue-mlsdb.

    Geoclue-mlsdb is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License.
*/

#include "yandexprovider.h"

#include "yandexonlinelocator.h"
#include "geoclue_adaptor.h"
#include "position_adaptor.h"

#include <QtGlobal>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfoList>
#include <QtCore/QSharedPointer>
#include <QtCore/QList>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>

#include <qofonoextcellwatcher.h>

#include <strings.h>
#include <sys/time.h>

namespace {
    YandexProvider *staticProvider = 0;
    const int MinimumCalculatedAccuracy = 2500; // 2500 metres - arbitrary but large, manual cell-based triangulation is error-prone.
    const int QuitIdleTime = 30000;             // 30s, plugin process will kill itself if no clients request position updates in this time
    const int FixTimeout = 30000;               // 30s, status will change from Available to Acquiring if no position update can be calculated in this time since last update.
    const quint32 MinimumInterval = 10000;      // 10s, the shortest interval at which the plugin will recalculate position since last update
    const quint32 ReuseInterval = 30000;        // 30s, the amount of time a previously calculated position updates will be re-used for without recalculating new position
    const quint32 FallbackInterval = 120000;    // 120s, the amount of time a previously calculated position update with high accuracy can supercede a newly calculated low-accuracy position
    const QString LocationSettingsDir = QStringLiteral("/etc/location/");
    const QString LocationSettingsFile = QStringLiteral("/etc/location/location.conf");
    const QString LocationSettingsEnabledKey = QStringLiteral("location/enabled");
    const QString LocationSettingsMlsEnabledKey = QStringLiteral("location/mls/enabled");
    const QString LocationSettingsMlsOnlineEnabledKey = QStringLiteral("location/mls/online_enabled");
    const QString LocationSettingsOldMlsEnabledKey = QStringLiteral("location/cell_id_positioning_enabled"); // deprecated key
    const QString LocationSettingsDataSourceOnlineAllowedKey = QStringLiteral("location/allowed_data_sources/online");
    const QString LocationSettingsDataSourceCellDataAllowedKey = QStringLiteral("location/allowed_data_sources/cell_data");
    const QString LocationSettingsDataSourceWlanDataAllowedKey = QStringLiteral("location/allowed_data_sources/wlan_data");
}

QDBusArgument &operator<<(QDBusArgument &argument, const Accuracy &accuracy)
{
    const qint32 GeoclueAccuracyLevelPostalcode = 4;

    argument.beginStructure();
    argument << GeoclueAccuracyLevelPostalcode << accuracy.horizontal() << accuracy.vertical();
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Accuracy &accuracy)
{
    qint32 level;
    double a;

    argument.beginStructure();
    argument >> level;
    argument >> a;
    accuracy.setHorizontal(a);
    argument >> a;
    accuracy.setVertical(a);
    argument.endStructure();
    return argument;
}

YandexProvider::YandexProvider(QObject *parent)
:   QObject(parent),
    m_positioningEnabled(false),
    m_cellDataAllowed(false),
    m_positioningStarted(false),
    m_status(StatusUnavailable),
    m_mlsdbOnlineLocator(0),
    m_onlinePositioningEnabled(false),
    m_onlineDataAllowed(false),
    m_wlanDataAllowed(false),
    m_cellWatcher(Q_NULLPTR),
    m_signalUpdateCell(false),
    m_signalUpdateWlan(false)
{
    if (staticProvider)
        qFatal("Only a single instance of MlsdbProvider is supported.");

    qRegisterMetaType<Location>();
    qDBusRegisterMetaType<Accuracy>();

    staticProvider = this;

    connect(&m_locationSettingsWatcher, &QFileSystemWatcher::fileChanged,
            this, &YandexProvider::updatePositioningEnabled);
    connect(&m_locationSettingsWatcher, &QFileSystemWatcher::directoryChanged,
            this, &YandexProvider::updatePositioningEnabled);
    m_locationSettingsWatcher.addPath(LocationSettingsDir);
    m_locationSettingsWatcher.addPath(LocationSettingsFile);
    updatePositioningEnabled();

    new GeoclueAdaptor(this);
    new PositionAdaptor(this);

    qDebug() << "Mozilla Location Services geoclue plugin active";
    if (m_watchedServices.isEmpty()) {
        m_idleTimer.start(QuitIdleTime, this);
    }

    QDBusConnection connection = QDBusConnection::sessionBus();
    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(connection);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &YandexProvider::serviceUnregistered);

    if (m_positioningEnabled) {
        cellularNetworkRegistrationChanged();
    } else {
        qDebug() << "positioning is not currently enabled, idling";
    }
}

YandexProvider::~YandexProvider()
{
    if (staticProvider == this)
        staticProvider = 0;
}

/* TODO: coalesce lookups to avoid unnecessary repeated file I/O */
bool YandexProvider::searchForCellIdLocation(const MlsdbUniqueCellId &uniqueCellId, MlsdbCoords *coords)
{
    // try to find the mlsdb data file which should contain it.
    // the mlsdb data files are separated into "first digit of location code" directories/buckets.
    QChar firstDigitAreaCode = QString::number(uniqueCellId.locationCode()).at(0);
    QDirIterator it("/usr/share/geoclue-provider-mlsdb/", QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString fname(it.next());
        if (fname.endsWith(QStringLiteral("/%1/mlsdb.data").arg(firstDigitAreaCode), Qt::CaseInsensitive)) {
            // found an mlsdb.data file which might contain the cell data.  search it.
            QFile file(fname);
            file.open(QIODevice::ReadOnly);
            QDataStream in(&file);
            quint32 magic = 0, expectedMagic = (quint32)0xc710cdb;
            in >> magic;
            if (magic != 0xc710cdb) {
                qDebug() << "geoclue-mlsdb data file" << fname << "format unknown:" << magic << "expected:" << expectedMagic;
                continue; // ignore this file
            }
            qint32 version;
            in >> version;
            if (version != 3) {
                qDebug() << "geoclue-mlsdb data file" << fname << "version unknown:" << version;
                continue; // ignore this file
            }

            QMap<MlsdbUniqueCellId, MlsdbCoords> perLcCellIdToLocations;
            in >> perLcCellIdToLocations;
            if (perLcCellIdToLocations.isEmpty()) {
                qDebug() << "geoclue-mlsdb data file" << fname << "contained no cell locations!";
            } else {
                if (perLcCellIdToLocations.contains(uniqueCellId)) {
                    *coords = perLcCellIdToLocations.value(uniqueCellId);
                    qDebug() << "geoclue-mlsdb data file" << fname << "contains the location of composed cell id:" << uniqueCellId.toString() << "->" << coords->lat << "," << coords->lon;
                    return true; // found!
                } else {
                    qDebug() << "geoclue-mlsdb data file" << fname << "contains" << perLcCellIdToLocations.size() << "cell locations, but not for:" << uniqueCellId.toString();
                }
            }
        }
    }

    qDebug() << "no geoclue-mlsdb data files contain the location of composed cell id:" << uniqueCellId.toString();
    return false;
}

void YandexProvider::AddReference()
{
    if (!calledFromDBus())
        qFatal("AddReference must only be called from DBus");

    bool wasInactive = m_watchedServices.isEmpty();
    const QString service = message().service();
    m_watcher->addWatchedService(service);
    m_watchedServices[service].referenceCount += 1;
    if (wasInactive) {
        qDebug() << "new watched service, stopping idle timer.";
        m_idleTimer.stop();
    }

    startPositioningIfNeeded();
}

void YandexProvider::RemoveReference()
{
    if (!calledFromDBus())
        qFatal("RemoveReference must only be called from DBus");

    const QString service = message().service();

    if (m_watchedServices[service].referenceCount > 0)
        m_watchedServices[service].referenceCount -= 1;

    if (m_watchedServices[service].referenceCount == 0) {
        m_watcher->removeWatchedService(service);
        m_watchedServices.remove(service);
    }

    if (m_watchedServices.isEmpty()) {
        qDebug() << "no watched services, starting idle timer.";
        m_idleTimer.start(QuitIdleTime, this);
    }

    stopPositioningIfNeeded();
}

QString YandexProvider::GetProviderInfo(QString &description)
{
    description = tr("Mozilla Location Service Database cell-id position provider");
    return QLatin1String("Mlsdb");
}

int YandexProvider::GetStatus()
{
    return m_status;
}

void YandexProvider::SetOptions(const QVariantMap &options)
{
    if (!calledFromDBus())
        qFatal("SetOptions must only be called from DBus");

    const QString service = message().service();
    if (!m_watchedServices.contains(service)) {
        qWarning("Only active users can call SetOptions");
        return;
    }

    if (options.contains(QStringLiteral("UpdateInterval"))) {
        m_watchedServices[service].updateInterval =
            options.value(QStringLiteral("UpdateInterval")).toUInt();

        quint32 updateInterval = minimumRequestedUpdateInterval();
        m_recalculatePositionTimer.start(updateInterval, this);
    }
}

int YandexProvider::GetPosition(int &timestamp, double &latitude, double &longitude,
                                double &altitude, Accuracy &accuracy)
{
    if (m_currentLocation.timestamp() > 0) {
        qDebug() << "GetPosition:"
                                        << "timestamp:" << m_currentLocation.timestamp()
                                        << "latitude:" << m_currentLocation.latitude()
                                        << "longitude:" << m_currentLocation.longitude()
                                        << "accuracy:" << m_currentLocation.accuracy().horizontal();
    } else {
        qDebug() << "GetPosition: no valid current location known";
    }

    PositionFields positionFields = NoPositionFields;

    timestamp = m_currentLocation.timestamp() / 1000;
    if (!qIsNaN(m_currentLocation.latitude()))
        positionFields |= LatitudePresent;
    latitude = m_currentLocation.latitude();
    if (!qIsNaN(m_currentLocation.longitude()))
        positionFields |= LongitudePresent;
    longitude = m_currentLocation.longitude();
    if (!qIsNaN(m_currentLocation.altitude()))
        positionFields |= AltitudePresent;
    altitude = m_currentLocation.altitude();
    accuracy = m_currentLocation.accuracy();

    return positionFields;
}

void YandexProvider::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_idleTimer.timerId()) {
        m_idleTimer.stop();
        qDebug() << "have been idle for too long, quitting";
        qApp->quit();
    } else if (event->timerId() == m_fixLostTimer.timerId()) {
        m_fixLostTimer.stop();
        setStatus(StatusAcquiring);
    } else if (event->timerId() == m_recalculatePositionTimer.timerId()) {
        const qint64 currTimestamp = QDateTime::currentMSecsSinceEpoch();
        if (!m_positioningEnabled) {
            qDebug() << "positioning is disabled, preventing MLS calculation";
        } else if (m_currentLocation.timestamp() == 0
                || ((currTimestamp - m_currentLocation.timestamp()) > ReuseInterval)
                || m_signalUpdateCell || m_signalUpdateWlan) {
            qDebug() << "calculating new position information";
            m_signalUpdateCell = false;
            m_signalUpdateWlan = false;
            calculatePositionAndEmitLocation();
        } else {
            qDebug() << "re-using old position information";
            setLocation(m_currentLocation);
        }
    } else {
        QObject::timerEvent(event);
    }
}

void YandexProvider::calculatePositionAndEmitLocation()
{
    const QList<CellPositioningData> cellIds = seenCellIds();
    if (m_onlinePositioningEnabled) {
        if (!m_mlsdbOnlineLocator) {
            m_mlsdbOnlineLocator = new YandexOnlineLocator(this);
            m_mlsdbOnlineLocator->setWlanDataAllowed(m_wlanDataAllowed);
            connect(m_mlsdbOnlineLocator, &YandexOnlineLocator::wlanChanged,
                    this, &YandexProvider::onlineWlanChanged);
            connect(m_mlsdbOnlineLocator, &YandexOnlineLocator::locationFound,
                    this, &YandexProvider::onlineLocationFound);
            connect(m_mlsdbOnlineLocator, &YandexOnlineLocator::error,
                    this, &YandexProvider::onlineLocationError);
        }
        const QPair<QDateTime, QVariantMap> query = m_mlsdbOnlineLocator->buildLocationQuery(
                cellIds, m_previousQuery);
        if (m_mlsdbOnlineLocator->findLocation(query)) {
            m_previousQuery = query;
            return;
        }
    }

    // fall back to using offline position
    updateLocationFromCells(cellIds);
}

void YandexProvider::onlineWlanChanged()
{
    m_signalUpdateWlan = true;
}

void YandexProvider::onlineLocationFound(double latitude, double longitude, double accuracy)
{
    qDebug() << "Location from MLS online:" << latitude << longitude << accuracy;

    Location deviceLocation;
    deviceLocation.setTimestamp(QDateTime::currentMSecsSinceEpoch());
    deviceLocation.setLatitude(latitude);
    deviceLocation.setLongitude(longitude);

    Accuracy positionAccuracy;
    positionAccuracy.setHorizontal(accuracy);
    deviceLocation.setAccuracy(positionAccuracy);

    setLocation(deviceLocation);
}

void YandexProvider::onlineLocationError(const QString &errorString)
{
    qDebug() << "Cannot fetch position from online source:" << errorString
                                    << ", falling back to offline source";

    // fall back to using offline position
    updateLocationFromCells(seenCellIds());
}

QList<YandexProvider::CellPositioningData> YandexProvider::seenCellIds() const
{
    QList<CellPositioningData> cells;
    if (!m_cellDataAllowed) {
        return cells;
    }

    qDebug() << "have" << m_cellWatcher->cells().size() << "neighbouring cells";
    quint32 maxNeighborSignalStrength = 1;
    QSet<MlsdbUniqueCellId> seenCellIds;
    Q_FOREACH (const QSharedPointer<QOfonoExtCell> &c, m_cellWatcher->cells()) {
        CellPositioningData cell;
        quint32 locationCode = 0;
        quint32 cellId = 0;
        quint16 mcc = c->mcc();
        quint16 mnc = c->mnc();
        MlsdbCellType cellType = c->type() == QOfonoExtCell::LTE
                               ? MLSDB_CELL_TYPE_LTE
                               : c->type() == QOfonoExtCell::GSM
                               ? MLSDB_CELL_TYPE_GSM
                               : c->type() == QOfonoExtCell::WCDMA
                               ? MLSDB_CELL_TYPE_UMTS
                               : MLSDB_CELL_TYPE_UMTS;
        if (c->cid() != QOfonoExtCell::InvalidValue && c->cid() != 0 && mcc != 0) {
            locationCode = static_cast<quint32>(c->lac());
            cellId = static_cast<quint32>(c->cid());
        } else if (c->ci() != QOfonoExtCell::InvalidValue && c->ci() != 0 && mcc != 0) {
            locationCode = static_cast<quint32>(c->tac());
            cellId = static_cast<quint32>(c->ci());
        } else {
            qDebug() << "ignoring neighbour cell with no cell id with type:" << c->type()
                                            << " mcc:" << c->mcc() << " mnc:" << c->mnc() << " lac:" << c->lac()
                                            << " tac:" << c->tac() << " pci:" << c->pci() << " psc:" << c->psc();
            continue;
        }
        cell.uniqueCellId = MlsdbUniqueCellId(cellType, cellId, locationCode, mcc, mnc);
        if (!seenCellIds.contains(cell.uniqueCellId)) {
            qDebug() << "have neighbour cell:" << cell.uniqueCellId.toString()
                                            << "with strength:" << c->signalStrength();
            cell.signalStrength = c->signalStrength();
            if (cell.signalStrength > maxNeighborSignalStrength) {
                // used for the cells we're connected to.
                // if no signal strength data is available from ofono,
                // we assume they're at least as strong signals as the
                // strongest of our neighbor cells.
                maxNeighborSignalStrength = cell.signalStrength;
            }
            cells.append(cell);
            seenCellIds.insert(cell.uniqueCellId);
        }
    }
    return cells;
}

void YandexProvider::updateLocationFromCells(const QList<CellPositioningData> &cells)
{
    // determine which cells we have an accurate location for, from MLSDB data.
    double totalSignalStrength = 0.0;
    QMap<MlsdbUniqueCellId, MlsdbCoords> cellLocations;
    Q_FOREACH (const CellPositioningData &cell, cells) {
        MlsdbCoords cellCoords;
        if (!m_uniqueCellIdToLocation.contains(cell.uniqueCellId)) {
            if (m_knownCellIdsWithUnknownLocations.contains(cell.uniqueCellId)) {
                // we know that we don't know the location of this cellId.  Skip it.
                continue;
            } else {
                // this is a new cell Id that we haven't encountered yet.  Probe it.
                if (!searchForCellIdLocation(cell.uniqueCellId, &cellCoords)) {
                    // we now know that we don't know the location of this cellId.
                    m_knownCellIdsWithUnknownLocations.insert(cell.uniqueCellId);
                    continue;
                }
                // cache the location of the cell id for future reference.
                m_uniqueCellIdToLocation.insert(cell.uniqueCellId, cellCoords);
            }
        } else {
            cellCoords = m_uniqueCellIdToLocation.value(cell.uniqueCellId);
        }
        // we have a known location for this cell.  Update our locations list.
        cellLocations.insert(cell.uniqueCellId, cellCoords);
        totalSignalStrength += (1.0 * cell.signalStrength);
    }

    if (cellLocations.size() == 0) {
        qDebug() << "no cell id data to calculate position from";
        return;
    } else if (cellLocations.size() == 1) {
        qDebug() << "only one cell id datum to calculate position from, position will be extremely inaccurate";
    } else if (cellLocations.size() == 2) {
        qDebug() << "only two cell id data to calculate position from, position will be highly inaccurate";
    } else {
        qDebug() << "calculating position from" << cellLocations.size() << "cell id data";
    }

    // now use the current cell and neighboringcell information to triangulate our position.
    double deviceLatitude = 0.0;
    double deviceLongitude = 0.0;
    Q_FOREACH (const CellPositioningData &cell, cells) {
        if (cellLocations.contains(cell.uniqueCellId)) {
            const MlsdbCoords &cellCoords(cellLocations.value(cell.uniqueCellId));
            double weight = (((double)cell.signalStrength) / totalSignalStrength);
            deviceLatitude += (weight * cellCoords.lat);
            deviceLongitude += (weight * cellCoords.lon);
            qDebug() << "have cell:" << cell.uniqueCellId.toString()
                                            << "with position:" << cellCoords.lat << "," << cellCoords.lon
                                            << "with strength:" << ((double)cell.signalStrength / totalSignalStrength);
        } else {
            qDebug() << "do not know position of cell with id:" << cell.uniqueCellId.toString();
        }
    }

    Location deviceLocation;
    if (cellLocations.size()) {
        // estimate accuracy based on how many cells we have.
        Accuracy positionAccuracy;
        positionAccuracy.setHorizontal(qMax(MinimumCalculatedAccuracy,
                                            10000 - (1000 * cellLocations.size())));
        deviceLocation.setTimestamp(QDateTime::currentMSecsSinceEpoch());
        deviceLocation.setLatitude(deviceLatitude);
        deviceLocation.setLongitude(deviceLongitude);
        deviceLocation.setAccuracy(positionAccuracy);
    }

    // and set this as our location if it is at least as accurate as our previous data,
    // or if the previous data is more than two minutes old.
    if (m_currentLocation.timestamp() != 0
            && (QDateTime::currentMSecsSinceEpoch() - m_currentLocation.timestamp()) < FallbackInterval
            && m_currentLocation.accuracy().horizontal() < deviceLocation.accuracy().horizontal()) {
        qDebug() << "re-using old position information due to better accuracy";
        qDebug() << "preferring:" << m_currentLocation.latitude() << ","
                                                 << m_currentLocation.longitude() << ","
                                                 << m_currentLocation.accuracy().horizontal()
                                << "over:" << deviceLocation.latitude() << ","
                                           << deviceLocation.longitude() << ","
                                           << deviceLocation.accuracy().horizontal();
        setLocation(m_currentLocation);
    } else {
        setLocation(deviceLocation);
    }
}

void YandexProvider::setLocation(const Location &location)
{
    qDebug() << "setting current location to:"
                                    << "ts:" << location.timestamp() << ","
                                    << "lat:" << location.latitude() << "," << "lon:" << location.longitude() << ","
                                    << "accuracy:" << location.accuracy().horizontal();

    if (location.timestamp() != 0) {
        setStatus(StatusAvailable);
        m_fixLostTimer.start(FixTimeout, this);
        m_lastLocation = m_currentLocation;
    } else {
        qDebug() << "location invalid, lost positioning fix";
        m_lastLocation = Location(); // lost fix, reset last location also.
    }

    m_currentLocation = location;
    emitLocationChanged();
}

void YandexProvider::serviceUnregistered(const QString &service)
{
    m_watchedServices.remove(service);
    m_watcher->removeWatchedService(service);
    if (m_watchedServices.isEmpty()) {
        qDebug() << "no watched services, starting idle timer.";
        m_idleTimer.start(QuitIdleTime, this);
    }

    stopPositioningIfNeeded();
}

void YandexProvider::updatePositioningEnabled()
{
    bool positioningEnabled = false;
    bool cellPositioningEnabled = false;

    bool onlineDataAllowed = false;
    bool cellDataAllowed = false;
    bool wlanDataAllowed = false;

    getEnabled(&positioningEnabled,
               &cellPositioningEnabled,
               &m_onlinePositioningEnabled,
               &onlineDataAllowed,
               &cellDataAllowed,
               &wlanDataAllowed);

    qDebug() << "positioning is" << (positioningEnabled ? "enabled" : "disabled");
    qDebug() << "device-local cell triangulation positioning is" << (cellPositioningEnabled ? "enabled" : "disabled");
    qDebug() << "mls online service positioning is" << (m_onlinePositioningEnabled ? "enabled" : "disabled");

    qDebug() << "now checking MDM data source restrictions...";

    if (m_onlineDataAllowed != onlineDataAllowed) {
        m_onlineDataAllowed = onlineDataAllowed;
    }
    if (m_onlineDataAllowed) {
        qDebug() << "allowed to use online data to determine position";
    } else {
        qDebug() << "not allowed to use online data to determine position";
    }

    if (m_cellDataAllowed != cellDataAllowed) {
        m_cellDataAllowed = cellDataAllowed;
        if (!m_cellWatcher && m_cellDataAllowed) {
            qDebug() << "listening for cell data changes";
            m_cellWatcher = new QOfonoExtCellWatcher(this);
            connect(m_cellWatcher, &QOfonoExtCellWatcher::cellsChanged,
                    this, &YandexProvider::cellularNetworkRegistrationChanged);
        } else if (m_cellWatcher && !m_cellDataAllowed) {
            qDebug() << "no longer listening for cell data changes";
            m_cellWatcher->deleteLater();
            m_cellWatcher = Q_NULLPTR;
        }
    }
    if (m_cellDataAllowed) {
        qDebug() << "allowed to use adjacent cell id data to determine position";
    } else {
        qDebug() << "not allowed to use adjacent cell id data to determine position";
    }

    if (m_wlanDataAllowed != wlanDataAllowed) {
        m_wlanDataAllowed = wlanDataAllowed;
    }
    if (m_wlanDataAllowed) {
        qDebug() << "allowed to use wlan data to determine position";
    } else {
        qDebug() << "not allowed to use wlan data to determine position";
    }

    if (m_mlsdbOnlineLocator) {
        m_mlsdbOnlineLocator->setWlanDataAllowed(m_wlanDataAllowed);
    }

    bool previous = m_positioningEnabled;
    bool enabled = positioningEnabled && cellPositioningEnabled;
    if (previous == enabled) {
        return;
    }

    if (enabled) {
        qDebug() << "positioning has been enabled";
        m_positioningEnabled = true;
        startPositioningIfNeeded();
    } else {
        qDebug() << "positioning has been disabled";
        m_positioningEnabled = false;
        setLocation(Location());
        stopPositioningIfNeeded();
    }
}

void YandexProvider::cellularNetworkRegistrationChanged()
{
    m_signalUpdateCell = true;
}

void YandexProvider::emitLocationChanged()
{
    PositionFields positionFields = NoPositionFields;

    if (!qIsNaN(m_currentLocation.latitude()))
        positionFields |= LatitudePresent;
    if (!qIsNaN(m_currentLocation.longitude()))
        positionFields |= LongitudePresent;
    if (!qIsNaN(m_currentLocation.altitude()))
        positionFields |= AltitudePresent;

    emit PositionChanged(positionFields, m_currentLocation.timestamp() / 1000,
                         m_currentLocation.latitude(), m_currentLocation.longitude(),
                         m_currentLocation.altitude(), m_currentLocation.accuracy());
}

void YandexProvider::startPositioningIfNeeded()
{
    // Positioning is already started.
    if (m_positioningStarted)
        return;

    // Positioning is unused.
    if (m_watchedServices.isEmpty())
        return;

    // Positioning disabled externally
    if (!m_positioningEnabled)
        return;

    m_idleTimer.stop();

    qDebug() << "Starting positioning";
    m_positioningStarted = true;
    calculatePositionAndEmitLocation();
    quint32 updateInterval = minimumRequestedUpdateInterval();
    m_recalculatePositionTimer.start(updateInterval, this);
}

void YandexProvider::stopPositioningIfNeeded()
{
    // Positioning is already stopped.
    if (!m_positioningStarted)
        return;

    // Positioning enabled externally and positioning is still being used.
    if (m_positioningEnabled && !m_watchedServices.isEmpty())
        return;

    qDebug() << "Stopping positioning";
    m_positioningStarted = false;
    setStatus(StatusUnavailable);
    m_fixLostTimer.stop();
    m_recalculatePositionTimer.stop();
}

void YandexProvider::setStatus(YandexProvider::Status status)
{
    if (m_status == status)
        return;

    m_status = status;
    emit StatusChanged(m_status);
}

/*
    Checks the state of the Location enabled setting,
    the MLS enabled setting, and the MLS online_enabled setting.
*/
void YandexProvider::getEnabled(bool *positioningEnabled, bool *cellPositioningEnabled,
                               bool *onlinePositioningEnabled, bool *onlineDataAllowed,
                               bool *cellDataAllowed, bool *wlanDataAllowed)
{
    QSettings settings(LocationSettingsFile, QSettings::IniFormat);

    *positioningEnabled = settings.value(LocationSettingsEnabledKey, false).toBool();

    *cellPositioningEnabled = *positioningEnabled
                            && (settings.value(LocationSettingsMlsEnabledKey, false).toBool()
                             || settings.value(LocationSettingsOldMlsEnabledKey, false).toBool());

    *onlinePositioningEnabled = *cellPositioningEnabled
                            && settings.value(LocationSettingsMlsOnlineEnabledKey, false).toBool();

    *onlineDataAllowed = settings.value(LocationSettingsDataSourceOnlineAllowedKey, true).toBool();
    *cellDataAllowed = settings.value(LocationSettingsDataSourceCellDataAllowedKey, true).toBool();
    *wlanDataAllowed = settings.value(LocationSettingsDataSourceWlanDataAllowedKey, true).toBool();
}

quint32 YandexProvider::minimumRequestedUpdateInterval() const
{
    quint32 updateInterval = UINT_MAX;

    foreach (const ServiceData &data, m_watchedServices) {
        // Old data, service not currently using positioning.
        if (data.referenceCount <= 0) {
            qWarning("Service data was not removed!");
            continue;
        }

        // Service hasn't requested a specific update interval.
        if (data.updateInterval == 0)
            continue;

        updateInterval = qMin(updateInterval, data.updateInterval);
    }

    if (updateInterval == UINT_MAX)
        return MinimumInterval;

    return qMax(updateInterval, MinimumInterval);
}
