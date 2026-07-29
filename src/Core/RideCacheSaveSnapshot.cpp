/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "RideCacheSaveSnapshot.h"

#include "RideCachePersistence.h"

#include <QBuffer>
#include <QTextStream>

#include <cmath>

namespace {

QString protectedString(QString value)
{
    value.replace("\\", "\\\\");
    value.replace("\"", "\\\"");
    value.replace("\t", "\\t");
    value.replace("\n", "\\n");
    value.replace("\r", "\\r");
    value.replace("\b", "\\b");
    value.replace("\f", "\\f");
    value.replace("/", "\\/");
    value += " ";
    return value;
}

QString nameNumber(
    const QString &prefix,
    const QString &name,
    const QString &middle,
    double value,
    const QString &suffix)
{
    QString number;
    number.setNum(value, 'f', 5);

    QString result;
    result.reserve(
        prefix.size() + name.size() + middle.size()
        + number.size() + suffix.size());
    result.append(prefix);
    result.append(name);
    result.append(middle);
    result.append(number);
    result.append(suffix);
    return result;
}

QString nameNumbers(
    const QString &prefix,
    const QString &name,
    const QString &firstSeparator,
    double first,
    const QString &secondSeparator,
    double second,
    const QString &suffix)
{
    QString firstNumber;
    firstNumber.setNum(first, 'f', 5);
    QString secondNumber;
    secondNumber.setNum(second, 'f', 5);

    QString result;
    result.reserve(
        prefix.size() + name.size() + firstSeparator.size()
        + firstNumber.size() + secondSeparator.size()
        + secondNumber.size() + suffix.size());
    result.append(prefix);
    result.append(name);
    result.append(firstSeparator);
    result.append(firstNumber);
    result.append(secondSeparator);
    result.append(secondNumber);
    result.append(suffix);
    return result;
}

bool validateMetricValues(
    const RideCacheSave::MetricValues &values,
    int maximumIndex,
    const QString &description,
    QString &error)
{
    if (maximumIndex < 0) return true;
    if (values.values.size() <= maximumIndex) {
        error = QStringLiteral("%1 metric values do not match the schema")
                    .arg(description);
        return false;
    }
    if (values.counts.size() <= maximumIndex) {
        error = QStringLiteral("%1 metric counts do not match the schema")
                    .arg(description);
        return false;
    }
    return true;
}

bool validateSnapshot(
    const RideCacheSave::Snapshot &snapshot,
    QString &error)
{
    if (snapshot.version.isEmpty()) {
        error = QStringLiteral("Ride cache snapshot has no version");
        return false;
    }

    int maximumIndex = -1;
    for (const RideCacheSave::MetricDefinition &metric :
         snapshot.metrics) {
        if (metric.index < 0) {
            error = QStringLiteral(
                "Ride cache metric schema contains a negative index");
            return false;
        }
        maximumIndex = qMax(maximumIndex, metric.index);
    }

    for (qsizetype rideIndex = 0;
         rideIndex < snapshot.rides.size();
         ++rideIndex) {
        const RideCacheSave::Ride &ride =
            snapshot.rides.at(rideIndex);
        const QString rideDescription =
            QStringLiteral("Ride %1").arg(rideIndex);
        if (!validateMetricValues(
                ride.metricValues,
                maximumIndex,
                rideDescription,
                error)) {
            return false;
        }
        for (qsizetype intervalIndex = 0;
             intervalIndex < ride.intervals.size();
             ++intervalIndex) {
            if (!validateMetricValues(
                    ride.intervals.at(intervalIndex).metricValues,
                    maximumIndex,
                    QStringLiteral("Ride %1 interval %2")
                        .arg(rideIndex)
                        .arg(intervalIndex),
                    error)) {
                return false;
            }
        }
    }
    return true;
}

bool shouldWriteRideMetric(
    double value,
    double count,
    bool aggregateZero)
{
    return (!std::isinf(value) && !std::isnan(value)
            && value != 0.0)
        || (value == 0.0 && count > 1.0 && aggregateZero);
}

bool shouldWriteIntervalMetric(
    double value,
    double rideValue,
    double rideCount,
    bool aggregateZero)
{
    return value > 0.0 || value < 0.0
        || (rideValue == 0.0
            && rideCount > 1.0
            && aggregateZero);
}

void writeMetric(
    QTextStream &stream,
    const QString &indent,
    const RideCacheSave::MetricDefinition &definition,
    const RideCacheSave::MetricValues &values)
{
    const int index = definition.index;
    const double value = values.values.at(index);
    const double count = values.counts.at(index);
    const double stdMean = values.stdMeans.value(index, 0.0);
    const double stdVariance =
        values.stdVariances.value(index, 0.0);

    if (stdMean != 0.0 || stdVariance != 0.0) {
        stream << indent << "\"" << definition.name
               << "\":[\"" << QString("%1").arg(value, 0, 'f', 5)
               << "\",\"" << QString("%1").arg(count, 0, 'f', 5)
               << "\",\"" << QString("%1").arg(stdMean, 0, 'f', 5)
               << "\",\"" << QString("%1").arg(
                                  stdVariance, 0, 'f', 5)
               << "\"]";
    } else if (count == 0.0) {
        stream << nameNumber(
            indent + "\"",
            definition.name,
            "\":\"",
            value,
            "\"");
    } else {
        stream << nameNumbers(
            indent + "\"",
            definition.name,
            "\":[\"",
            value,
            "\",\"",
            count,
            "\"]");
    }
}

} // namespace

