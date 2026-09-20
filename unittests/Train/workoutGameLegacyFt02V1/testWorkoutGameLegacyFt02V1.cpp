/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <QtTest/QtTest>
#include "Train/WorkoutGameFeatureGeometry.h"
#include "Train/WorkoutGameLegacyFt02V1.h"

#include <cstring>
#include <limits>

namespace {

bool identical(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

// Literal observations from the untouched a4bec4b7/1d982d3c implementation,
// not generated from the helper under test. Keep the non-permille inputs.
struct Golden { double difficulty, x, radius, height; };
constexpr Golden Goldens[] = {
    {0, -0.2, 0.22, 0.17811911541508341},
    {0, -0.123456, 0.22, 0.35403406407951088},
    {0, 0, 0.22, 0.44},
    {0, 0.071234, 0.22, 0.41166135284415489},
    {0, 0.2, 0.22, 0.17811911541508335},
    {0.0004, -0.2, 0.22004000000000001, 0.17826034476426186},
    {0.0004, -0.123456, 0.22004000000000001, 0.3541284305136792},
    {0.0004, 0, 0.22004000000000001, 0.44008000000000003},
    {0.0004, 0.071234, 0.22004000000000001, 0.41174135284415492},
    {0.0004, 0.2, 0.22004000000000001, 0.17826034476426181},
    {0.123456789, -0.2, 0.23234567889999999, 0.22170842032040045},
    {0.123456789, -0.123456, 0.23234567889999999, 0.38315950645899466},
    {0.123456789, 0, 0.23234567889999999, 0.46469135779999998},
    {0.123456789, 0.071234, 0.23234567889999999, 0.43635271064415487},
    {0.123456789, 0.2, 0.23234567889999999, 0.22170842032040045},
    {0.5, -0.2, 0.27000000000000002, 0.35465580188810131},
    {0.5, -0.123456, 0.27000000000000002, 0.47199210678983838},
    {0.5, 0, 0.27000000000000002, 0.54000000000000004},
    {0.5, 0.071234, 0.27000000000000002, 0.51166135284415493},
    {0.5, 0.2, 0.27000000000000002, 0.35465580188810136},
    {0.50049, -0.2, 0.27004899999999998, 0.35482880784084475},
    {0.50049, -0.123456, 0.27004899999999998, 0.47210770567169436},
    {0.50049, 0, 0.27004899999999998, 0.54009799999999997},
    {0.50049, 0.071234, 0.27004899999999998, 0.51175935284415486},
    {0.50049, 0.2, 0.27004899999999998, 0.35482880784084475},
    {0.9996, -0.2, 0.31996000000000002, 0.48756565174420796},
    {0.9996, -0.123456, 0.31996000000000002, 0.58985578306599751},
    {0.9996, 0, 0.31996000000000002, 0.63992000000000004},
    {0.9996, 0.071234, 0.31996000000000002, 0.61158135284415494},
    {0.9996, 0.2, 0.31996000000000002, 0.48756565174420796},
    {1, -0.2, 0.32, 0.48766001817837618},
    {1, -0.123456, 0.32, 0.58995014950016578},
    {1, 0, 0.32, 0.64},
    {1, 0.071234, 0.32, 0.6116613528441549},
    {1, 0.2, 0.32, 0.48766001817837618}
};

// Pinned pre-extraction oracle for boundary/operation-order checks on the
// same platform. Intentionally independent of production constants/helpers.
double originalOffset(const WorkoutGameFeatureGeometryProfile &p, double x)
{
    if (!p.ready || x < p.startMeters || x > p.endMeters) return 0.0;
    if (x <= p.startMeters || x >= p.endMeters) return 0.0;
    const double radius = p.heightMeters * 0.5;
    constexpr double Pi = 3.14159265358979323846;
    for (int segment = 0; segment < 16 / 2; ++segment) {
        const double fromAngle = Pi - double(segment) * 2.0 * Pi / double(16);
        const double toAngle = Pi - double(segment + 1) * 2.0 * Pi / double(16);
        const double fromX = std::cos(fromAngle) * radius;
        const double toX = std::cos(toAngle) * radius;
        if (x <= toX + 1e-12) {
            const double amount = std::clamp((x - fromX) / (toX - fromX), 0.0, 1.0);
            const double fromY = std::sin(fromAngle) * p.heightMeters;
            const double toY = std::sin(toAngle) * p.heightMeters;
            return fromY + (toY - fromY) * amount;
        }
    }
    return radius;
}

auto profile(double difficulty)
{
    return WorkoutGameFeatureGeometry::profile(WorkoutGameTerrainKind::LogOver, difficulty);
}

} // namespace

class TestWorkoutGameLegacyFt02V1 : public QObject
{
    Q_OBJECT
private slots:
    void frozenHelperHasIndependentContract()
    {
        QCOMPARE(WorkoutGameLegacyFt02V1::RadialSegments, 16);
        QCOMPARE(WorkoutGameLogRadialSegments, WorkoutGameLegacyFt02V1::RadialSegments);
        for (const auto &golden : Goldens) {
            const double radius = WorkoutGameLegacyFt02V1::radiusMeters(golden.difficulty);
            QVERIFY(identical(radius, golden.radius));
            const double height = WorkoutGameLegacyFt02V1::surfaceOffsetMeters(
                    golden.x, -radius, radius, 2.0 * radius);
            QVERIFY(std::abs(height - golden.height) <= 1e-14);
            QVERIFY(identical(height, profile(golden.difficulty).surfaceOffset(golden.x)));
        }
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (double d : {-1.0, -0.0, nan, inf, -inf})
            QVERIFY(identical(WorkoutGameLegacyFt02V1::radiusMeters(d), 0.22));
        QVERIFY(identical(WorkoutGameLegacyFt02V1::radiusMeters(2.0), 0.32));
        QVERIFY(identical(WorkoutGameLegacyFt02V1::surfaceOffsetMeters(
                nan, -0.6, 0.6, 0.7), 0.35));
    }

