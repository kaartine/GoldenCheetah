/*
 * Copyright (c) 2026 Felix Gertz (felix@tredict.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "TredictMeasuresDownload.h"
#include "NetworkReplyWait.h"
#include "OAuthPKCE.h"
#include "Athlete.h"
#include "Settings.h"
#include "Secrets.h"
#include "Measures.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QPointer>
#include <QThread>
#include <QTimeZone>

#include <QUrlQuery>
#ifndef TREDICT_DEBUG
#define TREDICT_DEBUG false
#endif
#ifdef Q_CC_MSVC
#define printd(fmt, ...) do {                                                \
    if (TREDICT_DEBUG) {                                 \
        printf("[%s:%d %s] " fmt , __FILE__, __LINE__,        \
               __FUNCTION__, __VA_ARGS__);                    \
        fflush(stdout);                                       \
    }                                                         \
} while(0)
#else
#define printd(fmt, args...)                                            \
    do {                                                                \
        if (TREDICT_DEBUG) {                                       \
            printf("[%s:%d %s] " fmt , __FILE__, __LINE__,              \
                   __FUNCTION__, ##args);                               \
            fflush(stdout);                                             \
        }                                                               \
    } while(0)
#endif

TredictMeasuresDownload::TredictMeasuresDownload(Context *context)
    : TredictMeasuresDownload(
        context,
        {QUrl(QStringLiteral(
             "https://www.tredict.com/user/oauth/v2/token")),
         QUrl(QStringLiteral(
             "https://www.tredict.com/api/oauth/v2/bodyvalues")),
         QUrl(QStringLiteral(
             "https://www.tredict.com/api/oauth/v2/hrv")),
         30000})
{
}

TredictMeasuresDownload::TredictMeasuresDownload(
        Context *context, const NetworkOptions &networkOptions)
    : context(context), networkOptions(networkOptions)
{
    nam = new QNetworkAccessManager(this);
}

bool
TredictMeasuresDownload::refreshToken(QString &error)
{
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;
    if (guardedContext.isNull() || guardedAthlete.isNull()) {
        error = tr("Tredict request was cancelled.");
        return false;
    }

    QString rt = appsettings->cvalue(context->athlete->cyclist, GC_TREDICT_REFRESH_TOKEN, "").toString();
    if (rt.isEmpty()) {
        error = tr("No Tredict authorization token configured. Please authorize in Preferences.");
        return false;
    }

    QString newAccess, newRefresh, err;
    int expiresIn;
    if (!OAuthPKCE::refreshAccessTokenWithTimeout(
            networkOptions.tokenEndpoint.toString(),
            GC_TREDICT_CLIENT_ID,
            rt, newAccess, newRefresh, expiresIn, err,
            networkOptions.timeoutMs,
            [guardedContext, guardedAthlete]() {
                return guardedContext.isNull()
                    || guardedAthlete.isNull()
                    || guardedContext->athlete
                        != guardedAthlete.data();
            })) {
        error = err;
        return false;
    }

    if (guardedContext.isNull() || guardedAthlete.isNull()) {
        error = tr("Tredict request was cancelled.");
        return false;
    }

    if (!newAccess.isEmpty())
        appsettings->setCValue(context->athlete->cyclist, GC_TREDICT_TOKEN, newAccess);
    if (!newRefresh.isEmpty())
        appsettings->setCValue(context->athlete->cyclist, GC_TREDICT_REFRESH_TOKEN, newRefresh);
    appsettings->setCValue(context->athlete->cyclist, GC_TREDICT_LAST_REFRESH, QDateTime::currentDateTime());

    return true;
}

QByteArray
TredictMeasuresDownload::fetchEndpoint(const QString &urlString, QString &error)
{
    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;
    if (guardedContext.isNull() || guardedAthlete.isNull()) {
        error = tr("Tredict request was cancelled.");
        return {};
    }

    QString token = appsettings->cvalue(context->athlete->cyclist, GC_TREDICT_TOKEN, "").toString();
    if (token.isEmpty()) {
        error = tr("Not authorized with Tredict.");
        return QByteArray();
    }

    QUrl url(urlString);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", (QString("Bearer %1").arg(token)).toLatin1());
    request.setRawHeader("Accept", "application/json;charset=UTF-8");

    QNetworkReply *reply = nam->get(request);
    QThread *thread = QThread::currentThread();
    const NetworkReplyWaitResult waitResult =
            waitForNetworkReply(
                reply, networkOptions.timeoutMs,
                [guardedContext, guardedAthlete, thread]() {
                    return guardedContext.isNull()
                        || guardedAthlete.isNull()
                        || thread->isInterruptionRequested();
                });

    if (waitResult == NetworkReplyWaitResult::TimedOut) {
        error = tr("Tredict request timed out.");
        reply->deleteLater();
        return {};
    }
    if (waitResult == NetworkReplyWaitResult::Interrupted
        || guardedContext.isNull()
        || guardedAthlete.isNull()) {
        error = tr("Tredict request was cancelled.");
        reply->deleteLater();
        return {};
    }
    if (reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
        reply->deleteLater();
        return {};
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

bool
TredictMeasuresDownload::getBodyMeasures(QString &error, QDateTime from, QDateTime to,
                                         QList<Measure> &data)
{
    printd("TredictMeasuresDownload::getBodyMeasures\n");

    emit downloadStarted(100);

    if (!refreshToken(error)) return false;

    emit downloadProgress(30);

    QByteArray r = fetchEndpoint(
        networkOptions.bodyEndpoint.toString(), error);
    if (r.isEmpty()) return false;

    emit downloadProgress(70);

    printd("response: %s\n", r.toStdString().c_str());

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(r, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = tr("JSON parser error: %1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    QJsonArray bodyvalues = root["bodyvalues"].toArray();

    for (int i = 0; i < bodyvalues.size(); i++) {
        QJsonObject entry = bodyvalues.at(i).toObject();

        QDateTime when = QDateTime::fromString(entry["timestamp"].toString(), Qt::ISODate);
        if (!when.isValid()) continue;
        if (when < from || when > to) continue;

        double weight = entry.contains("weightInKilograms") ? entry["weightInKilograms"].toDouble() : 0;
        double fatPercent = entry.contains("bodyFatInPercent") ? entry["bodyFatInPercent"].toDouble() : 0;
        double musclePercent = entry.contains("muscleMassInPercent") ? entry["muscleMassInPercent"].toDouble() : 0;

        if (weight <= 0 && fatPercent <= 0 && musclePercent <= 0) continue;

        Measure m;
        m.when = when;
        m.source = Measure::Tredict;
        m.originalSource = "Tredict";

        m.values[Measure::WeightKg] = weight;
        m.values[Measure::FatPercent] = fatPercent;

        if (weight > 0 && fatPercent > 0)
            m.values[Measure::FatKg] = weight * fatPercent / 100.0;

        if (weight > 0 && musclePercent > 0)
            m.values[Measure::MuscleKg] = weight * musclePercent / 100.0;

        if (weight > 0 && m.values[Measure::FatKg] > 0)
            m.values[Measure::LeanKg] = weight - m.values[Measure::FatKg];

        data.append(m);
    }

    emit downloadEnded(100);
    return true;
}

bool
TredictMeasuresDownload::getHrvMeasures(QString &error, QDateTime from, QDateTime to,
                                        QList<Measure> &data)
{
    printd("TredictMeasuresDownload::getHrvMeasures\n");

    emit downloadStarted(100);

    if (!refreshToken(error)) return false;

    emit downloadProgress(30);

    // startDate = newest, endDate = oldest (Tredict API convention)
    QUrl url = networkOptions.hrvEndpoint;
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("startDate"), to.toUTC().toString(Qt::ISODate));
    query.addQueryItem(
        QStringLiteral("endDate"), from.toUTC().toString(Qt::ISODate));
    url.setQuery(query);

    QByteArray r = fetchEndpoint(url.toString(), error);
    if (r.isEmpty()) return false;

    emit downloadProgress(70);

    printd("response: %s\n", r.toStdString().c_str());

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(r, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = tr("JSON parser error: %1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    QJsonObject hrv = root["hrv"].toObject();

    // HRV object has YYYYMMDD keys with [RMSSD, Baseline] tuples
    for (auto it = hrv.begin(); it != hrv.end(); ++it) {
        QString dateKey = it.key();
        QDate date = QDate::fromString(dateKey, "yyyyMMdd");
        if (!date.isValid()) continue;

        QDateTime when(date, QTime(0, 0), QTimeZone::UTC);
        if (when < from || when > to) continue;

        QJsonArray tuple = it.value().toArray();
        if (tuple.isEmpty()) continue;

        double rmssd = tuple.at(0).toDouble();
        if (rmssd <= 0) continue;

        Measure m;
        m.when = when;
        m.source = Measure::Tredict;
        m.originalSource = "Tredict";
        m.values[0] = rmssd; // RMSSD
        data.append(m);
    }

    emit downloadEnded(100);
    return true;
}
