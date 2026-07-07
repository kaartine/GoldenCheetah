/*
 * Copyright (c) 2011 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2016 Arto Jantunen (viiru@iki.fi)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "GoldenCheetah.h"

#include "RealtimeController.h"
#include "DeviceConfiguration.h"
#include "ConfigDialog.h"
#include <QBluetoothLocalDevice>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>

#include <array>

#include "BluetoothTelemetryRouter.h"
#include "BT40Device.h"

#ifndef _GC_BT40Controller_h
#define _GC_BT40Controller_h 1

class DeviceInfo;

class BT40Controller : public RealtimeController
{
    Q_OBJECT

public:
    BT40Controller (TrainSidebar *parent =0, DeviceConfiguration *dc =0);
    ~BT40Controller();

    int start();
    int restart();                              // restart after paused
    int pause();                                // pauses data collection, inbound telemetry is discarded
    int stop();                                 // stops data collection thread

    bool find();
    bool discover(QString name);
    void setDevice(QString);
    QList<QBluetoothDeviceInfo> getDeviceInfo();

    void setLoad(double);
    void setGradient(double);
    void setMode(int);
    void setWindSpeed(double);
    void setWeight(double);
    void setRollingResistance(double);
    void setWindResistance(double);
    void setWheelCircumference(double);

    // telemetry push pull
    bool doesPush(), doesPull(), doesLoad();
    void getRealtimeData(RealtimeData &rtData);
    void pushRealtimeData(RealtimeData &rtData);
    void setBPM(BT40Device *source, float value,
                BluetoothTelemetryPriority priority);
    void setWatts(BT40Device *source, double value,
                  BluetoothTelemetryPriority priority);
    void setWheelRpm(BT40Device *source, double value,
                     BluetoothTelemetryPriority priority);
    void setSpeed(BT40Device *source, double value,
                  BluetoothTelemetryPriority priority);
    void setCadence(BT40Device *source, double value,
                    BluetoothTelemetryPriority priority);
    void setRespiratoryFrequency(BT40Device *source, double value,
                                 BluetoothTelemetryPriority priority);
    void setRespiratoryMinuteVolume(BT40Device *source, double value,
                                    BluetoothTelemetryPriority priority);
    void setVO2_VCO2(BT40Device *source, double vo2, double vco2,
                     BluetoothTelemetryPriority priority);
    void setTv(BT40Device *source, double value,
               BluetoothTelemetryPriority priority);
    void setFeO2(BT40Device *source, double value,
                 BluetoothTelemetryPriority priority);
    void removeTelemetrySource(BT40Device *source);
    void emitVO2Data() {
        emit vo2Data(telemetry.getRf(), telemetry.getRMV(), telemetry.getVO2(), telemetry.getVCO2(), telemetry.getTv(), telemetry.getFeO2());
    }

    // Calibration overrides.
    uint8_t  getCalibrationType();
    uint8_t  getCalibrationState();
    double   getCalibrationTargetSpeed();
    uint16_t getCalibrationSpindownTime();
    uint16_t getCalibrationZeroOffset();
    uint16_t getCalibrationSlope();

signals:
    void vo2Data(double rf, double rmv, double vo2, double vco2, double tv, double feo2);
    void scanFinished(bool foundAnyDevices);

private slots:
    void addDevice(const QBluetoothDeviceInfo&);
    void startScan();
    void scanFinished();
    void deviceScanError(QBluetoothDeviceDiscoveryAgent::Error);
    void rescanDevice();
    void deviceConnectionRestored();

private:
    bool deviceAllowed(const QBluetoothDeviceInfo& info);
    bool isHeartRateOnly() const;
    bool allConfiguredDevicesFound() const;
    void resetScanRetryState();
    void scheduleScanRetry(const QString &firstNotice);
    qint64 telemetryNowMs() const;
    void publishTelemetry(BT40Device *source, BluetoothTelemetryMetric metric,
                          double value, BluetoothTelemetryPriority priority);
    void refreshTelemetry();
    void refreshTelemetryMetric(BluetoothTelemetryMetric metric, qint64 nowMs,
                                quintptr forceSource = 0);

private:
    QBluetoothDeviceDiscoveryAgent *discoveryAgent;
    QBluetoothLocalDevice* localDevice;
    RealtimeData telemetry;
    BluetoothTelemetryRouter telemetryRouter;
    QElapsedTimer telemetryClock;
    std::array<quintptr, BluetoothTelemetryRouter::MetricCount>
            appliedTelemetrySources{};
    std::array<bool, BluetoothTelemetryRouter::MetricCount>
            appliedTelemetryAvailable{};
    QList<BT40Device*> devices;
    DeviceConfiguration* localDc;
    QList<DeviceInfo> allowedDevices;
    QTimer *scanRetryTimer;
    int scanRetryDelayMs;
    bool missingDeviceNoticeShown;
    bool running;
    QSet<BT40Device*> devicesAwaitingRediscovery;

    double load;
    double gradient;
    int mode;
    double windSpeed;
    double weight;
    double rollingResistance;
    double windResistance;
    double wheelSize;
};

class DeviceInfo
{
public:
    DeviceInfo(QString data);
    DeviceInfo(QString name, QString address, QString uuid);
    QString getName() const;
    QString getAddress() const;
    QString getUuid() const;
    bool isValid() const;

private:
    QString name;
    QString address;
    QString uuid;
};
#endif // _GC_BT40Controller_h
