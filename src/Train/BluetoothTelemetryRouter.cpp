#include "BluetoothTelemetryRouter.h"

#include <algorithm>
#include <cmath>
#include <limits>

BluetoothTelemetryRouter::BluetoothTelemetryRouter(qint64 staleAfterMs) :
    staleAfterMs(std::max<qint64>(0, staleAfterMs))
{
    owners.fill(0);
}

bool BluetoothTelemetryRouter::publish(
        quintptr source, BluetoothTelemetryMetric metric, double value,
        BluetoothTelemetryPriority priority, qint64 nowMs)
{
    const int index = metricIndex(metric);
    if (source == 0 || index < 0 || !std::isfinite(value) || nowMs < 0 ||
        !validPriority(priority)) {
        return false;
    }

    Sample &sample = sources[source].samples[static_cast<size_t>(index)];
    sample.available = true;
    sample.value = value;
    sample.priority = priority;
    sample.updatedAtMs = nowMs;
    return true;
}

void BluetoothTelemetryRouter::removeSource(quintptr source)
{
    if (source == 0) return;
    sources.remove(source);
    for (quintptr &owner : owners) {
        if (owner == source) owner = 0;
    }
}

BluetoothTelemetryValue BluetoothTelemetryRouter::resolve(
        BluetoothTelemetryMetric metric, qint64 nowMs)
{
    const int index = metricIndex(metric);
    if (index < 0 || nowMs < 0) return BluetoothTelemetryValue();

    const size_t sampleIndex = static_cast<size_t>(index);
    int highestPriority = std::numeric_limits<int>::min();
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        const Sample &sample = it.value().samples[sampleIndex];
        if (!fresh(sample, nowMs)) continue;
        highestPriority = std::max(
                highestPriority, static_cast<int>(sample.priority));
    }

    if (highestPriority == std::numeric_limits<int>::min()) {
        owners[sampleIndex] = 0;
        return BluetoothTelemetryValue();
    }

    const quintptr currentOwner = owners[sampleIndex];
    const auto current = sources.constFind(currentOwner);
    if (current != sources.cend()) {
        const Sample &sample = current.value().samples[sampleIndex];
        if (fresh(sample, nowMs) &&
            static_cast<int>(sample.priority) == highestPriority) {
            return valueFor(currentOwner, sample);
        }
    }

    quintptr selectedSource = 0;
    const Sample *selectedSample = nullptr;
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        const Sample &sample = it.value().samples[sampleIndex];
        if (!fresh(sample, nowMs) ||
            static_cast<int>(sample.priority) != highestPriority) {
            continue;
        }

        if (!selectedSample || sample.updatedAtMs > selectedSample->updatedAtMs ||
            (sample.updatedAtMs == selectedSample->updatedAtMs &&
             it.key() < selectedSource)) {
            selectedSource = it.key();
            selectedSample = &sample;
        }
    }

    if (!selectedSample) {
        owners[sampleIndex] = 0;
        return BluetoothTelemetryValue();
    }

    owners[sampleIndex] = selectedSource;
    return valueFor(selectedSource, *selectedSample);
}

void BluetoothTelemetryRouter::clear()
{
    sources.clear();
    owners.fill(0);
}

int BluetoothTelemetryRouter::metricIndex(BluetoothTelemetryMetric metric)
{
    const int index = static_cast<int>(metric);
    return index >= 0 && index < MetricCount ? index : -1;
}

bool BluetoothTelemetryRouter::validPriority(
        BluetoothTelemetryPriority priority)
{
    return priority == BluetoothTelemetryPriority::Trainer ||
           priority == BluetoothTelemetryPriority::DedicatedSensor;
}

bool BluetoothTelemetryRouter::fresh(const Sample &sample, qint64 nowMs) const
{
    if (!sample.available) return false;
    if (nowMs < sample.updatedAtMs) return true;
    return nowMs - sample.updatedAtMs <= staleAfterMs;
}

BluetoothTelemetryValue BluetoothTelemetryRouter::valueFor(
        quintptr source, const Sample &sample) const
{
    return BluetoothTelemetryValue{
        true, sample.value, source, sample.priority
    };
}
