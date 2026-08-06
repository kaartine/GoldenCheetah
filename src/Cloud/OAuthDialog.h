/*
 * Copyright (c) 2009 Justin F. Knotzke (jknotzke@shampoo.ca)
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

#ifndef OAUTHDIALOG_H
#define OAUTHDIALOG_H
#include "GoldenCheetah.h"
#include "Pages.h"
#include "CloudService.h"
#include <QObject>
#include <QtGui>
#include <QWidget>
#include <QStackedLayout>
#include <QUrl>
#include <QSslSocket>
#include <QUrlQuery>

#include <QWebEngineHistory>
#include <QWebEngineHistoryItem>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>

#include <cstdint>
#include <memory>

namespace OAuthCallbackPolicy {
class Session;
}

namespace StravaCredentialDurability {
class Mutation;
}

struct StravaCredentialAttempt;

class OAuthTokenReplyController;

class OAuthDialog : public QDialog
{
    Q_OBJECT
    G_OBJECT

public:
    typedef enum {
        NONE=0,
        STRAVA,
        DROPBOX,
        CYCLING_ANALYTICS,
        NOLIO,
        SPORTTRACKS,
        WITHINGS,
        POLAR,
        XERT,
        RIDEWITHGPS,
        AZUM,
        TREDICT
    } OAuthSite;

    // will work with old config via site and new via cloudservice (which is null for calendar and withings for now)
    OAuthDialog(Context *context, OAuthSite site, CloudService *service, QString baseURL="", QString clientsecret="");
    ~OAuthDialog();

#ifdef GC_OAUTH_DIALOG_TEST_HOOKS
    struct TestConstruction {};
    OAuthDialog(OAuthSite site, TestConstruction);
    bool trackTokenReplyForTest(QNetworkReply *reply);
    void finishTokenReplyForTest(QNetworkReply *reply);
#endif

    bool sslLibMissing() { return noSSLlib; }
    bool canAuthorize() const
    {
        return authorizationReady && !noSSLlib;
    }

private slots:
    // Strava/Cyclinganalytics
    void urlChanged(const QUrl& url);
    void networkRequestFinished(QNetworkReply *reply);
    void onSslErrors(QNetworkReply *reply, const QList<QSslError>&error);


private:
    void initializeTokenReplyController();
    void prepareStravaTokenRequest(
        const QUrl &tokenUrl, const QByteArray &data);
    void startTokenRequest(
        const QUrl &tokenUrl, const QByteArray &data,
        const QString &authorizationHeader = QString());
    Context *context;
    bool noSSLlib = false;
    bool authorizationReady = true;
    bool ignore = false;
    OAuthSite site;
    CloudService *service;
    QString baseURL; // can be passed, but typically is blank (used by Todays Plan)
    QString clientsecret; // can be passed, but typicall is blank (used by Todays Plan)
    QString stravaClientId;
    QString stravaClientSecret;
    QString codeVerifier; // PKCE code_verifier, used by Tredict and other PKCE services
    QStringList tokenRequestSensitiveValues;
    std::uint64_t stravaAuthorizationEpoch = 0;
    std::shared_ptr<StravaCredentialDurability::Mutation>
        stravaCredentialMutation;
    std::shared_ptr<StravaCredentialAttempt>
        stravaCredentialAttempt;
    std::unique_ptr<OAuthCallbackPolicy::Session> callbackSession;
    QUrl redirectUri;

    QVBoxLayout *layout = nullptr;

    // QUrl split into QUrlQuerty in QT5
    QWebEngineView *view = nullptr;

    QNetworkAccessManager *manager = nullptr;
    std::unique_ptr<OAuthTokenReplyController>
        tokenReplyController;

    QUrl url;
};

#endif // OAUTHDIALOG_H
