/*
 * Copyright (c) 2026 Jukka Kaartinen
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "StravaActivityDescription.h"

#include <QChar>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

constexpr int SparklineBins = 32;

bool positive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

QString durationText(double seconds)
{
    const qint64 rounded = qMax<qint64>(0, qRound64(seconds));
    const qint64 hours = rounded / 3600;
    const qint64 minutes = (rounded % 3600) / 60;
    const qint64 secs = rounded % 60;

    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString zoneDurationText(double seconds)
{
    const qint64 roundedSeconds = qMax<qint64>(0, qRound64(seconds));
    if (roundedSeconds < 60)
        return QStringLiteral("%1s").arg(roundedSeconds);

    const qint64 roundedMinutes = qRound64(seconds / 60.0);
    if (roundedMinutes < 60)
        return QStringLiteral("%1m").arg(roundedMinutes);
    return QStringLiteral("%1h%2m")
        .arg(roundedMinutes / 60)
        .arg(roundedMinutes % 60, 2, 10, QLatin1Char('0'));
}

double resolved(double supplied, double calculated)
{
    return positive(supplied) ? supplied : calculated;
}

struct CalculatedMetrics {
    double duration = 0.0;
    double work = 0.0;
    double averagePower = 0.0;
    double maximumPower = 0.0;
    double normalizedPower = 0.0;
    double intensityFactor = 0.0;
    double bikeStress = 0.0;
    double averageHeartRate = 0.0;
    double maximumHeartRate = 0.0;
    double averageCadence = 0.0;
};

CalculatedMetrics calculate(
    const StravaActivityDescription::Input &input)
{
    CalculatedMetrics result;
    const double recordingInterval = positive(input.recordingInterval)
        ? input.recordingInterval : 1.0;

    if (!input.samples.isEmpty()) {
        result.duration = input.samples.constLast().seconds
            - input.samples.constFirst().seconds + recordingInterval;
    }

    double powerTotal = 0.0;
    int powerCount = 0;
    double heartRateTotal = 0.0;
    int heartRateCount = 0;
    double cadenceTotal = 0.0;
    int cadenceCount = 0;

    for (const auto &sample : input.samples) {
        if (input.hasPower && std::isfinite(sample.power)
            && sample.power >= 0.0) {
            powerTotal += sample.power;
            result.maximumPower = qMax(result.maximumPower, sample.power);
            result.work += sample.power * recordingInterval / 1000.0;
            ++powerCount;
        }
        if (input.hasHeartRate && positive(sample.heartRate)) {
            heartRateTotal += sample.heartRate;
            result.maximumHeartRate = qMax(
                result.maximumHeartRate, sample.heartRate);
            ++heartRateCount;
        }
        if (input.hasCadence && positive(sample.cadence)) {
            cadenceTotal += sample.cadence;
            ++cadenceCount;
        }
    }

    if (powerCount > 0) result.averagePower = powerTotal / powerCount;
    if (heartRateCount > 0)
        result.averageHeartRate = heartRateTotal / heartRateCount;
    if (cadenceCount > 0) result.averageCadence = cadenceTotal / cadenceCount;

    const int rollingWindow = qFloor(30.0 / recordingInterval);
    if (input.hasPower && rollingWindow > 1) {
        QVector<double> rolling(rollingWindow, 0.0);
        int rollingIndex = 0;
        double rollingSum = 0.0;
        double fourthPowerTotal = 0.0;
        int count = 0;
        for (const auto &sample : input.samples) {
            const double power = std::isfinite(sample.power)
                ? qMax(0.0, sample.power) : 0.0;
            rollingSum += power - rolling.at(rollingIndex);
            rolling[rollingIndex] = power;
            fourthPowerTotal += std::pow(
                rollingSum / rollingWindow, 4.0);
            rollingIndex = (rollingIndex + 1) % rollingWindow;
            ++count;
        }
        if (count > 0) {
            result.normalizedPower = std::pow(
                fourthPowerTotal / count, 0.25);
            if (positive(input.ftp)) {
                result.intensityFactor =
                    result.normalizedPower / input.ftp;
                result.bikeStress = result.normalizedPower
                    * count * recordingInterval
                    * result.intensityFactor
                    / (input.ftp * 3600.0) * 100.0;
            }
        }
    }
    return result;
}

QString powerSparkline(
    const StravaActivityDescription::Input &input)
{
    if (!input.hasPower || input.samples.isEmpty()) return {};

    const int binCount = qMin(SparklineBins, input.samples.size());
    QVector<double> totals(binCount, 0.0);
    QVector<int> counts(binCount, 0);
    for (int index = 0; index < input.samples.size(); ++index) {
        const int bin = qMin(
            binCount - 1,
            static_cast<int>((static_cast<qint64>(index) * binCount)
                             / input.samples.size()));
        const double power = input.samples.at(index).power;
        if (std::isfinite(power) && power >= 0.0) {
            totals[bin] += power;
            ++counts[bin];
        }
    }

    QVector<double> averages(binCount, 0.0);
    double maximum = 0.0;
    for (int index = 0; index < binCount; ++index) {
        if (counts.at(index) > 0)
            averages[index] = totals.at(index) / counts.at(index);
        maximum = qMax(maximum, averages.at(index));
    }
    if (!positive(maximum)) return {};

    static const ushort Blocks[] = {
        0x2581, 0x2582, 0x2583, 0x2584,
        0x2585, 0x2586, 0x2587, 0x2588
    };
    QString result;
    result.reserve(binCount);
    for (double average : averages) {
        const int level = qBound(
            0, qRound((average / maximum) * 7.0), 7);
        result.append(QChar(Blocks[level]));
    }
    return result;
}

QString powerZoneLine(
    const StravaActivityDescription::Input &input)
{
    if (!input.hasPower || input.powerZones.isEmpty()) return {};

    QVector<double> seconds(input.powerZones.size(), 0.0);
    const double recordingInterval = positive(input.recordingInterval)
        ? input.recordingInterval : 1.0;
    for (const auto &sample : input.samples) {
        if (!std::isfinite(sample.power) || sample.power < 0.0) continue;
        int zoneIndex = input.powerZones.size() - 1;
        for (int index = 0; index < input.powerZones.size(); ++index) {
            if (sample.power < input.powerZones.at(index).high) {
                zoneIndex = index;
                break;
            }
        }
        seconds[zoneIndex] += recordingInterval;
    }

    QStringList values;
    for (int index = 0; index < input.powerZones.size(); ++index) {
        if (seconds.at(index) < 30.0) continue;
        const QString name = input.powerZones.at(index).name.trimmed().isEmpty()
            ? QStringLiteral("Z%1").arg(index + 1)
            : input.powerZones.at(index).name.trimmed();
        values.append(QStringLiteral("%1 %2")
            .arg(name, zoneDurationText(seconds.at(index))));
    }
    return values.join(QStringLiteral(" | "));
}

double intervalAveragePower(
    const StravaActivityDescription::Input &input,
    const StravaActivityDescription::Interval &interval)
{
    double total = 0.0;
    int count = 0;
    for (const auto &sample : input.samples) {
        if (sample.seconds < interval.start || sample.seconds > interval.stop)
            continue;
        if (!std::isfinite(sample.power) || sample.power < 0.0) continue;
        total += sample.power;
        ++count;
    }
    return count > 0 ? total / count : 0.0;
}

QString intervalLine(
    const StravaActivityDescription::Input &input,
    double activityDuration,
    double activityAveragePower)
{
    struct Group {
        int count = 0;
        double durationTotal = 0.0;
        double powerTotal = 0.0;
        double firstStart = 0.0;
    };

    QVector<Group> groups;
    int usefulIntervalCount = 0;
    for (const auto &interval : input.intervals) {
        const double duration = interval.stop - interval.start;
        if (!positive(duration)
            || (positive(activityDuration)
                && duration >= activityDuration * 0.85)) {
            continue;
        }
        ++usefulIntervalCount;
        const double power = input.hasPower
            ? intervalAveragePower(input, interval) : 0.0;
        if (input.hasPower && positive(activityAveragePower)
            && power < activityAveragePower * 1.05) {
            continue;
        }

        Group *matching = nullptr;
        for (auto &group : groups) {
            const double groupDuration = group.durationTotal / group.count;
            const double groupPower = group.powerTotal / group.count;
            const bool similarDuration = std::abs(duration - groupDuration)
                <= qMax(5.0, groupDuration * 0.08);
            const bool similarPower = !input.hasPower
                || !positive(groupPower)
                || std::abs(power - groupPower)
                    <= qMax(15.0, groupPower * 0.15);
            if (similarDuration && similarPower) {
                matching = &group;
                break;
            }
        }
        if (!matching) {
            groups.append(Group{0, 0.0, 0.0, interval.start});
            matching = &groups.last();
        }
        ++matching->count;
        matching->durationTotal += duration;
        matching->powerTotal += power;
    }

    std::sort(groups.begin(), groups.end(), [](const Group &left, const Group &right) {
        return left.firstStart < right.firstStart;
    });
    QStringList repeated;
    for (const auto &group : groups) {
        if (group.count < 2) continue;
        QString text = QStringLiteral("%1 x %2")
            .arg(group.count)
            .arg(durationText(group.durationTotal / group.count));
        if (input.hasPower && positive(group.powerTotal)) {
            text += QStringLiteral(" @ %1 W")
                .arg(qRound(group.powerTotal / group.count));
        }
        repeated.append(text);
        if (repeated.size() == 3) break;
    }

    if (!repeated.isEmpty()) return repeated.join(QStringLiteral(" | "));
    if (usefulIntervalCount > 1)
        return StravaActivityDescription::tr("%1 completed")
            .arg(usefulIntervalCount);
    return {};
}

} // namespace

StravaActivityDescription::Mode
StravaActivityDescription::modeFromSetting(const QString &setting)
{
    if (setting == QStringLiteral("Automatic summary"))
        return Mode::SummaryOnly;
    if (setting == QStringLiteral("Notes + automatic summary"))
        return Mode::NotesAndSummary;
    return Mode::NotesOnly;
}

QString
StravaActivityDescription::summary(const Input &input)
{
    const CalculatedMetrics calculated = calculate(input);
    const double duration = resolved(input.duration, calculated.duration);
    const double work = resolved(input.work, calculated.work);
    const double averagePower = resolved(
        input.averagePower, calculated.averagePower);
    const double maximumPower = resolved(
        input.maximumPower, calculated.maximumPower);
    const double normalizedPower = resolved(
        input.normalizedPower, calculated.normalizedPower);
    const double intensityFactor = resolved(
        input.intensityFactor, calculated.intensityFactor);
    const double bikeStress = resolved(
        input.bikeStress, calculated.bikeStress);
    const double averageHeartRate = resolved(
        input.averageHeartRate, calculated.averageHeartRate);
    const double maximumHeartRate = resolved(
        input.maximumHeartRate, calculated.maximumHeartRate);
    const double averageCadence = resolved(
        input.averageCadence, calculated.averageCadence);

    QStringList lines;
    QStringList volume;
    if (positive(duration))
        volume.append(tr("Duration %1").arg(durationText(duration)));
    if (input.hasPower && positive(work))
        volume.append(tr("Work %1 kJ").arg(qRound(work)));
    if (!volume.isEmpty()) lines.append(volume.join(QStringLiteral(" | ")));

    if (input.hasPower && positive(averagePower)) {
        QStringList power;
        power.append(tr("%1 W avg").arg(qRound(averagePower)));
        if (positive(normalizedPower))
            power.append(tr("%1 W NP").arg(qRound(normalizedPower)));
        if (positive(maximumPower))
            power.append(tr("%1 W max").arg(qRound(maximumPower)));
        lines.append(tr("Power %1").arg(power.join(QStringLiteral(" | "))));
    }

    if (input.hasHeartRate && positive(averageHeartRate)) {
        QStringList heartRate;
        heartRate.append(tr("%1 bpm avg").arg(qRound(averageHeartRate)));
        if (positive(maximumHeartRate))
            heartRate.append(tr("%1 bpm max").arg(qRound(maximumHeartRate)));
        lines.append(tr("HR %1").arg(
            heartRate.join(QStringLiteral(" | "))));
    }

    QStringList load;
    if (positive(intensityFactor))
        load.append(QStringLiteral("IF %1").arg(
            intensityFactor, 0, 'f', 2));
    if (positive(bikeStress))
        load.append(tr("BikeStress %1").arg(qRound(bikeStress)));
    if (input.hasCadence && positive(averageCadence))
        load.append(tr("Cadence %1 rpm").arg(qRound(averageCadence)));
    if (!load.isEmpty()) lines.append(load.join(QStringLiteral(" | ")));

    const QString intervals = intervalLine(input, duration, averagePower);
    if (!intervals.isEmpty()) lines.append(tr("Intervals %1").arg(intervals));

    const QString sparkline = powerSparkline(input);
    if (!sparkline.isEmpty()) lines.append(tr("Power %1").arg(sparkline));

    const QString zones = powerZoneLine(input);
    if (!zones.isEmpty()) lines.append(tr("Zones %1").arg(zones));

    return lines.join(QLatin1Char('\n'));
}

QString
StravaActivityDescription::compose(
    const QString &notes,
    bool notesUsedAsActivityName,
    Mode mode,
    const QString &automaticSummary)
{
    const QString usableNotes = notesUsedAsActivityName
        ? QString() : notes.trimmed();
    const QString usableSummary = automaticSummary.trimmed();

    switch (mode) {
    case Mode::SummaryOnly:
        return usableSummary;
    case Mode::NotesAndSummary:
        if (usableNotes.isEmpty()) return usableSummary;
        if (usableSummary.isEmpty()) return usableNotes;
        return usableNotes + QStringLiteral("\n\n") + usableSummary;
    case Mode::NotesOnly:
    default:
        return usableNotes;
    }
}
