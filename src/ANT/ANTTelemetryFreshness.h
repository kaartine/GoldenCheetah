#ifndef GC_ANTTELEMETRYFRESHNESS_H
#define GC_ANTTELEMETRYFRESHNESS_H

#include <QList>
#include <QtGlobal>

#include <array>
#include <cstddef>

enum class ANTTelemetryMetric {
    HeartRate,
    Power,
    AltPower,
    WheelRpm,
    Speed,
    Cadence,
    MuscleOxygen,
    Temperature,
    CoreTemperature,
    LeftRightBalance,
    TorqueEffectiveness,
    PedalSmoothness,
    RightPowerPhase,
    LeftPowerPhase,
    PedalPosition,
    Torque,
    TrainerStatus,
    Count
};

enum class ANTTelemetryPriority {
    Secondary = 100,
    Primary = 200
};

class ANTTelemetryFreshness
{
public:
    static constexpr qint64 FastTimeoutMs = 5000;
    static constexpr qint64 SlowTimeoutMs = 30000;
    static constexpr int MetricCount =
            static_cast<int>(ANTTelemetryMetric::Count);

    bool publish(int source, ANTTelemetryMetric metric,
                 ANTTelemetryPriority priority, qint64 nowMs)
    {
        const int index = metricIndex(metric);
        if (source < -1 || index < 0 || nowMs < 0 ||
            !validPriority(priority)) {
            return false;
        }

        State &state = states[static_cast<std::size_t>(index)];
        if (fresh(state, metric, nowMs) &&
            static_cast<int>(state.priority) >
                    static_cast<int>(priority)) {
            return false;
        }

        state.available = true;
        state.source = source;
        state.priority = priority;
        state.updatedAtMs = nowMs;
        return true;
    }

    QList<ANTTelemetryMetric> expire(qint64 nowMs)
    {
        QList<ANTTelemetryMetric> expired;
        if (nowMs < 0) return expired;

        for (int index = 0; index < MetricCount; ++index) {
            State &state = states[static_cast<std::size_t>(index)];
            const auto metric = static_cast<ANTTelemetryMetric>(index);
            if (state.available && !fresh(state, metric, nowMs)) {
                state.available = false;
                expired.append(metric);
            }
        }
        return expired;
    }

    QList<ANTTelemetryMetric> removeSource(int source)
    {
        QList<ANTTelemetryMetric> removed;
        if (source < -1) return removed;

        for (int index = 0; index < MetricCount; ++index) {
            State &state = states[static_cast<std::size_t>(index)];
            if (state.available && state.source == source) {
                state.available = false;
                removed.append(static_cast<ANTTelemetryMetric>(index));
            }
        }
        return removed;
    }

private:
    struct State {
        bool available = false;
        int source = -1;
        ANTTelemetryPriority priority = ANTTelemetryPriority::Secondary;
        qint64 updatedAtMs = 0;
    };

    static int metricIndex(ANTTelemetryMetric metric)
    {
        const int index = static_cast<int>(metric);
        return index >= 0 && index < MetricCount ? index : -1;
    }

    static bool validPriority(ANTTelemetryPriority priority)
    {
        return priority == ANTTelemetryPriority::Secondary ||
               priority == ANTTelemetryPriority::Primary;
    }

    static qint64 timeoutMs(ANTTelemetryMetric metric)
    {
        return metric == ANTTelemetryMetric::Temperature ||
               metric == ANTTelemetryMetric::CoreTemperature
                ? SlowTimeoutMs : FastTimeoutMs;
    }

    static bool fresh(const State &state, ANTTelemetryMetric metric,
                      qint64 nowMs)
    {
        if (!state.available) return false;
        if (nowMs < state.updatedAtMs) return true;
        return nowMs - state.updatedAtMs <= timeoutMs(metric);
    }

    std::array<State, MetricCount> states{};
};

#endif // GC_ANTTELEMETRYFRESHNESS_H
