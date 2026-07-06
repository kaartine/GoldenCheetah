#ifndef GC_SPLIT_RIDE_DATA_H
#define GC_SPLIT_RIDE_DATA_H

class RideFile;

enum class SplitSegmentEnd
{
    Exclude,
    Include
};

RideFile *extractSplitRideSegment(
    const RideFile &source,
    long startIndex,
    long stopIndex,
    SplitSegmentEnd segmentEnd);

#endif