namespace RideCacheSave {

bool deferTarget(
    QStringList &pendingTargets,
    bool refreshActive,
    const QString &targetPath)
{
    if (!refreshActive) return false;
    if (!pendingTargets.contains(targetPath)) {
        pendingTargets.append(targetPath);
    }
    return true;
}

QStringList takeDeferredTargets(QStringList &pendingTargets)
{
    QStringList result;
    result.swap(pendingTargets);
    return result;
}

QStringList takeDeferredTargetsForCancellation(
    QStringList &pendingTargets,
    const QString &defaultTarget,
    bool exiting,
    bool cacheComplete)
{
    QStringList result = takeDeferredTargets(pendingTargets);
    if (exiting && !cacheComplete) {
        result.removeAll(defaultTarget);
    }
    return result;
}

bool serialize(
    const Snapshot &snapshot,
    QByteArray &document,
    QString &error)
{
    error.clear();
    if (!validateSnapshot(snapshot, error)) return false;

    QByteArray serialized;
    QBuffer buffer(&serialized);
    if (!buffer.open(QIODevice::WriteOnly)) {
        error = QStringLiteral("Cannot prepare the ride cache: %1")
                    .arg(buffer.errorString());
        return false;
    }

    QTextStream stream(&buffer);
    stream.setGenerateByteOrderMark(true);
    stream << "{";
    stream << QStringLiteral("\n  \"VERSION\":\"%1\",")
                  .arg(snapshot.version);
    stream << "\n  \"RIDES\":[\n";

    bool firstRide = true;
    for (const Ride &ride : snapshot.rides) {
        if (!firstRide) stream << ",\n";
        firstRide = false;

        stream << "\t{\n";
        stream << "\t\t\"date\":\""
               << ride.dateTime.toUTC().toString(
                      QStringLiteral("yyyy/MM/dd hh:mm:ss' UTC'"))
               << "\",\n";
        stream << "\t\t\"filename\":\"" << ride.fileName << "\",\n";
        stream << "\t\t\"fingerprint\":\""
               << ride.fingerprint << "\",\n";
        stream << "\t\t\"crc\":\"" << ride.crc << "\",\n";
        stream << "\t\t\"metacrc\":\""
               << ride.metadataCrc << "\",\n";
        stream << "\t\t\"timestamp\":\""
               << ride.timestamp << "\",\n";
        stream << "\t\t\"dbversion\":\""
               << ride.databaseVersion << "\",\n";
        stream << "\t\t\"udbversion\":\""
               << ride.userDatabaseVersion << "\",\n";
        stream << "\t\t\"color\":\"" << ride.color << "\",\n";
        stream << "\t\t\"present\":\"" << ride.present << "\",\n";
        stream << "\t\t\"sport\":\"" << ride.sport << "\",\n";
        stream << "\t\t\"aero\":\""
               << (ride.aero ? "1" : "0") << "\",\n";
        stream << "\t\t\"weight\":\"" << ride.weight << "\",\n";

        if (ride.zoneRange >= 0) {
            stream << "\t\t\"zonerange\":\""
                   << ride.zoneRange << "\",\n";
        }
        if (ride.hrZoneRange >= 0) {
            stream << "\t\t\"hrzonerange\":\""
                   << ride.hrZoneRange << "\",\n";
        }
        if (ride.paceZoneRange >= 0) {
            stream << "\t\t\"pacezonerange\":\""
                   << ride.paceZoneRange << "\",\n";
        }
        if (!ride.overrides.isEmpty()) {
            stream << "\t\t\"overrides\":\""
                   << ride.overrides.join(",") << "\",\n";
        }
        stream << "\t\t\"samples\":\""
               << (ride.samples ? "1" : "0") << "\",\n";

        stream << "\n\t\t\"METRICS\":{\n";
        bool firstMetric = true;
        for (const MetricDefinition &definition : snapshot.metrics) {
            const double value =
                ride.metricValues.values.at(definition.index);
            const double count =
                ride.metricValues.counts.at(definition.index);
            if (!shouldWriteRideMetric(
                    value, count, definition.aggregateZero)) {
                continue;
            }
            if (!firstMetric) stream << ",\n";
            firstMetric = false;
            writeMetric(
                stream,
                QStringLiteral("\t\t\t"),
                definition,
                ride.metricValues);
        }
        stream << "\n\t\t}";

        if (!ride.metadata.isEmpty()) {
            stream << ",\n\t\t\"TAGS\":{\n";
            auto metadata = ride.metadata.constBegin();
            while (metadata != ride.metadata.constEnd()) {
                stream << "\t\t\t\"" << metadata.key()
                       << "\":\""
                       << protectedString(metadata.value()) << "\"";
                ++metadata;
                stream << (metadata != ride.metadata.constEnd()
                               ? ",\n"
                               : "\n");
            }
            stream << "\n\t\t}";
        }

        if (!ride.xdata.isEmpty()) {
            stream << ",\n\t\t\"XDATA\":{\n";
            auto xdata = ride.xdata.constBegin();
            while (xdata != ride.xdata.constEnd()) {
                stream << "\t\t\t\"" << xdata.key() << "\":[ ";
                bool firstValue = true;
                for (const QString &value : xdata.value()) {
                    if (!firstValue) stream << ", ";
                    stream << "\"" << protectedString(value) << "\"";
                    firstValue = false;
                }
                ++xdata;
                stream << (xdata != ride.xdata.constEnd()
                               ? "],\n"
                               : "]\n");
            }
            stream << "\n\t\t}";
        }

        if (!ride.intervals.isEmpty()) {
            stream << ",\n\t\t\"INTERVALS\":[\n";
            bool firstInterval = true;
            for (const Interval &interval : ride.intervals) {
                if (!firstInterval) stream << ",\n";
                firstInterval = false;

                stream << "\t\t\t{\n";
                stream << "\t\t\t\"name\":\""
                       << protectedString(interval.name) << "\",\n";
                stream << "\t\t\t\"start\":\""
                       << interval.start << "\",\n";
                stream << "\t\t\t\"stop\":\""
                       << interval.stop << "\",\n";
                stream << "\t\t\t\"startKM\":\""
                       << interval.startKm << "\",\n";
                stream << "\t\t\t\"stopKM\":\""
                       << interval.stopKm << "\",\n";
                stream << "\t\t\t\"type\":\""
                       << interval.type << "\",\n";
                stream << "\t\t\t\"test\":\""
                       << (interval.test ? "true" : "false")
                       << "\",\n";
                stream << "\t\t\t\"color\":\""
                       << interval.color << "\",\n";
                if (!interval.route.isEmpty()) {
                    stream << "\t\t\t\"route\":\""
                           << interval.route << "\",\n";
                }
                stream << "\t\t\t\"seq\":\""
                       << interval.sequence << "\"";

                bool hasMetrics = false;
                for (double value :
                     interval.metricValues.values) {
                    if (value > 0.0 || value < 0.0) {
                        hasMetrics = true;
                        break;
                    }
                }
                if (hasMetrics) {
                    stream << ",\n\n\t\t\t\"METRICS\":{\n";
                    bool firstIntervalMetric = true;
                    for (const MetricDefinition &definition :
                         snapshot.metrics) {
                        const int index = definition.index;
                        if (!shouldWriteIntervalMetric(
                                interval.metricValues.values.at(index),
                                ride.metricValues.values.at(index),
                                ride.metricValues.counts.at(index),
                                definition.aggregateZero)) {
                            continue;
                        }
                        if (!firstIntervalMetric) stream << ",\n";
                        firstIntervalMetric = false;
                        writeMetric(
                            stream,
                            QStringLiteral("\t\t\t\t"),
                            definition,
                            interval.metricValues);
                    }
                    stream << "\n\t\t\t\t}";
                }
                stream << "\n\t\t\t}";
            }
            stream << "\n\t\t]";
        }

        stream << "\n\t}";
    }

    stream << "\n  ]\n}";
    stream.flush();
    if (stream.status() != QTextStream::Ok) {
        error = QStringLiteral(
            "Cannot serialize the complete ride cache");
        return false;
    }
    buffer.close();
    document = serialized;
    return true;
}

bool write(
    const Snapshot &snapshot,
    QString &error,
    const AtomicFileWriterFactory &writerFactory)
{
    if (snapshot.targetPath.isEmpty()) {
        error = QStringLiteral(
            "Ride cache snapshot has no target path");
        return false;
    }

    QByteArray document;
    if (!serialize(snapshot, document, error)) return false;
    return writeRideCacheAtomically(
        snapshot.targetPath, document, error, writerFactory);
}

} // namespace RideCacheSave
