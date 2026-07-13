/*
 * Copyright (c) 2010 Mark Liversedge (liversedge@gmail.com)
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

#include "WithingsDownload.h"
#include "CloudCredentialTransport.h"
#include "NetworkReplyWait.h"
#include "WithingsReading.h"
#include "MainWindow.h"
#include "Athlete.h"
#include "RideCache.h"
#include "Secrets.h"
#include "Measures.h"
#include <QMessageBox>
#include <QPointer>
#include <QThread>

#ifndef WITHINGS_DEBUG
#define WITHINGS_DEBUG false
#endif
#ifdef Q_CC_MSVC
#define printd(fmt, ...) do {                                                \
    if (WITHINGS_DEBUG) {                                 \
        printf("[%s:%d %s] " fmt , __FILE__, __LINE__,        \
               __FUNCTION__, __VA_ARGS__);                    \
        fflush(stdout);                                       \
    }                                                         \
} while(0)
#else
#define printd(fmt, args...)                                            \
    do {                                                                \
        if (WITHINGS_DEBUG) {                                       \
            printf("[%s:%d %s] " fmt , __FILE__, __LINE__,              \
                   __FUNCTION__, ##args);                               \
            fflush(stdout);                                             \
        }                                                               \
    } while(0)
#endif

WithingsDownload::WithingsDownload(Context *context)
    : WithingsDownload(
        context,
        {QUrl(QStringLiteral("https://wbsapi.withings.net/v2/oauth2")),
         QUrl(QStringLiteral("https://wbsapi.withings.net/measure")),
         30000})
{
}

WithingsDownload::WithingsDownload(
        Context *context, const NetworkOptions &networkOptions)
    : context(context), networkOptions(networkOptions)
{
    nam = new QNetworkAccessManager(this);
}

bool
WithingsDownload::getBodyMeasures(QString &error, QDateTime from, QDateTime to, QList<Measure> &data)
{
    if (!context || !context->athlete) {
        error = tr("Withings request was cancelled.");
        return false;
    }

    QString response;

    QString strNokiaRefreshToken = "";
    QString access_token = "";

    strNokiaRefreshToken = appsettings->cvalue(context->athlete->cyclist, GC_NOKIA_REFRESH_TOKEN).toString();

    if(strNokiaRefreshToken.isEmpty() || strNokiaRefreshToken == "" || strNokiaRefreshToken == "0" ) {
        #ifdef Q_OS_MACOS
        #define GC_PREF tr("Golden Cheetah->Preferences")
        #else
        #define GC_PREF tr("Tools->Options")
        #endif
        QString advise = QString(tr("Error fetching OAuth credentials.  Please make sure to complete the Withings authorization procedure found under %1.")).arg(GC_PREF);
        QMessageBox oautherr(QMessageBox::Critical, tr("OAuth Error"), advise);
        oautherr.exec();
        return false;
    }

    if(!strNokiaRefreshToken.isEmpty()) {
        printd("OAuth 2.0 API\n");

        QUrlQuery postData;

        QString refresh_token = appsettings->cvalue(context->athlete->cyclist, GC_NOKIA_REFRESH_TOKEN).toString();

        postData.addQueryItem("action", "requesttoken");
        postData.addQueryItem("grant_type", "refresh_token");
        postData.addQueryItem("client_id", GC_NOKIA_CLIENT_ID );
        postData.addQueryItem("client_secret", GC_NOKIA_CLIENT_SECRET );
        postData.addQueryItem("refresh_token", refresh_token );

        QUrl url = networkOptions.tokenEndpoint;

        emit downloadStarted(100);

        QNetworkRequest request(url);
        request.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
        QNetworkReply *tokenReply = nam->post(
            request, postData.toString(QUrl::FullyEncoded).toUtf8());
        printd("Token request sent\n");

        if (!waitForReply(tokenReply, error, response))
            return false;

        printd("Token response received\n");


        if (response.contains("\"access_token\"", Qt::CaseInsensitive))
        {
                QJsonParseError parseResult;
                QJsonDocument migrateJson = QJsonDocument::fromJson(response.toUtf8(), &parseResult);

                access_token = migrateJson.object()["body"].toObject()["access_token"].toString();
                QString refresh_token = migrateJson.object()["body"].toObject()["refresh_token"].toString();
                QString userid = QString("%1").arg(migrateJson.object()["body"].toObject()["userid"].toInt());


                if (access_token != "") appsettings->setCValue(context->athlete->cyclist, GC_NOKIA_TOKEN, access_token);
                if (refresh_token != "") appsettings->setCValue(context->athlete->cyclist, GC_NOKIA_REFRESH_TOKEN, refresh_token);
                if (userid != "") appsettings->setCValue(context->athlete->cyclist, GC_WIUSER, userid);


                emit downloadStarted(100);

                const CloudCredentialTransport::Request transport =
                    CloudCredentialTransport::makeWithingsMeasuresRequest(
                        networkOptions.measuresEndpoint,
                        access_token,
                        from,
                        to);
                printd("Measures request sent\n");

                QNetworkReply *measuresReply = nam->post(
                    transport.request, transport.body);

                emit downloadProgress(50);

                if (!waitForReply(measuresReply, error, response))
                    return false;

                emit downloadEnded(100);

        }

    }
    printd("Measures response received\n");

    QJsonParseError parseResult;
    if (response.contains("\"status\":0", Qt::CaseInsensitive)) {
    		parseResult = parse(response, data);
    } else {
        QMessageBox oautherr(QMessageBox::Critical, tr("Error"),
                             tr("There was an error during fetching. Please check the error description."));
        oautherr.setDetailedText(response); // probably blank
        oautherr.exec();
        return false;

    }

    if (QJsonParseError::NoError != parseResult.error) {
        QMessageBox oautherr(QMessageBox::Critical, tr("Error"),
                             tr("Error parsing Withings API response. Please check the error description."));
        QString errorStr = parseResult.errorString() + " at offset " + QString::number(parseResult.offset);
        error = errorStr.simplified();
        oautherr.setDetailedText(error);
        oautherr.exec();
        return false;
    };
    return true;
}


QJsonParseError
WithingsDownload::parse(QString text, QList<Measure> &bodyMeasures)
{

    QJsonParseError parseResult;
	QJsonDocument withingsJson = QJsonDocument::fromJson(text.toUtf8(), &parseResult);
	QList<WithingsReading> readings = jsonDocumentToWithingsReading(withingsJson);

    if (parseResult.error != QJsonParseError::NoError) {
    		return parseResult;
    }

    // convert from Withings to general format
    foreach(WithingsReading r, readings) {
        Measure w;
        // we just take
        if (r.weightkg > 0 && r.when.isValid()) {
            w.when =  r.when;
            w.comment = r.comment;
            w.values[Measure::WeightKg] = r.weightkg;
            w.values[Measure::FatKg] = r.fatkg;
            w.values[Measure::LeanKg] = r.leankg;
            w.values[Measure::FatPercent] = r.fatpercent;
            w.values[Measure::MuscleKg] = r.musclekg;
            w.values[Measure::BonesKg] = r.boneskg;
            w.source = Measure::Withings;
            bodyMeasures.append(w);
        }
    }

    return parseResult;

}

QList<WithingsReading>
WithingsDownload::jsonDocumentToWithingsReading(QJsonDocument doc) {
    QList<WithingsReading> readings = QList<WithingsReading>();

    //Get the array of measurement groups
    QJsonArray jMeasureGrpsArr = doc.object()["body"].toObject()["measuregrps"].toArray();

    //Iterate the measurement groups
    for (int i = 0; i < jMeasureGrpsArr.size(); i++) {

        QJsonObject jThisMeasureGroup = jMeasureGrpsArr[i].toObject();
        QJsonArray jMeasuresArr = jThisMeasureGroup["measures"].toArray();

        //Get the context for this readings
        int grpid = jThisMeasureGroup["grpid"].toInt();
        int attrib = jThisMeasureGroup["attrib"].toInt();
        int category = jThisMeasureGroup["category"].toInt();
        int date = jThisMeasureGroup["date"].toInt();
        QString comment = jThisMeasureGroup["comment"].toString();

        WithingsReading thisReading = WithingsReading();
        thisReading.groupId = grpid;
        thisReading.attribution = attrib;
        thisReading.category = category;
        thisReading.when.setSecsSinceEpoch(date);
        thisReading.comment = comment;

        //Iterate the individual measurements in each group to create a WithingsReading object
        for (int j = 0; j < jMeasuresArr.size(); j++) {

            QJsonObject jMeasure = jMeasuresArr[j].toObject();
            int unscaledValue = jMeasure["value"].toInt();
            int type = jMeasure["type"].toInt();
            int unit = jMeasure["unit"].toInt();

            double value = (double)unscaledValue * pow(10.00, unit);
            switch (type) {
                case 1 : thisReading.weightkg = value; break;
                case 4 : thisReading.sizemeter = value; break;
                case 5 : thisReading.leankg = value; break;
                case 6 : thisReading.fatpercent = value; break;
                case 8 : thisReading.fatkg = value; break;
                case 76: thisReading.musclekg = value; break;
                case 88: thisReading.boneskg = value; break;
                default: break;
            }
        }
        readings << thisReading;
    }
    return readings;
}

bool WithingsDownload::waitForReply(
        QNetworkReply *reply,
        QString &error,
        QString &response)
{
    if (!reply) {
        error = tr("Withings request could not be started.");
        return false;
    }

    QPointer<Context> guardedContext(context);
    QPointer<Athlete> guardedAthlete =
            guardedContext ? guardedContext->athlete : nullptr;
    QThread *thread = QThread::currentThread();
    const NetworkReplyWaitResult waitResult =
            waitForNetworkReply(
                reply, networkOptions.timeoutMs,
                [guardedContext, guardedAthlete, thread]() {
                    return guardedContext.isNull()
                        || guardedAthlete.isNull()
                        || thread->isInterruptionRequested();
                });

    bool succeeded = false;
    if (waitResult == NetworkReplyWaitResult::TimedOut) {
        error = tr("Withings request timed out.");
    } else if (waitResult
               == NetworkReplyWaitResult::Interrupted
               || guardedContext.isNull()
               || guardedAthlete.isNull()) {
        error = tr("Withings request was cancelled.");
    } else if (reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
    } else {
        response = QString::fromUtf8(reply->readAll());
        succeeded = true;
    }
    reply->deleteLater();
    return succeeded;
}
