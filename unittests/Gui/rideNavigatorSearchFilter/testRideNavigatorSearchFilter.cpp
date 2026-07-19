#include <QtTest>

#include <QElapsedTimer>

#include "RideNavigatorSearchFilter.h"

class ActivityModel final : public QAbstractTableModel
{
public:
    explicit ActivityModel(int rows, QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
        filenames_.reserve(rows);
        for (int row = 0; row < rows; ++row) {
            filenames_.append(
                QStringLiteral("activity-%1.fit")
                    .arg(row, 5, 10, QLatin1Char('0')));
        }
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : filenames_.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 3;
    }

    QVariant data(
        const QModelIndex &index,
        int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole) return QVariant();

        switch (index.column()) {
        case 0:
            return filenames_.at(index.row());
        case 1:
            return isPlanned(index.row());
        case 2:
            return false;
        default:
            return QVariant();
        }
    }

    QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QVariant();
        }

        switch (section) {
        case 0:
            return QStringLiteral("filename");
        case 1:
            return QStringLiteral("planned");
        case 2:
            return QStringLiteral("dirty");
        default:
            return QVariant();
        }
    }

    QString filename(int row) const
    {
        return filenames_.at(row);
    }

private:
    static bool isPlanned(int row)
    {
        return row % 3 == 0;
    }

    QStringList filenames_;
};

class TestRideNavigatorSearchFilter : public QObject
{
    Q_OBJECT

private slots:
    void matchesExactFilenamesAndReplacesSearches();
    void combinesSearchAndDisplayFilters();
    void filtersFiftyThousandRowsWithinBudget();
};

void TestRideNavigatorSearchFilter::matchesExactFilenamesAndReplacesSearches()
{
    ActivityModel model(6);
    SearchFilter filter;
    filter.setSourceModel(&model);

    filter.setStrings({
        model.filename(1),
        model.filename(3),
        model.filename(3),
        QStringLiteral("ACTIVITY-00005.FIT")
    });
    QCOMPARE(filter.rowCount(), 2);

    filter.setStrings({model.filename(2)});
    QCOMPARE(filter.rowCount(), 1);

    filter.clearStrings();
    QCOMPARE(filter.rowCount(), 6);
}

void TestRideNavigatorSearchFilter::combinesSearchAndDisplayFilters()
{
    ActivityModel model(12);
    SearchFilter filter;
    filter.setSourceModel(&model);

    filter.setDisplayFilter(RideNavFilter::COMPLETED);
    QCOMPARE(filter.rowCount(), 8);

    filter.setDisplayFilter(RideNavFilter::PLANNED);
    QCOMPARE(filter.rowCount(), 4);

    filter.setStrings({model.filename(0), model.filename(1)});
    QCOMPARE(filter.rowCount(), 1);

    filter.setDisplayFilter(RideNavFilter::ALL);
    QCOMPARE(filter.rowCount(), 2);
}

void TestRideNavigatorSearchFilter::filtersFiftyThousandRowsWithinBudget()
{
    constexpr int RowCount = 50000;
    constexpr int MatchCount = RowCount / 2;
    constexpr qint64 BudgetMilliseconds = 1000;

    ActivityModel model(RowCount);
    SearchFilter filter;
    filter.setSourceModel(&model);

    QStringList matches;
    matches.reserve(MatchCount);
    for (int row = 0; row < RowCount; row += 2) {
        matches.append(model.filename(row));
    }

    QElapsedTimer timer;
    timer.start();
    filter.setStrings(matches);
    QCOMPARE(filter.rowCount(), MatchCount);
    const qint64 elapsed = timer.elapsed();

    QVERIFY2(
        elapsed < BudgetMilliseconds,
        qPrintable(QStringLiteral("50k-row filtering took %1 ms").arg(elapsed)));
}

QTEST_APPLESS_MAIN(TestRideNavigatorSearchFilter)

#include "testRideNavigatorSearchFilter.moc"
