#include "SplitRideData.h"

#include "RideFile.h"

namespace {

bool timeIsInSegment(
    double value,
    double start,
    double stop,
    SplitSegmentEnd segmentEnd)
{
    return value >= start
        && (segmentEnd == SplitSegmentEnd::Include
                ? value <= stop
                : value < stop);
}

} // namespace

RideFile *extractSplitRideSegment(
    const RideFile &source,
    long startIndex,
    long stopIndex,
    SplitSegmentEnd segmentEnd)
{
    const qsizetype pointCount = source.dataPoints().size();
    if (startIndex < 0 || stopIndex <= startIndex
        || startIndex >= pointCount || stopIndex >= pointCount) {
        return nullptr;
    }

    const RideFilePoint *startPoint =
        source.dataPoints().at(startIndex);
    const double offset = startPoint->secs;
    const double distanceOffset = startPoint->km;
    const double startTime = offset;
    const double stopTime =
        source.dataPoints().at(stopIndex)->secs;

    RideFile *result = new RideFile;
    result->setStartTime(
        source.startTime().addMSecs(qRound64(offset * 1000.0)));
    result->setRecIntSecs(source.recIntSecs());
    result->setDeviceType(source.deviceType());
    result->setFileFormat(source.fileFormat());
    for (auto it = source.tags().cbegin();
         it != source.tags().cend();
         ++it) {
        result->setTag(it.key(), it.value());
    }
    result->removeTag(QStringLiteral("Linked Filename"));

    const long pointEnd =
        segmentEnd == SplitSegmentEnd::Include
            ? stopIndex + 1
            : stopIndex;
    for (long index = startIndex; index < pointEnd; ++index) {
        RideFilePoint point = *source.dataPoints().at(index);
        point.secs -= offset;
        point.km -= distanceOffset;
        result->appendPoint(point);
    }

    for (auto it = source.xdata().cbegin();
         it != source.xdata().cend();
         ++it) {
        const XDataSeries *sourceSeries = it.value();
        XDataSeries *series = new XDataSeries;
        series->name = sourceSeries->name;
        series->valuename = sourceSeries->valuename;
        series->unitname = sourceSeries->unitname;
        series->valuetype = sourceSeries->valuetype;
        for (const XDataPoint *sourcePoint : sourceSeries->datapoints) {
            if (!timeIsInSegment(
                    sourcePoint->secs,
                    startTime,
                    stopTime,
                    segmentEnd)) {
                continue;
            }
            XDataPoint *point = new XDataPoint(*sourcePoint);
            point->secs -= offset;
            point->km -= distanceOffset;
            series->datapoints.append(point);
        }
        if (series->datapoints.isEmpty()) {
            delete series;
        } else {
            result->addXData(it.key(), series);
        }
    }

    for (const RideFileInterval *interval : source.intervals()) {
        if (!timeIsInSegment(
                interval->start,
                startTime,
                stopTime,
                segmentEnd)) {
            continue;
        }
        result->addInterval(
            RideFileInterval::USER,
            interval->start - offset,
            qMin(interval->stop, stopTime) - offset,
            interval->name);
    }

    return result;
}
