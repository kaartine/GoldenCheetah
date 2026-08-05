#include <QtTest>

#include "IntervalItem.h"
#include "PowerHist.h"

class PowerHistSelectionAccess : private PowerHist
{
public:
    using PowerHist::isSelected;
};

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

QList<IntervalItem *> RideItem::intervalsSelected() const
{
    QList<IntervalItem *> selected;
    for (IntervalItem *interval : intervals_) {
        if (interval && interval->selected) selected.append(interval);
    }
    return selected;
}

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

class TestPowerHistSelection : public QObject
{
    Q_OBJECT

private slots:
    void nullRideOrPointIsNotSelected();
    void rideWithoutSelectionsDoesNotSelectPoints();
    void pointsRespectOpenOverlapBoundaries();
    void multipleIntervalsAndSelectionFlagsAreHonored();
};

void TestPowerHistSelection::nullRideOrPointIsNotSelected()
{
    RideFilePoint point;
    RideItem ride;

    QVERIFY(!PowerHistSelectionAccess::isSelected(nullptr, &point, 1.0));
    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, nullptr, 1.0));
}

void TestPowerHistSelection::rideWithoutSelectionsDoesNotSelectPoints()
{
    RideItem ride;
    RideFilePoint point;
    point.secs = 15.0;

    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));
}

void TestPowerHistSelection::pointsRespectOpenOverlapBoundaries()
{
    RideItem ride;
    IntervalItem interval;
    interval.selected = true;
    interval.start = 10.0;
    interval.stop = 20.0;
    ride.intervals().append(&interval);

    RideFilePoint point;

    point.secs = 10.0;
    QVERIFY(PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));

    point.secs = 9.0;
    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));

    point.secs = 19.0;
    QVERIFY(PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));

    point.secs = 20.0;
    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));

    point.secs = 25.0;
    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));
}

void TestPowerHistSelection::multipleIntervalsAndSelectionFlagsAreHonored()
{
    RideItem ride;
    IntervalItem first;
    first.selected = true;
    first.start = 10.0;
    first.stop = 20.0;

    IntervalItem second;
    second.selected = true;
    second.start = 30.0;
    second.stop = 40.0;

    IntervalItem ignored;
    ignored.selected = false;
    ignored.start = 50.0;
    ignored.stop = 60.0;

    ride.intervals().append(&first);
    ride.intervals().append(nullptr);
    ride.intervals().append(&second);
    ride.intervals().append(&ignored);

    RideFilePoint point;
    point.secs = 35.0;
    QVERIFY(PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));

    point.secs = 55.0;
    QVERIFY(!PowerHistSelectionAccess::isSelected(&ride, &point, 1.0));
}

QTEST_APPLESS_MAIN(TestPowerHistSelection)

#include "testPowerHistSelection.moc"
