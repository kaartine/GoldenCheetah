#ifndef BLUETOOTHTELEMETRYROUTER_H
#define BLUETOOTHTELEMETRYROUTER_H

#include <QHash>
#include <QtGlobal>

#include <array>

enum class BluetoothTelemetryMetric {
    HeartRate,
    Power,
    WheelRpm,
    Speed,
    Cadence,
    RespiratoryFrequency,
    RespiratoryMinuteVolume,
    Vo2,
    Vco2,
    TidalVolume,
    FeO2,
    Count
};

enum class BluetoothTelemetryPriority {
    Trainer = 100,
    DedicatedSensor = 200
};

struct BluetoothTelemetryValue {
    bool available = false;
    double value = 0.0;
    quintptr source = 0;
    BluetoothTelemetryPriority priority = BluetoothTelemetryPriority::Trainer;
};

class BluetoothTelemetryRouter
{
public:
    static constexpr int MetricCount =
            static_cast<int>(BluetoothTelemetryMetric::Count);

    explicit BluetoothTelemetryRouter(qint64 staleAfterMs = 5000);

    bool publish(quintptr source, BluetoothTelemetryMetric metric, double value,
                 BluetoothTelemetryPriority priority, qint64 nowMs);
    void removeSource(quintptr source);
    BluetoothTelemetryValue resolve(BluetoothTelemetryMetric metric,
                                    qint64 nowMs);
    void clear();

private:
    struct Sample {
        bool available = false;
        double value = 0.0;
        BluetoothTelemetryPriority priority =
                BluetoothTelemetryPriority::Trainer;
        qint64 updatedAtMs = 0;
    };

    struct SourceState {
        std::array<Sample, MetricCount> samples;
    };

    static int metricIndex(BluetoothTelemetryMetric metric);
    static bool validPriority(BluetoothTelemetryPriority priority);
    bool fresh(const Sample &sample, qint64 nowMs) const;
    BluetoothTelemetryValue valueFor(quintptr source, const Sample &sample) const;

    QHash<quintptr, SourceState> sources;
    std::array<quintptr, MetricCount> owners{};
    qint64 staleAfterMs;
};

#endif // BLUETOOTHTELEMETRYROUTER_H
