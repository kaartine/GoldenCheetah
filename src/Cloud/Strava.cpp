/*
 * Copyright (c) 2017 Mark Liversedge (liversedge@gmail.com)
 * Copyright (c) 2013 Damien.Grauser (damien.grauser@pev-geneve.ch)
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

#include "Strava.h"
#include "JsonRideFile.h"
#include "Athlete.h"
#include "Settings.h"
#include "Secrets.h"
#include "NetworkReplyWait.h"
#include "StravaApiReplyPolicy.h"
#include "StravaCredentialPublisher.h"
#include "StravaOAuthPolicy.h"
#include "StravaTokenRefresh.h"
#include "mvjson.h"
#include <QByteArray>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QThread>

#include <memory>

#ifndef STRAVA_DEBUG
#define STRAVA_DEBUG false
#endif
#ifdef Q_CC_MSVC
#define printd(fmt, ...) do {                                                \
    if (STRAVA_DEBUG) {                                 \
        printf("[%s:%d %s] " fmt , __FILE__, __LINE__,        \
               __FUNCTION__, __VA_ARGS__);                    \
        fflush(stdout);                                       \
    }                                                         \
} while(0)
#else
#define printd(fmt, args...)                                            \
    do {                                                                \
        if (STRAVA_DEBUG) {                                       \
            printf("[%s:%d %s] " fmt , __FILE__, __LINE__,              \
                   __FUNCTION__, ##args);                               \
            fflush(stdout);                                             \
        }                                                               \
    } while(0)
#endif

namespace {

constexpr auto OversizedReplyProperty =
        "_gcStravaReplyExceededLimit";

bool appendAvailableReplyData(
    QNetworkReply *reply,
    QByteArray *data)
{
    if (!reply || !data
        || reply->property(OversizedReplyProperty).toBool()) {
        return false;
    }

    const qsizetype remaining =
        StravaApiReplyPolicy::MaximumPayloadSize - data->size();
    if (remaining <= 0
        || reply->bytesAvailable() >= remaining) {
        reply->setProperty(OversizedReplyProperty, true);
        data->clear();
        return false;
    }

    const QByteArray chunk = reply->readAll();
    if (chunk.size() >= remaining) {
        reply->setProperty(OversizedReplyProperty, true);
        data->clear();
        return false;
    }
    data->append(chunk);
    return true;
}

QStringList stravaSensitiveValues(const Strava *service)
{
    return {
        service->getSetting(GC_STRAVA_TOKEN, QString()).toString(),
        service->getSetting(
            GC_STRAVA_REFRESH_TOKEN, QString()).toString(),
        QStringLiteral(GC_STRAVA_CLIENT_SECRET)
    };
}

} // namespace

Strava::Strava(Context *context) : CloudService(context), context(context), root_(NULL) {

    if (context) {
        nam = new QNetworkAccessManager(this);
        connect(nam, SIGNAL(sslErrors(QNetworkReply*, const QList<QSslError> & )), this, SLOT(onSslErrors(QNetworkReply*, const QList<QSslError> & )));
        garminSmartRecording = appsettings->value(
            nullptr, GC_GARMIN_SMARTRECORD, Qt::Checked).toInt() != 0;
        garminHighWaterMark = appsettings->value(
            nullptr, GC_GARMIN_HWMARK, 30).toInt();
    }

    uploadCompression = gzip; // gzip
    downloadCompression = none;
    filetype = uploadType::TCX;
    useMetric = true; // distance and duration metadata

    // config
    settings.insert(OAuthToken, GC_STRAVA_TOKEN);
    settings.insert(Local1, GC_STRAVA_REFRESH_TOKEN);
    settings.insert(Local2, GC_STRAVA_LAST_REFRESH);
    settings.insert(Metadata1, QString("%1::Activity Name").arg(GC_STRAVA_ACTIVITY_NAME));
}

Strava::~Strava() {
    if (context) delete nam;
}

QImage Strava::logo() const
{
    return QImage(":images/services/strava.png");
}

void
Strava::onSslErrors(QNetworkReply *reply, const QList<QSslError>&errors)
{
    sslErrors(context->mainWindow, reply, errors);
}

bool
Strava::open(QStringList &errors)
{
    printd("Strava::open\n");
    const QString configuredRefreshToken =
        getSetting(GC_STRAVA_REFRESH_TOKEN, "").toString();
    if (configuredRefreshToken.isEmpty()) {
        errors << tr("No authorisation token configured.");
        return false;
    }

    const QString accountKey =
        property("_gcAthleteName").toString();
    if (accountKey.trimmed().isEmpty()) {
        errors << tr("The Strava account identity is unavailable.");
        return false;
    }

    printd("Get access token for this session.\n");
    const auto interrupted = [] {
        return QThread::currentThread()
            ->isInterruptionRequested();
    };
    const StravaTokenRefreshResult refreshResult =
        StravaTokenRefreshCoordinator::refresh(
            accountKey,
            configuredRefreshToken,
            [this, interrupted](
                    const QString &effectiveRefreshToken) {
                StravaTokenRefreshResult result;
                const StravaOAuthPolicy::TokenRequest tokenRequest =
                    StravaOAuthPolicy::refreshTokenRequest(
                        QStringLiteral(GC_STRAVA_CLIENT_ID),
                        QStringLiteral(GC_STRAVA_CLIENT_SECRET),
                        effectiveRefreshToken);
                if (!tokenRequest.isValid()) {
                    result.error = tokenRequest.error;
                    return result;
                }

                QNetworkRequest request(tokenRequest.endpoint);
                request.setHeader(
                    QNetworkRequest::ContentTypeHeader,
                    QStringLiteral(
                        "application/x-www-form-urlencoded"));
                QNetworkReply *tokenReply =
                    nam->post(request, tokenRequest.body);
                if (!tokenReply) {
                    result.error = tr(
                        "The Strava token request could not be started.");
                    return result;
                }

                const NetworkReplyWaitResult waitResult =
                    waitForNetworkReply(
                        tokenReply, 30000, interrupted);
                if (waitResult
                    == NetworkReplyWaitResult::Interrupted) {
                    result.error =
                        tr("Strava token refresh was cancelled.");
                    tokenReply->deleteLater();
                    return result;
                }
                if (waitResult
                    == NetworkReplyWaitResult::TimedOut) {
                    result.error =
                        tr("Strava token refresh timed out.");
                    tokenReply->deleteLater();
                    return result;
                }

                const int statusCode = tokenReply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                printd("HTTP response code: %d\n", statusCode);
                const QByteArray payload = tokenReply->readAll();
                if (tokenReply->error()
                    != QNetworkReply::NoError) {
                    printd(
                        "Got error %s\n",
                        tokenReply->errorString()
                            .toStdString().c_str());
                    result.error =
                        StravaOAuthPolicy::tokenFailureMessage(
                            statusCode,
                            tokenReply->error(),
                            tokenReply->errorString(),
                            payload,
                            {
                                QStringLiteral(
                                    GC_STRAVA_CLIENT_SECRET),
                                effectiveRefreshToken
                            });
                    tokenReply->deleteLater();
                    return result;
                }

                const StravaOAuthPolicy::TokenResponse response =
                    StravaOAuthPolicy::parseTokenResponse(payload);
                tokenReply->deleteLater();
                if (!response.isValid()) {
                    result.error = response.error;
                    return result;
                }
                result.success = true;
                result.accessToken = response.accessToken;
                result.refreshToken = response.refreshToken;
                return result;
            },
            interrupted);
    if (!refreshResult.isValid()) {
        errors << (refreshResult.error.isEmpty()
            ? tr("Strava token refresh failed.")
            : refreshResult.error);
        return false;
    }

    const QString refreshedAt =
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    StravaCredentialPublisher::Request publication;
    publication.accountKey = accountKey;
    publication.expectedRefreshToken =
        refreshResult.sourceRefreshToken;
    publication.replacement = {
        refreshResult.accessToken,
        refreshResult.refreshToken
    };
    publication.refreshedAt = refreshedAt;
    publication.mode =
        StravaTokenPublication::PublicationMode::CompareAndSwap;
    const StravaTokenPublication::PublicationResult published =
        StravaCredentialPublisher::publish(
            publication, 30000, interrupted);
    if (!published.isSuccess()) {
        errors << (published.error.isEmpty()
            ? tr("Strava credentials could not be stored securely.")
            : published.error);
        return false;
    }

    setSetting(GC_STRAVA_TOKEN, refreshResult.accessToken);
    setSetting(
        GC_STRAVA_REFRESH_TOKEN, refreshResult.refreshToken);
    setSetting(GC_STRAVA_LAST_REFRESH, refreshedAt);
    return true;
}

bool
Strava::close()
{
    printd("Strava::close\n");
    // nothing to do for now
    return true;
}

QList<CloudServiceEntry*>
Strava::readdir(QString path, QStringList &errors, QDateTime from, QDateTime to)
{
    printd("Strava::readdir(%s)\n", path.toStdString().c_str());

    QList<CloudServiceEntry*> returning;

    // do we have a token
    QString token = getSetting(GC_STRAVA_TOKEN, "").toString();
    if (token == "") {
        errors << tr("You must authorise with Strava first");
        return returning;
    }

    // Do Paginated Access to the Activities List
    const int pageSize = 30;
    int offset = 0;
    int resultCount = INT_MAX;

    while (offset < resultCount) {
        QString urlstr = "https://www.strava.com/api/v3/athlete/activities?";

        QUrlQuery params;

        // use toMSecsSinceEpoch for compatibility with QT4
        params.addQueryItem("before", QString::number(to.addDays(1).toMSecsSinceEpoch()/1000.0f, 'f', 0));
        params.addQueryItem("after", QString::number(from.addDays(-1).toMSecsSinceEpoch()/1000.0f, 'f', 0));
        params.addQueryItem("per_page", QString("%1").arg(pageSize));
        params.addQueryItem("page",  QString("%1").arg(offset/pageSize+1));

        QUrl url = QUrl( urlstr + params.toString() );
        printd("URL used: %s\n", url.url().toStdString().c_str());

        // request using the bearer token
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", (QString("Bearer %1").arg(token)).toLatin1());

        QNetworkReply *reply = nam->get(request);

        // blocking request
        QEventLoop loop;
        connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "error" << reply->errorString();
            errors << tr("Network Problem reading Strava data");
            //return returning;
        }
        // did we get a good response ?
        QByteArray r = reply->readAll();
        printd("response: %s\n", r.toStdString().c_str());

        QJsonParseError parseError;
        QJsonDocument document = QJsonDocument::fromJson(r, &parseError);

        // if path was returned all is good, lets set root
        if (parseError.error == QJsonParseError::NoError) {

            // results ?
            QJsonArray results = document.array();

            // lets look at that then
            if (results.size()>0) {
                for(int i=0; i<results.size(); i++) {
                    QJsonObject each = results.at(i).toObject();
                    CloudServiceEntry *add = newCloudServiceEntry();


                    //Strava has full path, we just want the file name
                    add->label = QFileInfo(each["name"].toString()).fileName();
                    add->id = QString("%1").arg(each["id"].toVariant().toULongLong());

                    add->isDir = false;
                    add->distance = each["distance"].toDouble()/1000.0;
                    add->duration = each["elapsed_time"].toInt();
                    add->name = QDateTime::fromString(each["start_date_local"].toString(), Qt::ISODate).toString("yyyy_MM_dd_HH_mm_ss")+".json";

                    printd("direntry: %s %s\n", add->id.toStdString().c_str(), add->name.toStdString().c_str());

                    returning << add;
                }
                // next page
                offset += pageSize;
            } else
                offset = INT_MAX;

        } else {
            // we had a parsing error - so something is wrong - stop requesting more data by ending the loop
            offset = INT_MAX;
        }
    }

    // all good ?
    return returning;
}

// read a file at location (relative to home) into passed array
bool
Strava::readFile(QByteArray *data, QString remotename, QString remoteid)
{
    printd("Strava::readFile(%s)\n", remotename.toStdString().c_str());

    if (!data) {
        return false;
    }

    // do we have a token ?
    QString token = getSetting(GC_STRAVA_TOKEN, "").toString();
    if (token == "") return false;

    // lets connect and get basic info on the root directory
    QString url = QString("https://www.strava.com/api/v3/activities/%1")
          .arg(remoteid);

    printd("url:%s\n", url.toStdString().c_str());

    // request using the bearer token
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", (QString("Bearer %1").arg(token)).toLatin1());
    request.setRawHeader("Accept", "application/json");

    // put the file
    QNetworkReply *reply = nam->get(request);

    // remember
    mapReply(reply,remotename);
    buffers.insert(reply,data);

    // catch finished signal
    connect(
        reply, &QNetworkReply::finished,
        this, &Strava::readFileCompleted);
    connect(
        reply, &QIODevice::readyRead,
        this, &Strava::readyRead);
    return true;
}

bool
Strava::writeFile(QByteArray &data, QString remotename, RideFile *ride)
{
    // Manual activity upload or File upload according to available data
    bool manual = ride->dataPoints().isEmpty();

    printd("Strava::writeFile(%s) manual(%s)\n", remotename.toStdString().c_str(), manual ? "true" : "false");

    QString token = getSetting(GC_STRAVA_TOKEN, "").toString();

    // The V3 API doc said "https://api.strava.com" but it is not working yet
    QUrl url = manual ?  QUrl( "https://www.strava.com/api/v3/activities" )
                      :  QUrl( "https://www.strava.com/api/v3/uploads" );
    QNetworkRequest request = QNetworkRequest(url);

    //QString boundary = QString::number(QRandomGenerator::global()->generate() * (90000000000) / (RAND_MAX + 1) + 10000000000, 16);
    QString boundary = QVariant(QRandomGenerator::global()->generate()).toString() +
        QVariant(QRandomGenerator::global()->generate()).toString() + QVariant(QRandomGenerator::global()->generate()).toString();

    // MULTIPART *****************

    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    multiPart->setBoundary(boundary.toLatin1());

    QHttpPart accessTokenPart;
    accessTokenPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                              QVariant("form-data; name=\"access_token\""));
    accessTokenPart.setBody(token.toLatin1());
    multiPart->append(accessTokenPart);

    QHttpPart activityTypePart;
    activityTypePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               manual ? QVariant("form-data; name=\"type\"")
                                      : QVariant("form-data; name=\"activity_type\""));

    // Map some known sports and default to ride for anything else
    if (ride->isRun())
      activityTypePart.setBody("Run");
    else if (ride->isSwim())
      activityTypePart.setBody("Swim");
    else if (ride->sport() == "Row")
      activityTypePart.setBody("Rowing");
    else if (ride->sport() == "Ski")
      activityTypePart.setBody("NordicSki");
    else if (ride->sport() == "Gym")
      activityTypePart.setBody("WeightTraining");
    else if (ride->sport() == "Walk")
      activityTypePart.setBody("Walk");
    else if (ride->xdata("TRAIN") && ride->isDataPresent(RideFile::lat))
      activityTypePart.setBody("VirtualRide");
    else
      activityTypePart.setBody("Ride");
    multiPart->append(activityTypePart);

    QHttpPart activityNamePart;
    activityNamePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"name\""));

    // use metadata config if the user selected it
    QString activityNameFieldname = getSetting(GC_STRAVA_ACTIVITY_NAME, QVariant("")).toString();
    QString activityName = "";
    if (activityNameFieldname != "")
        activityName = ride->getTag(activityNameFieldname, "");
    activityNamePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain;charset=utf-8"));
    activityNamePart.setBody(activityName.toUtf8());
    if (activityName != "")
        multiPart->append(activityNamePart);

    QHttpPart activityDescriptionPart;
    activityDescriptionPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"description\""));
    QString activityDescription = "";
    if (activityNameFieldname != "Notes")
        activityDescription = ride->getTag("Notes", "");
    activityDescriptionPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain;charset=utf-8"));
    activityDescriptionPart.setBody(activityDescription.toUtf8());
    if (activityDescription != "")
        multiPart->append(activityDescriptionPart);

    //XXXQHttpPart privatePart;
    //XXXprivatePart.setHeader(QNetworkRequest::ContentDispositionHeader,
    //XXX                      QVariant("form-data; name=\"private\""));
    //XXXprivatePart.setBody(parent->privateChk->isChecked() ? "1" : "0");
    //XXXmultiPart->append(privatePart);

    QHttpPart commutePart;
    commutePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                          QVariant("form-data; name=\"commute\""));
    commutePart.setBody(ride->getTag("Commute", "0").toInt() ? "1" : "0");
    multiPart->append(commutePart);

    QHttpPart trainerPart;
    trainerPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                          QVariant("form-data; name=\"trainer\""));
    trainerPart.setBody((ride->getTag("Trainer", "0").toInt() ||
                         (ride->xdata("TRAIN") && !ride->isDataPresent(RideFile::lat))) ? "1" : "0");
    multiPart->append(trainerPart);

    if (manual) {

        // create manual activity data
        QDateTime rideDateTime = ride->startTime();
        QHttpPart startDateTimePart;
        startDateTimePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"start_date_local\""));
        startDateTimePart.setBody(rideDateTime.toString(Qt::ISODate).toStdString().c_str());
        multiPart->append(startDateTimePart);

        if (ride->metricOverrides.contains("workout_time")) {
            QString duration = ride->metricOverrides.value("workout_time").value("value");
            QHttpPart elapsedTimePart;
            elapsedTimePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"elapsed_time\""));
            elapsedTimePart.setBody(duration.toStdString().c_str());
            multiPart->append(elapsedTimePart);
        }

        if (ride->metricOverrides.contains("total_distance")) {
            QString distance = QString::number(ride->metricOverrides.value("total_distance").value("value").toDouble() * 1000.0);
            QHttpPart distancePart;
            distancePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"distance\""));
            distancePart.setBody(distance.toStdString().c_str());
            multiPart->append(distancePart);
        }
    } else {

        // upload file data
        QString filename = QFileInfo(remotename).baseName();

        QHttpPart dataTypePart;
        dataTypePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"data_type\""));
        dataTypePart.setBody("tcx.gz");
        multiPart->append(dataTypePart);

        QHttpPart externalIdPart;
        externalIdPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"external_id\""));
        externalIdPart.setBody(filename.toStdString().c_str());
        multiPart->append(externalIdPart);

        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/xml"));
        filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"file\"; filename=\""+remotename+"\"; type=\"text/xml\""));
        filePart.setBody(data);
        multiPart->append(filePart);
    }

    // this must be performed asyncronously and call made
    // to notifyWriteCompleted(QString remotename, QString message) when done
    reply = nam->post(request, multiPart);

    // catch finished signal
    connect(reply, SIGNAL(finished()), this, SLOT(writeFileCompleted()));

    // remember
    mapReply(reply,remotename);
    return true;
}

void
Strava::writeFileCompleted()
{
    printd("Strava::writeFileCompleted()\n");

    QNetworkReply *reply = static_cast<QNetworkReply*>(QObject::sender());

    bool uploadSuccessful = false;
    QString response = reply->readLine();
    QString uploadError="invalid response or parser error";

    printd("reply:%s\n", response.toStdString().c_str());

    try {

        // parse !
        MVJSONReader jsonResponse(response.toStdString());

        // get error field
        if (jsonResponse.root) {
            if (jsonResponse.root->hasField("error")) {
                uploadError = jsonResponse.root->getFieldString("error").c_str();
            } else {
                uploadError = ""; // no error
            }

            // get upload_id, but if not available use id
            //XXX if (jsonResponse.root->hasField("upload_id")) {
            //XXX     stravaUploadId = jsonResponse.root->getFieldInt("upload_id");
            //XXX } else if (jsonResponse.root->hasField("id")) {
            //XXX     stravaUploadId = jsonResponse.root->getFieldInt("id");
            //XXX } else {
            //XXX     stravaUploadId = 0;
            //XXX }
        } else {
            uploadError = "no connection";
        }

    } catch(...) { // not really sure what exceptions to expect so do them all (bad, sorry)
        uploadError=tr("invalid response or parser exception.");
    }

    if (uploadError.toLower() == "none" || uploadError.toLower() == "null") uploadError = "";

    // if successful update ID
    if (uploadError.length()>0 || reply->error() != QNetworkReply::NoError)  uploadSuccessful = false;
    else {

        //XXXride->ride()->setTag("Strava uploadId", QString("%1").arg(stravaUploadId));
        //XXXride->setDirty(true);

        //qDebug() << "uploadId: " << stravaUploadId;
        uploadSuccessful = true;
    }

    // return response
    if (uploadSuccessful && reply->error() == QNetworkReply::NoError) {
        notifyWriteComplete(replyName(static_cast<QNetworkReply*>(QObject::sender())), tr("Completed."));
    } else {
        notifyWriteComplete(replyName(static_cast<QNetworkReply*>(QObject::sender())), uploadError);
    }
}

void
Strava::readyRead()
{
    QNetworkReply *reply =
        qobject_cast<QNetworkReply *>(QObject::sender());
    QByteArray *data = buffers.value(reply, nullptr);
    if (!reply || !data) {
        return;
    }
    if (!appendAvailableReplyData(reply, data)
        && reply->isRunning()) {
        reply->abort();
    }
}

void
Strava::readFileCompleted()
{
    printd("Strava::readFileCompleted\n");

    QNetworkReply *reply =
        qobject_cast<QNetworkReply *>(QObject::sender());
    if (!reply) {
        return;
    }

    QByteArray *data = buffers.take(reply);
    const QString name = replymap_.take(reply);
    if (!data) {
        reply->deleteLater();
        return;
    }

    bool oversized =
        reply->property(OversizedReplyProperty).toBool();
    if (!oversized && !appendAvailableReplyData(reply, data)) {
        oversized =
            reply->property(OversizedReplyProperty).toBool();
    }

    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError =
        reply->error();
    const QString networkErrorString = reply->errorString();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString();
    reply->deleteLater();

    if (oversized) {
        data->clear();
        notifyReadComplete(
            data,
            name,
            tr("Strava response exceeds the download size limit."),
            false);
        return;
    }

    const StravaApiReplyPolicy::Result validation =
        StravaApiReplyPolicy::validate(
            StravaApiReplyPolicy::PayloadKind::Activity,
            httpStatus,
            networkError,
            networkErrorString,
            *data,
            stravaSensitiveValues(this),
            contentType);
    if (!validation.isValid()) {
        data->clear();
        notifyReadComplete(data, name, validation.error, false);
        return;
    }

    QString conversionError;
    if (!prepareResponse(data, conversionError)) {
        data->clear();
        notifyReadComplete(
            data, name, conversionError, false);
        return;
    }

    printd("reply:%s\n", data->toStdString().c_str());
    notifyReadComplete(data, name, tr("Completed."), true);
}

bool
Strava::addSamples(
        RideFile *ret,
        const QString &remoteid,
        QString &error)
{
    printd("Strava::addSamples(%s)\n", remoteid.toStdString().c_str());

    const QString token =
        getSetting(GC_STRAVA_TOKEN, "").toString();
    if (token.isEmpty()) {
        error = tr("No Strava access token is available.");
        return false;
    }

    const QString streamsList =
        QStringLiteral(
            "time,latlng,distance,altitude,velocity_smooth,"
            "heartrate,cadence,watts,temp");
    const QString urlstr = QStringLiteral(
        "https://www.strava.com/api/v3/activities/%1/streams/%2")
            .arg(remoteid, streamsList);

    const QUrl url(urlstr);
    printd("url:%s\n", url.url().toStdString().c_str());

    QNetworkRequest request(url);
    request.setRawHeader(
        "Authorization",
        QStringLiteral("Bearer %1").arg(token).toLatin1());
    request.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = nam->get(request);
    QByteArray payload;
    QEventLoop loop;
    connect(
        reply, &QIODevice::readyRead,
        &loop, [&payload, reply]() {
            if (!appendAvailableReplyData(reply, &payload)
                && reply->isRunning()) {
                reply->abort();
            }
        });
    connect(
        reply, &QNetworkReply::finished,
        &loop, &QEventLoop::quit);
    loop.exec();

    bool oversized =
        reply->property(OversizedReplyProperty).toBool();
    if (!oversized
        && !appendAvailableReplyData(reply, &payload)) {
        oversized =
            reply->property(OversizedReplyProperty).toBool();
    }

    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError =
        reply->error();
    const QString networkErrorString = reply->errorString();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString();
    reply->deleteLater();

    if (oversized) {
        error = tr(
            "Strava streams response exceeds the download size limit.");
        return false;
    }

    const StravaApiReplyPolicy::Result validation =
        StravaApiReplyPolicy::validate(
            StravaApiReplyPolicy::PayloadKind::Streams,
            httpStatus,
            networkError,
            networkErrorString,
            payload,
            stravaSensitiveValues(this),
            contentType);
    if (!validation.isValid()) {
        error = validation.error;
        return false;
    }

    printd("response: %s\n", payload.toStdString().c_str());
    const QJsonArray streams =
        QJsonDocument::fromJson(payload).array();

    static const struct {
        RideFile::seriestype type;
        const char *stravaName;
        double factor;
    } seriesNames[] = {
        {RideFile::secs, "time", 1.0},
        {RideFile::lat, "latlng", 1.0},
        {RideFile::alt, "altitude", 1.0},
        {RideFile::km, "distance", 0.001},
        {RideFile::kph, "velocity_smooth", 3.6},
        {RideFile::hr, "heartrate", 1.0},
        {RideFile::cad, "cadence", 1.0},
        {RideFile::watts, "watts", 1.0},
        {RideFile::temp, "temp", 1.0},
        {RideFile::none, "", 0.0}
    };

    struct StravaStream {
        double factor = 0.0;
        RideFile::seriestype type = RideFile::none;
        QJsonArray samples;
    };
    QList<StravaStream> data;

    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        const QString type =
            stream.value(QStringLiteral("type")).toString();
        for (int index = 0;
             seriesNames[index].type != RideFile::none;
             ++index) {
            if (type == QLatin1String(seriesNames[index].stravaName)) {
                data.append({
                    seriesNames[index].factor,
                    seriesNames[index].type,
                    stream.value(QStringLiteral("data")).toArray()
                });
                break;
            }
        }
    }

    if (data.isEmpty()) {
        return true;
    }

    for (qsizetype index = 0;
         index < validation.sampleCount;
         ++index) {
        RideFilePoint point;
        for (const StravaStream &stream : data) {
            if (stream.type == RideFile::lat) {
                const QJsonArray coordinates =
                    stream.samples.at(index).toArray();
                point.setValue(
                    RideFile::lat,
                    stream.factor * coordinates.at(0).toDouble());
                point.setValue(
                    RideFile::lon,
                    stream.factor * coordinates.at(1).toDouble());
            } else {
                point.setValue(
                    stream.type,
                    stream.factor
                        * stream.samples.at(index).toDouble());
            }
        }
        ret->appendPoint(point);
    }
    return true;
}

void
Strava:: fixLapSwim(RideFile* ret, QJsonArray laps)
{
    // Lap Swim & SmartRecording enabled: use distance from laps to fix samples
    if (ret->isSwim() && ret->isDataPresent(RideFile::km) && !ret->isDataPresent(RideFile::lat)) {

        int lastSecs = 0;
        double lastDist = 0.0;
        for (int i=0; i<laps.count() && i<ret->intervals().count(); i++) {
            QJsonObject lap = laps[i].toObject();
            double start = ret->intervals()[i]->start;
            double end = ret->intervals()[i]->stop;
            int startIndex = ret->timeIndex(start) ? ret->timeIndex(start)-1 : 0;
            double km = ret->getPointValue(startIndex, RideFile::km);

            // fill gaps and fix distance before the lap
            double deltaDist = (start>lastSecs && km>lastDist+0.001) ? (km-lastDist)/(start-lastSecs) : 0;
            double kph = 3600.0*deltaDist;
            for (int secs=lastSecs; secs<start; secs++) {
                ret->appendOrUpdatePoint(secs, 0.0, 0.0, lastDist+deltaDist,
                        kph, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        RideFile::NA, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0, false);
                // force update, even when secs or kph are 0
                if (secs == 0 || kph == 0.0)
                    ret->setPointValue(secs, RideFile::kph, kph);
                lastDist += deltaDist;
            }

            // fill gaps and fix distance inside the lap
            deltaDist = 0.001*lap["distance"].toDouble()/(end-start);
            kph = 3600.0*deltaDist;
            for (int secs=start; secs<end; secs++) {
                ret->appendOrUpdatePoint(secs, 0.0, 0.0, lastDist+deltaDist,
                        kph, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        RideFile::NA, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0.0, 0.0,
                        0.0, 0.0, 0.0, 0.0,
                        0, false);
                // force update, even when secs or kph are 0
                if (secs == 0 || kph == 0.0)
                    ret->setPointValue(secs, RideFile::kph, kph);
                lastDist += deltaDist;
            }
            lastSecs = end;
        }
    }
}

void
Strava:: fixSmartRecording(RideFile* ret)
{
    if (!garminSmartRecording) return;

    int highWaterMark = garminHighWaterMark;
    // default to 30 seconds
    if (highWaterMark == 0) highWaterMark = 30;
    // minimum 90 seconds for swims
    if (ret->isSwim() && highWaterMark < 90) highWaterMark = 90;

    // The following fragment was adapted from FixGaps

    // If there are less than 2 dataPoints then there
    // is no way of post processing anyway (e.g. manual workouts)
    if (ret->dataPoints().count() < 2) return;

    // OK, so there are probably some gaps, lets post process them
    RideFilePoint *last = NULL;

    for (int position = 0; position < ret->dataPoints().count(); position++) {
        RideFilePoint *point = ret->dataPoints()[position];

        if (NULL != last) {
            double gap = point->secs - last->secs - ret->recIntSecs();

            // if we have gps and we moved, then this isn't a stop
            bool stationary = ((last->lat || last->lon) && (point->lat || point->lon)) // gps is present
                         && last->lat == point->lat && last->lon == point->lon;

            // moved for less than GarminHWM seconds ... interpolate
            if (!stationary && gap >= 1 && gap <= highWaterMark) {
                int count = gap/ret->recIntSecs();
                double hrdelta = (point->hr - last->hr) / (double) count;
                double pwrdelta = (point->watts - last->watts) / (double) count;
                double kphdelta = (point->kph - last->kph) / (double) count;
                double kmdelta = (point->km - last->km) / (double) count;
                double caddelta = (point->cad - last->cad) / (double) count;
                double altdelta = (point->alt - last->alt) / (double) count;
                double nmdelta = (point->nm - last->nm) / (double) count;
                double londelta = (point->lon - last->lon) / (double) count;
                double latdelta = (point->lat - last->lat) / (double) count;
                double hwdelta = (point->headwind - last->headwind) / (double) count;
                double slopedelta = (point->slope - last->slope) / (double) count;
                double temperaturedelta = (point->temp - last->temp) / (double) count;
                double lrbalancedelta = (point->lrbalance - last->lrbalance) / (double) count;
                double ltedelta = (point->lte - last->lte) / (double) count;
                double rtedelta = (point->rte - last->rte) / (double) count;
                double lpsdelta = (point->lps - last->lps) / (double) count;
                double rpsdelta = (point->rps - last->rps) / (double) count;
                double lpcodelta = (point->lpco - last->lpco) / (double) count;
                double rpcodelta = (point->rpco - last->rpco) / (double) count;
                double lppbdelta = (point->lppb - last->lppb) / (double) count;
                double rppbdelta = (point->rppb - last->rppb) / (double) count;
                double lppedelta = (point->lppe - last->lppe) / (double) count;
                double rppedelta = (point->rppe - last->rppe) / (double) count;
                double lpppbdelta = (point->lpppb - last->lpppb) / (double) count;
                double rpppbdelta = (point->rpppb - last->rpppb) / (double) count;
                double lpppedelta = (point->lpppe - last->lpppe) / (double) count;
                double rpppedelta = (point->rpppe - last->rpppe) / (double) count;
                double smo2delta = (point->smo2 - last->smo2) / (double) count;
                double thbdelta = (point->thb - last->thb) / (double) count;
                double rcontactdelta = (point->rcontact - last->rcontact) / (double) count;
                double rcaddelta = (point->rcad - last->rcad) / (double) count;
                double rvertdelta = (point->rvert - last->rvert) / (double) count;
                double tcoredelta = (point->tcore - last->tcore) / (double) count;

                // add the points
                for(int i=0; i<count; i++) {
                    RideFilePoint *add = new RideFilePoint(last->secs+((i+1)*ret->recIntSecs()),
                                                           last->cad+((i+1)*caddelta),
                                                           last->hr + ((i+1)*hrdelta),
                                                           last->km + ((i+1)*kmdelta),
                                                           last->kph + ((i+1)*kphdelta),
                                                           last->nm + ((i+1)*nmdelta),
                                                           last->watts + ((i+1)*pwrdelta),
                                                           last->alt + ((i+1)*altdelta),
                                                           last->lon + ((i+1)*londelta),
                                                           last->lat + ((i+1)*latdelta),
                                                           last->headwind + ((i+1)*hwdelta),
                                                           last->slope + ((i+1)*slopedelta),
                                                           last->temp + ((i+1)*temperaturedelta),
                                                           last->lrbalance + ((i+1)*lrbalancedelta),
                                                           last->lte + ((i+1)*ltedelta),
                                                           last->rte + ((i+1)*rtedelta),
                                                           last->lps + ((i+1)*lpsdelta),
                                                           last->rps + ((i+1)*rpsdelta),
                                                           last->lpco + ((i+1)*lpcodelta),
                                                           last->rpco + ((i+1)*rpcodelta),
                                                           last->lppb + ((i+1)*lppbdelta),
                                                           last->rppb + ((i+1)*rppbdelta),
                                                           last->lppe + ((i+1)*lppedelta),
                                                           last->rppe + ((i+1)*rppedelta),
                                                           last->lpppb + ((i+1)*lpppbdelta),
                                                           last->rpppb + ((i+1)*rpppbdelta),
                                                           last->lpppe + ((i+1)*lpppedelta),
                                                           last->rpppe + ((i+1)*rpppedelta),
                                                           last->smo2 + ((i+1)*smo2delta),
                                                           last->thb + ((i+1)*thbdelta),
                                                           last->rvert + ((i+1)*rvertdelta),
                                                           last->rcad + ((i+1)*rcaddelta),
                                                           last->rcontact + ((i+1)*rcontactdelta),
                                                           last->tcore + ((i+1)*tcoredelta),
                                                           last->interval);

                    ret->insertPoint(position++, add);
                }

            }
        }
        last = point;
    }
}

bool
Strava::prepareResponse(
        QByteArray *data,
        QString &error)
{
    printd("Strava::prepareResponse()\n");

    if (!data) {
        error = tr("Strava returned no activity data.");
        return false;
    }

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(data->constData(), &parseError);

    // if path was returned all is good, lets set root
    if (parseError.error == QJsonParseError::NoError) {
        QJsonObject each = document.object();

        QDateTime starttime = QDateTime::fromString(each["start_date_local"].toString(), Qt::ISODate);

        // 1s samples with start time
        std::unique_ptr<RideFile> ride(
            new RideFile(starttime.toUTC(), 1.0f));

        // set strava id in metadata (to show where we got it from - to add View on Strava link in Summary view
        if (!each["id"].isNull()) ride->setTag("StravaID",  QString("%1").arg(each["id"].toVariant().toULongLong()));

        // what sport?
        if (!each["type"].isNull()) {
            QString stype = each["type"].toString();
            if (stype.endsWith("Ride")) ride->setTag("Sport", "Bike");
            else if (stype.endsWith("Run")) ride->setTag("Sport", "Run");
            else if (stype.endsWith("Swim")) ride->setTag("Sport", "Swim");
            else if (stype.endsWith("Rowing")) ride->setTag("Sport", "Row");
            else if (stype.endsWith("Ski")) ride->setTag("Sport", "Ski");
            else if (stype.startsWith("Weight")) ride->setTag("Sport", "Gym");
            else if (stype.endsWith("Walk")) ride->setTag("Sport", "Walking");
            else ride->setTag("Sport", stype);
            // Set SubSport to preserve the original when Sport was mapped
            if (stype != ride->getTag("Sport", "")) ride->setTag("SubSport", stype);
        }

        if (each["device_name"].toString().length()>0)
            ride->setDeviceType(each["device_name"].toString());
        else
            ride->setDeviceType("Strava"); // The device type is unknown

        // activity name save to configured meta field
        if (!each["name"].isNull()) {
            QString meta = getSetting(GC_STRAVA_ACTIVITY_NAME, QVariant("")).toString();
            if (meta != "") ride->setTag(meta, each["name"].toString());
        }

        // description saved to Notes, if Notes is not already used for name
        if (!each["description"].isNull()) {
            QString meta = getSetting(GC_STRAVA_ACTIVITY_NAME, QVariant("")).toString();
            if (meta != "Notes") ride->setTag("Notes", each["description"].toString());
        }

        if (!each["commute"].isNull()) {
            ride->setTag("Commute", each["commute"].toBool() ? "1" : "0");
        }

        if (!each["trainer"].isNull()) {
            ride->setTag("Trainer", each["trainer"].toBool() ? "1" : "0");
        }

        if (each["gear"].isObject()) {
            QJsonObject gear = each["gear"].toObject();
            if (gear["name"].isString()) {
                ride->setTag("Equipment", gear["name"].toString());
            }
        }

        if (!each["perceived_exertion"].isNull()) {
            ride->setTag("RPE", QString("%1").arg(each["perceived_exertion"].toDouble()));
        }

        if (each["manual"].toBool()) {
            if (each["distance"].toDouble()>0) {
                QMap<QString,QString> map;
                map.insert("value", QString("%1").arg(each["distance"].toDouble()/1000.0));
                ride->metricOverrides.insert("total_distance", map);
            }
            if (each["moving_time"].toDouble()>0) {
                QMap<QString,QString> map;
                map.insert("value", QString("%1").arg(each["moving_time"].toDouble()));
                ride->metricOverrides.insert("time_riding", map);
            }
            if (each["elapsed_time"].toDouble()>0) {
                QMap<QString,QString> map;
                map.insert("value", QString("%1").arg(each["elapsed_time"].toDouble()));
                ride->metricOverrides.insert("workout_time", map);
            }
            if (each["total_elevation_gain"].toDouble()>0) {
                QMap<QString,QString> map;
                map.insert("value", QString("%1").arg(each["total_elevation_gain"].toDouble()));
                ride->metricOverrides.insert("elevation_gain", map);
            }

        } else {
            if (!addSamples(
                    ride.get(),
                    QString("%1").arg(
                        each["id"].toVariant().toULongLong()),
                    error)) {
                return false;
            }

            // laps?
            if (!each["laps"].isNull()) {
                QJsonArray laps = each["laps"].toArray();

                double last_lap = 0.0;
                for (const QJsonValue &value : laps) {
                    QJsonObject lap = value.toObject();

                    double start = starttime.secsTo(QDateTime::fromString(lap["start_date_local"].toString(), Qt::ISODate));
                    if (start < last_lap) start = last_lap + 1; // Don't overlap
                    double end = start + std::max(lap["elapsed_time"].toDouble(), 1.0) - 1;

                    last_lap = end;

                    ride->addInterval(RideFileInterval::USER, start, end, lap["name"].toString());
                }

                // Fix distance from laps and fill gaps for pool swims
                fixLapSwim(ride.get(), laps);
            }
            fixSmartRecording(ride.get());
        }



        JsonFileReader reader;
        const QByteArray converted =
            reader.toByteArray(
                context, ride.get(), true, true, true, true);
        if (converted.isEmpty()) {
            error = tr("Unable to convert the Strava activity.");
            return false;
        }
        *data = converted;

        printd("reply:%s\n", data->toStdString().c_str());
        return true;
    }

    error = tr("Strava returned malformed activity data.");
    return false;
}

static bool addStrava() {
    CloudServiceFactory::instance().addService(new Strava(NULL));
    return true;
}

static bool add = addStrava();
