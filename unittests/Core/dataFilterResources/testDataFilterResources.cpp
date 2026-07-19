#include <QtTest>

#include "DataFilterResources.h"

class CountingModel
{
public:
    CountingModel()
    {
        ++constructed;
        ++live;
    }

    ~CountingModel()
    {
        ++destroyed;
        --live;
    }

    static int constructed;
    static int destroyed;
    static int live;
};

int CountingModel::constructed = 0;
int CountingModel::destroyed = 0;
int CountingModel::live = 0;

class TestDataFilterResources : public QObject
{
    Q_OBJECT

private slots:
    void releasesTenThousandFilterResourceSets();
};

void TestDataFilterResources::releasesTenThousandFilterResourceSets()
{
    constexpr int FilterCount = 10000;
    constexpr int ModelsPerFilter = 5;

    gsl_rng_env_setup();
    CountingModel::constructed = 0;
    CountingModel::destroyed = 0;
    CountingModel::live = 0;

    for (int filter = 0; filter < FilterCount; ++filter) {
        {
            DataFilterResourceOwner<CountingModel> resources;
            for (int model = 0; model < ModelsPerFilter; ++model) {
                resources.addModel(new CountingModel());
            }
            resources.setRandomGenerator(
                gsl_rng_alloc(gsl_rng_default));

            QCOMPARE(resources.models().size(), ModelsPerFilter);
            QVERIFY(resources.randomGenerator() != nullptr);
            QCOMPARE(CountingModel::live, ModelsPerFilter);
        }
        QCOMPARE(CountingModel::live, 0);
    }

    QCOMPARE(
        CountingModel::constructed,
        FilterCount * ModelsPerFilter);
    QCOMPARE(CountingModel::destroyed, CountingModel::constructed);
}

QTEST_APPLESS_MAIN(TestDataFilterResources)

#include "testDataFilterResources.moc"
