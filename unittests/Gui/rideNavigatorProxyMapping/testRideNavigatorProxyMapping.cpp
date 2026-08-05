#include <QtTest>

#include "RideNavigatorProxy.h"

class ResettableActivityModel final : public QAbstractTableModel
{
public:
    explicit ResettableActivityModel(int rows, QObject *parent = nullptr)
        : QAbstractTableModel(parent), rows_(rows)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : rows_;
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : 3;
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || role != Qt::DisplayRole) return QVariant();
        return QStringLiteral("row-%1-column-%2")
            .arg(index.row())
            .arg(index.column());
    }

    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QVariant();
        }

        static const QStringList headers = {
            QStringLiteral("filename"),
            QStringLiteral("ride_date"),
            QStringLiteral("planned")
        };
        return section >= 0 && section < headers.size()
            ? headers.at(section)
            : QVariant();
    }

    void resetRows(int rows)
    {
        beginResetModel();
        rows_ = rows;
        endResetModel();
    }

private:
    int rows_;
};

QString GroupByModel::groupFromValue(QString,
                                     QString value,
                                     double,
                                     double) const
{
    return value;
}

QColor GCColor::getColor(int)
{
    return QColor(Qt::black);
}

RideFile *RideItem::ride(bool)
{
    return nullptr;
}

void RideNavigator::resetView()
{
}

class TestRideNavigatorProxyMapping : public QObject
{
    Q_OBJECT

private slots:
    void rowZeroMapsToProxy();
    void everyRowAndColumnRoundTrips();
    void groupedRowsRoundTripWithoutTransientPointers();
    void modelResetInvalidatesAndRebuildsMappings();
};

void TestRideNavigatorProxyMapping::rowZeroMapsToProxy()
{
    ResettableActivityModel source(4);
    GroupByModel proxy(nullptr);
    proxy.setSourceModel(&source);

    const QModelIndex mapped = proxy.mapFromSource(source.index(0, 0));
    QVERIFY(mapped.isValid());
    QCOMPARE(proxy.mapToSource(mapped), source.index(0, 0));
}

void TestRideNavigatorProxyMapping::everyRowAndColumnRoundTrips()
{
    ResettableActivityModel source(5);
    GroupByModel proxy(nullptr);
    proxy.setSourceModel(&source);

    for (int row = source.rowCount() - 1; row >= 0; --row) {
        for (int column = 0; column < source.columnCount(); ++column) {
            const QModelIndex sourceIndex = source.index(row, column);
            const QModelIndex proxyIndex = proxy.mapFromSource(sourceIndex);
            QVERIFY2(proxyIndex.isValid(),
                     qPrintable(QStringLiteral("row %1 column %2")
                                    .arg(row)
                                    .arg(column)));
            QCOMPARE(proxy.mapToSource(proxyIndex), sourceIndex);
        }
    }
}

void TestRideNavigatorProxyMapping::groupedRowsRoundTripWithoutTransientPointers()
{
    ResettableActivityModel source(5);
    GroupByModel proxy(nullptr);
    proxy.setSourceModel(&source);
    proxy.setGroupBy(2);

    for (int row = source.rowCount() - 1; row >= 0; --row) {
        const QModelIndex sourceIndex = source.index(row, 1);
        const QModelIndex proxyIndex = proxy.mapFromSource(sourceIndex);
        QVERIFY(proxyIndex.isValid());
        QCOMPARE(proxy.mapToSource(proxyIndex), sourceIndex);
    }
}

void TestRideNavigatorProxyMapping::modelResetInvalidatesAndRebuildsMappings()
{
    ResettableActivityModel source(3);
    GroupByModel proxy(nullptr);
    proxy.setSourceModel(&source);

    QPersistentModelIndex stale(proxy.mapFromSource(source.index(2, 1)));
    QVERIFY(stale.isValid());

    source.resetRows(7);
    QVERIFY(!stale.isValid());

    for (int row = 0; row < source.rowCount(); ++row) {
        const QModelIndex sourceIndex = source.index(row, 1);
        const QModelIndex proxyIndex = proxy.mapFromSource(sourceIndex);
        QVERIFY(proxyIndex.isValid());
        QCOMPARE(proxy.mapToSource(proxyIndex), sourceIndex);
    }
}

QTEST_APPLESS_MAIN(TestRideNavigatorProxyMapping)

#include "testRideNavigatorProxyMapping.moc"