    void capturedGoldenProfiles()
    {
        QCOMPARE(WorkoutGameLogRadialSegments, 16);
        for (const auto &golden : Goldens) {
            const auto p = profile(golden.difficulty);
            QVERIFY(p.ready);
            QVERIFY(identical(p.startMeters, -golden.radius));
            QVERIFY(identical(p.endMeters, golden.radius));
            QVERIFY(identical(p.heightMeters, 2.0 * golden.radius));
            QCOMPARE(p.shape, WorkoutGameFeatureGeometryShape::FacetedLog);
            QVERIFY(p.plateauStartMeters == 0.0 && p.plateauEndMeters == 0.0);
            QVERIFY(p.difficulty == 0.0 && p.landingStartMeters == 0.0
                    && p.recoveryStartMeters == 0.0);
            // Only literal libm-dependent samples allow a small absolute
            // cross-platform tolerance (10 femtometres). Other checks below
            // compare exact same-platform representations, not qFuzzyCompare.
            QVERIFY(std::abs(p.surfaceOffset(golden.x) - golden.height) <= 1e-14);
        }
    }

    void difficultyNormalizationIsNotQuantization()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (double d : {-1.0, -0.0, nan, inf, -inf})
            QVERIFY(identical(profile(d).heightMeters, profile(0.0).heightMeters));
        QVERIFY(identical(profile(2.0).heightMeters, profile(1.0).heightMeters));
        QVERIFY(!identical(profile(0.50049).heightMeters, profile(0.5).heightMeters));
    }

    void supportAndNonfiniteDistances()
    {
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (double d : {0.0, 0.123456789, 0.5, 1.0}) {
            auto p = profile(d);
            for (double x : {p.startMeters, p.endMeters, -inf, inf,
                            std::nextafter(p.startMeters, -inf),
                            std::nextafter(p.endMeters, inf)})
                QVERIFY(identical(p.surfaceOffset(x), 0.0));
            QVERIFY(identical(p.surfaceOffset(nan), p.heightMeters * 0.5));
            QVERIFY(p.surfacePresent(nan));
            p.ready = false;
            QVERIFY(identical(p.surfaceOffset(nan), 0.0));
        }
    }

    void allFacetsAndFractionalAnchorsRetainOperationOrder()
    {
        const double inf = std::numeric_limits<double>::infinity();
        constexpr double Pi = 3.14159265358979323846;
        for (double d : {0.0, 0.0004, 0.123456789, 0.5, 0.50049, 0.9996, 1.0}) {
            const auto p = profile(d);
            for (int facet = 0; facet <= 8; ++facet) {
                const double boundary = std::cos(Pi - double(facet) * 2.0 * Pi / 16.0)
                        * (p.heightMeters * 0.5);
                const double threshold = boundary + 1e-12;
                for (double x : {boundary, std::nextafter(boundary, -inf),
                                std::nextafter(boundary, inf),
                                threshold, std::nextafter(threshold, -inf),
                                std::nextafter(threshold, inf),
                                boundary - 1.1e-12, boundary + 1.1e-12}) {
                    for (double anchor : {0.0, 12.3456789, 1000.0001234}) {
                        const double local = (anchor + x) - anchor;
                        QVERIFY(identical(p.surfaceOffset(local), originalOffset(p, local)));
                    }
                }
            }
            for (int sample = 0; sample <= 2048; ++sample) {
                const double x = -0.4 + double(sample) * 0.8 / 2048.0;
                QVERIFY(identical(p.surfaceOffset(x), originalOffset(p, x)));
            }
        }
    }

    void publicProfileFieldsRemainAuthoritative()
    {
        auto p = profile(0.5);
        p.startMeters = -0.6;
        p.endMeters = 0.6;
        p.heightMeters = 0.7;
        p.difficulty = 0.123456;
        const double samples[] = {-0.4, -0.123456, 0.0, 0.071234, 0.4};
        const double expected[] = {8.572527594031472e-17, 0.65088614954555379,
                                  0.69999999999999996, 0.67166135284415485,
                                  0.34999999999999998};
        for (int i = 0; i < 5; ++i) {
            QVERIFY(std::abs(p.surfaceOffset(samples[i]) - expected[i]) <= 1e-14);
            QVERIFY(identical(p.surfaceOffset(samples[i]), originalOffset(p, samples[i])));
        }
    }
};

QTEST_APPLESS_MAIN(TestWorkoutGameLegacyFt02V1)
#include "testWorkoutGameLegacyFt02V1.moc"
