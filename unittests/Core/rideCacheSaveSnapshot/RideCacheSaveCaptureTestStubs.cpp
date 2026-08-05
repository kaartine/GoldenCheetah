#include "IntervalItem.h"
#include "RideItem.h"
#include "RideMetric.h"

QList<QString> RideMetricFactory::compatibilitymetrics;

RideItem::RideItem()
    : ride_(nullptr),
      fileCache_(nullptr),
      context(nullptr),
      isdirty(false),
      isstale(true),
      isedit(false),
      skipsave(false),
      color(Qt::black),
      planned(false),
      isBike(false),
      isRun(false),
      isSwim(false),
      isXtrain(false),
      isAero(false),
      samples(false),
      zoneRange(-1),
      hrZoneRange(-1),
      paceZoneRange(-1),
      fingerprint(0),
      metacrc(0),
      crc(0),
      timestamp(0),
      dbversion(0),
      udbversion(0),
      weight(0)
{
}

RideItem::~RideItem() = default;
void RideItem::modified() {}
void RideItem::reverted() {}
void RideItem::saved() {}
void RideItem::notifyRideDataChanged() {}
void RideItem::notifyRideMetadataChanged() {}

IntervalItem::IntervalItem()
    : rideItem_(nullptr),
      selected(false),
      type(RideFileInterval::USER),
      start(0),
      stop(0),
      startKM(0),
      stopKM(0),
      displaySequence(0),
      color(Qt::black),
      test(false),
      rideInterval(nullptr)
{
}

void RenameIntervalDialog::applyClicked() {}
void RenameIntervalDialog::cancelClicked() {}
void EditIntervalDialog::applyClicked() {}
void EditIntervalDialog::cancelClicked() {}
