/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "Athlete.h"
#include "Colors.h"
#include "CompressedActivityFile.h"
#include "Context.h"
#include "FilterHRV.h"
#include "Specification.h"
#include "SplineLookup.h"
#include "Units.h"
#include "WPrime.h"
#include "Core/Settings.h"
#include "Core/Utils.h"
#include "FileIO/TTSReader.h"
#include "Metrics/Zones.h"
#include "Train/ZwoParser.h"
#include "Cloud/Strava.h"

#include <QDataStream>
#include <QThread>
#include <QTimeZone>

#include <atomic>

std::atomic_bool gpxSettingsReadBlocked{false};
std::atomic_bool gpxSettingsReadEntered{false};
std::atomic_int gpxSettingsReadCalls{0};

GSettings::GSettings(QString, QString)
    : newFormat(true)
{
}

GSettings::~GSettings() = default;

QVariant GSettings::value(
    const QObject *, const QString, const QVariant defaultValue)
{
    gpxSettingsReadCalls.fetch_add(1, std::memory_order_release);
    gpxSettingsReadEntered.store(true, std::memory_order_release);
    while (gpxSettingsReadBlocked.load(std::memory_order_acquire))
        QThread::msleep(1);
    return defaultValue;
}

QVariant GSettings::cvalue(
    QString, QString, QVariant defaultValue)
{
    return defaultValue;
}

namespace {
GSettings testSettings(
    QStringLiteral("GoldenCheetah"),
    QStringLiteral("StravaRoutesDownloadPipelineTest"));
}

GSettings *appsettings = &testSettings;

QDateTime convertToLocalTime(QString timestamp)
{
    QDateTime value = QDateTime::fromString(timestamp, Qt::ISODate);
    if (timestamp.endsWith(QLatin1Char('z'), Qt::CaseInsensitive)) {
        value.setTimeZone(QTimeZone::UTC);
        return value.toLocalTime();
    }
    return value;
}

GlobalContext *GlobalContext::context()
{
    return nullptr;
}

namespace CompressedActivityFile {

bool extractSingleFile(
    std::unique_ptr<QIODevice>,
    Format,
    QIODevice *)
{
    return false;
}

} // namespace CompressedActivityFile

void FilterHrv(XDataSeries *, double, double, double, int)
{
}

double Specification::secsStart() const
{
    return -1.0;
}

double Specification::secsEnd() const
{
    return -1.0;
}

double Athlete::getWeight(QDate, RideFile *)
{
    return 75.0;
}

double Athlete::getHeight(RideFile *)
{
    return 1.75;
}

void SplineLookup::update(
    const QwtSplineBasis &,
    const QPolygonF &,
    double)
{
}

double SplineLookup::valueY(double) const
{
    return 0.0;
}

WPrime::WPrime()
    : minY(0.0)
    , maxY(0.0)
    , TAU(0.0)
    , PCP_(0.0)
    , CP(0.0)
    , WPRIME(0.0)
    , EXP(0.0)
    , rideFile(nullptr)
    , last(0)
    , wasIntegral(false)
{
}

void WPrime::setRide(RideFile *ride)
{
    rideFile = ride;
}

QColor GCColor::getColor(int)
{
    return {};
}

QString kphToPace(double, bool, bool)
{
    return {};
}

namespace Utils {

QString xmlprotect(const QString &value)
{
    return value.toHtmlEscaped();
}

} // namespace Utils

int Zones::whichRange(const QDate &) const
{
    return -1;
}

int Zones::getCP(int) const
{
    return 300;
}

int Zones::whichZone(int, double) const
{
    return -1;
}

QList<int> Zones::getZoneHighs(int) const
{
    return {};
}

QList<QString> Zones::getZoneNames(int) const
{
    return {};
}

bool ZwoParser::startDocument()
{
    return false;
}

bool ZwoParser::endDocument()
{
    return false;
}

bool ZwoParser::endElement(
    const QString &, const QString &, const QString &)
{
    return false;
}

bool ZwoParser::startElement(
    const QString &,
    const QString &,
    const QString &,
    const QXmlAttributes &)
{
    return false;
}

bool ZwoParser::characters(const QString &)
{
    return false;
}

const std::vector<NS_TTSReader::Point> &
NS_TTSReader::TTSReader::getPoints() const
{
    static const std::vector<Point> empty;
    return empty;
}

const std::vector<NS_TTSReader::Segment> &
NS_TTSReader::TTSReader::getSegments() const
{
    static const std::vector<Segment> empty;
    return empty;
}

const std::wstring &NS_TTSReader::TTSReader::getRouteName() const
{
    static const std::wstring empty;
    return empty;
}

const std::wstring &
NS_TTSReader::TTSReader::getRouteDescription() const
{
    static const std::wstring empty;
    return empty;
}

bool NS_TTSReader::TTSReader::parseFile(QDataStream &)
{
    return false;
}

StravaAuthenticatedSession::Result Strava::authenticatedGet(
    const QUrl &,
    qsizetype,
    const CancellationCheck &)
{
    StravaAuthenticatedSession::Result result;
    result.error = QStringLiteral("Network access is disabled in this test.");
    return result;
}
