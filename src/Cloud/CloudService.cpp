/*
 * Copyright (c) 2015 Mark Liversedge (liversedge@gmail.com)
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

#include "CloudService.h"
#include "AtomicFileWriter.h"
#include "CompressedActivityFile.h"

#include "Athlete.h"
#include "RideCache.h"
#include "RideItem.h"
#include "MainWindow.h"
#include "JsonRideFile.h"
#include "CsvRideFile.h"
#include "Colors.h"
#include "Units.h"
#include "DataProcessor.h"  // to run auto data processors
#include "RideMetadata.h"   // for linked defaults processing

#include <QCoreApplication>
#include <QBuffer>
#include <QIcon>
#include <QFileIconProvider>
#include <QMessageBox>
#include <QHeaderView>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QTemporaryFile>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../qzip/zipwriter.h"

#ifdef Q_CC_MSVC
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
int cloudAutoDownloadRequestTimeoutMs();
void cloudAutoDownloadBufferAllocated(QByteArray *data);
void cloudAutoDownloadBufferReleased(QByteArray *data);
bool cloudAutoDownloadProviderAccessed(CloudService *provider);
int cloudAutoDownloadMaximumQueuedDownloadsForTest();
qint64 cloudAutoDownloadMaximumQueuedDownloadBytesForTest();
bool cloudSslWarningHandledForTest(
    const QString &title,
    const QString &message,
    quint64 occurrences,
    quint64 omitted);
#endif

//
// CLOUDSERVICE BASE CLASS
//

CloudServiceFactory *CloudServiceFactory::instance_;

namespace {

struct QueuedSslWarning
{
    std::string title;
    std::string message;
    quint64 occurrences = 1;
};

constexpr int MaximumQueuedSslWarnings = 32;
constexpr qint64 MaximumQueuedSslWarningBytes =
        qint64(256) * 1024;
std::mutex queuedSslWarningsMutex;
std::vector<QueuedSslWarning> queuedSslWarnings;
qint64 queuedSslWarningBytes = 0;
quint64 omittedSslWarningOccurrences = 0;
bool queuedSslWarningDispatchPending = false;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
CloudGuiHandoffQueueStats queuedSslWarningStats;
#endif

void showQueuedSslWarnings();

qint64 saturatedAdd(qint64 current, qint64 addition)
{
    if (addition <= 0) return current;
    const qint64 maximum = std::numeric_limits<qint64>::max();
    if (current > maximum - addition) return maximum;
    return current + addition;
}

quint64 saturatedOccurrenceAdd(quint64 current, quint64 addition)
{
    const quint64 maximum = std::numeric_limits<quint64>::max();
    if (current > maximum - addition) return maximum;
    return current + addition;
}

qint64 storedStringBytes(const std::string &value)
{
    const size_t maximum = size_t(std::numeric_limits<qint64>::max());
    return qint64(std::min(value.capacity(), maximum));
}

qint64 sslWarningBytes(const QueuedSslWarning &warning)
{
    qint64 bytes = qint64(sizeof(QueuedSslWarning));
    bytes = saturatedAdd(bytes, storedStringBytes(warning.title));
    return saturatedAdd(bytes, storedStringBytes(warning.message));
}

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
void updateSslWarningStatsLocked()
{
    queuedSslWarningStats.currentItems =
            int(queuedSslWarnings.size());
    queuedSslWarningStats.currentBytes = queuedSslWarningBytes;
    queuedSslWarningStats.maximumItems = MaximumQueuedSslWarnings;
    queuedSslWarningStats.maximumBytes =
            MaximumQueuedSslWarningBytes;
    queuedSslWarningStats.peakItems = std::max(
        queuedSslWarningStats.peakItems,
        queuedSslWarningStats.currentItems);
    queuedSslWarningStats.peakBytes = std::max(
        queuedSslWarningStats.peakBytes,
        queuedSslWarningStats.currentBytes);
    queuedSslWarningStats.queuedOccurrences = 0;
    for (const QueuedSslWarning &warning : queuedSslWarnings) {
        queuedSslWarningStats.queuedOccurrences =
                saturatedOccurrenceAdd(
                    queuedSslWarningStats.queuedOccurrences,
                    warning.occurrences);
    }
    queuedSslWarningStats.omittedOccurrences =
            omittedSslWarningOccurrences;
}
#endif

void enqueueSslWarning(
        QCoreApplication *application,
        std::string title,
        std::string message)
{
    bool scheduleDispatch = false;
    {
        const std::lock_guard<std::mutex> lock(
            queuedSslWarningsMutex);
        auto duplicate = std::find_if(
            queuedSslWarnings.begin(), queuedSslWarnings.end(),
            [&](const QueuedSslWarning &warning) {
                return warning.title == title
                    && warning.message == message;
            });
        if (duplicate != queuedSslWarnings.end()) {
            if (duplicate->occurrences
                != std::numeric_limits<quint64>::max()) {
                ++duplicate->occurrences;
            }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedSslWarningStats.coalesced;
#endif
        } else {
            QueuedSslWarning warning{
                std::move(title), std::move(message), 1};
            const qint64 bytes = sslWarningBytes(warning);
            if (int(queuedSslWarnings.size())
                    >= MaximumQueuedSslWarnings
                || bytes > MaximumQueuedSslWarningBytes
                || queuedSslWarningBytes
                    > MaximumQueuedSslWarningBytes - bytes) {
                if (omittedSslWarningOccurrences
                    != std::numeric_limits<quint64>::max()) {
                    ++omittedSslWarningOccurrences;
                }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                ++queuedSslWarningStats.rejected;
#endif
            } else {
                queuedSslWarningBytes += bytes;
                queuedSslWarnings.push_back(std::move(warning));
            }
        }

        if (!queuedSslWarningDispatchPending) {
            queuedSslWarningDispatchPending = true;
            scheduleDispatch = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedSslWarningStats.dispatchPosts;
#endif
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateSslWarningStatsLocked();
#endif
    }

    if (scheduleDispatch) {
        QMetaObject::invokeMethod(
            application,
            []() { showQueuedSslWarnings(); },
            Qt::QueuedConnection);
    }
}

void showQueuedSslWarnings()
{
    std::vector<QueuedSslWarning> warnings;
    quint64 omitted = 0;
    {
        const std::lock_guard<std::mutex> lock(
            queuedSslWarningsMutex);
        warnings.swap(queuedSslWarnings);
        queuedSslWarningBytes = 0;
        omitted = omittedSslWarningOccurrences;
        omittedSslWarningOccurrences = 0;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        ++queuedSslWarningStats.dispatchCalls;
        updateSslWarningStatsLocked();
#endif
    }

    QString title = QObject::tr("HTTP");
    QStringList messages;
    quint64 occurrences = 0;
    for (const QueuedSslWarning &warning : warnings) {
        if (messages.isEmpty()) {
            title = QString::fromUtf8(
                warning.title.data(), qsizetype(warning.title.size()));
        }
        QString message = QString::fromUtf8(
            warning.message.data(), qsizetype(warning.message.size()));
        if (warning.occurrences > 1) {
            message += QObject::tr("\nRepeated %1 times.")
                    .arg(warning.occurrences);
        }
        messages.append(message);
        occurrences = saturatedOccurrenceAdd(
            occurrences, warning.occurrences);
    }
    if (omitted > 0) {
        messages.append(
            QObject::tr("%1 additional SSL warnings were omitted.")
                .arg(omitted));
    }

    const QString message = messages.join(QStringLiteral("\n\n"));
    bool handledForTest = false;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    handledForTest = cloudSslWarningHandledForTest(
        title, message, occurrences, omitted);
#endif
    if (!handledForTest && !message.isEmpty()) {
        QMessageBox::warning(
            nullptr, title, message);
    }

    bool scheduleNext = false;
    {
        const std::lock_guard<std::mutex> lock(
            queuedSslWarningsMutex);
        if (queuedSslWarnings.empty()
            && omittedSslWarningOccurrences == 0) {
            queuedSslWarningDispatchPending = false;
        } else {
            scheduleNext = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedSslWarningStats.dispatchPosts;
#endif
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateSslWarningStatsLocked();
#endif
    }
    if (scheduleNext) {
        if (QCoreApplication *application =
                QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                application,
                []() { showQueuedSslWarnings(); },
                Qt::QueuedConnection);
        }
    }
}

struct QueuedCloudSettingExpectation
{
    std::string key;
    std::string expectedValue;
    std::string defaultValue;
    bool athleteScoped = true;
};

struct QueuedCloudSettingUpdate
{
    std::string key;
    std::string value;
    bool athleteScoped = true;
};

struct QueuedCloudSettingsTransaction
{
    std::string athlete;
    std::string source;
    std::vector<QueuedCloudSettingExpectation> expectations;
    std::vector<QueuedCloudSettingUpdate> updates;
};

constexpr int MaximumQueuedCloudSettingsTransactions = 64;
constexpr qint64 MaximumQueuedCloudSettingsBytes =
        qint64(1024) * 1024;
std::mutex queuedCloudSettingsMutex;
std::deque<QueuedCloudSettingsTransaction> queuedCloudSettings;
qint64 queuedCloudSettingsBytes = 0;
bool queuedCloudSettingsDispatchPending = false;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
CloudGuiHandoffQueueStats queuedCloudSettingsStats;
#endif

void dispatchQueuedCloudSettings();

qint64 cloudSettingsTransactionBytes(
        const QueuedCloudSettingsTransaction &transaction)
{
    qint64 bytes = qint64(sizeof(QueuedCloudSettingsTransaction));
    bytes = saturatedAdd(bytes, storedStringBytes(transaction.athlete));
    bytes = saturatedAdd(bytes, storedStringBytes(transaction.source));
    for (const QueuedCloudSettingExpectation &setting
         : transaction.expectations) {
        bytes = saturatedAdd(
            bytes, qint64(sizeof(QueuedCloudSettingExpectation)));
        bytes = saturatedAdd(bytes, storedStringBytes(setting.key));
        bytes = saturatedAdd(
            bytes, storedStringBytes(setting.expectedValue));
        bytes = saturatedAdd(
            bytes, storedStringBytes(setting.defaultValue));
    }
    for (const QueuedCloudSettingUpdate &setting
         : transaction.updates) {
        bytes = saturatedAdd(
            bytes, qint64(sizeof(QueuedCloudSettingUpdate)));
        bytes = saturatedAdd(bytes, storedStringBytes(setting.key));
        bytes = saturatedAdd(bytes, storedStringBytes(setting.value));
    }
    return bytes;
}

bool sameSetting(
        const QueuedCloudSettingExpectation &left,
        const QueuedCloudSettingExpectation &right)
{
    return left.athleteScoped == right.athleteScoped
        && left.key == right.key;
}

bool sameSetting(
        const QueuedCloudSettingUpdate &left,
        const QueuedCloudSettingExpectation &right)
{
    return left.athleteScoped == right.athleteScoped
        && left.key == right.key;
}

bool sameSetting(
        const QueuedCloudSettingUpdate &left,
        const QueuedCloudSettingUpdate &right)
{
    return left.athleteScoped == right.athleteScoped
        && left.key == right.key;
}

bool composeCloudSettingsTransactions(
        const QueuedCloudSettingsTransaction &pending,
        const QueuedCloudSettingsTransaction &next,
        QueuedCloudSettingsTransaction &composed)
{
    if (pending.athlete != next.athlete
        || pending.source != next.source) {
        return false;
    }

    for (const QueuedCloudSettingExpectation &expected
         : next.expectations) {
        const auto pendingExpectation = std::find_if(
            pending.expectations.cbegin(), pending.expectations.cend(),
            [&](const QueuedCloudSettingExpectation &candidate) {
                return sameSetting(candidate, expected);
            });
        if (pendingExpectation == pending.expectations.cend()
            || pendingExpectation->defaultValue
                != expected.defaultValue) {
            return false;
        }

        std::string virtualValue = pendingExpectation->expectedValue;
        const auto pendingUpdate = std::find_if(
            pending.updates.cbegin(), pending.updates.cend(),
            [&](const QueuedCloudSettingUpdate &candidate) {
                return sameSetting(candidate, expected);
            });
        if (pendingUpdate != pending.updates.cend()) {
            virtualValue = pendingUpdate->value;
            if (virtualValue.empty()
                && !expected.defaultValue.empty()) {
                virtualValue = expected.defaultValue;
            }
        }
        if (virtualValue != expected.expectedValue) return false;
    }

    composed = pending;
    for (const QueuedCloudSettingUpdate &update : next.updates) {
        auto existing = std::find_if(
            composed.updates.begin(), composed.updates.end(),
            [&](const QueuedCloudSettingUpdate &candidate) {
                return sameSetting(candidate, update);
            });
        if (existing == composed.updates.end()) {
            composed.updates.push_back(update);
        } else {
            existing->value = update.value;
        }
    }
    return true;
}

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
void updateCloudSettingsStatsLocked()
{
    queuedCloudSettingsStats.currentItems =
            int(queuedCloudSettings.size());
    queuedCloudSettingsStats.currentBytes = queuedCloudSettingsBytes;
    queuedCloudSettingsStats.maximumItems =
            MaximumQueuedCloudSettingsTransactions;
    queuedCloudSettingsStats.maximumBytes =
            MaximumQueuedCloudSettingsBytes;
    queuedCloudSettingsStats.peakItems = std::max(
        queuedCloudSettingsStats.peakItems,
        queuedCloudSettingsStats.currentItems);
    queuedCloudSettingsStats.peakBytes = std::max(
        queuedCloudSettingsStats.peakBytes,
        queuedCloudSettingsStats.currentBytes);
}
#endif

bool enqueueCloudSettingsTransaction(
        QCoreApplication *application,
        QueuedCloudSettingsTransaction transaction)
{
    bool accepted = false;
    bool scheduleDispatch = false;
    {
        const std::lock_guard<std::mutex> lock(
            queuedCloudSettingsMutex);
        if (!queuedCloudSettings.empty()) {
            QueuedCloudSettingsTransaction composed;
            if (composeCloudSettingsTransactions(
                    queuedCloudSettings.back(),
                    transaction,
                    composed)) {
                const qint64 previousBytes =
                    cloudSettingsTransactionBytes(
                        queuedCloudSettings.back());
                const qint64 composedBytes =
                    cloudSettingsTransactionBytes(composed);
                if (composedBytes <= MaximumQueuedCloudSettingsBytes
                    && queuedCloudSettingsBytes - previousBytes
                        <= MaximumQueuedCloudSettingsBytes
                            - composedBytes) {
                    queuedCloudSettings.back() = std::move(composed);
                    queuedCloudSettingsBytes =
                        queuedCloudSettingsBytes
                        - previousBytes + composedBytes;
                    accepted = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                    ++queuedCloudSettingsStats.coalesced;
#endif
                }
            }
        }

        if (!accepted) {
            const qint64 bytes =
                    cloudSettingsTransactionBytes(transaction);
            if (int(queuedCloudSettings.size())
                    < MaximumQueuedCloudSettingsTransactions
                && bytes <= MaximumQueuedCloudSettingsBytes
                && queuedCloudSettingsBytes
                    <= MaximumQueuedCloudSettingsBytes - bytes) {
                queuedCloudSettingsBytes += bytes;
                queuedCloudSettings.push_back(std::move(transaction));
                accepted = true;
            } else {
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                ++queuedCloudSettingsStats.rejected;
#endif
            }
        }

        if (accepted && !queuedCloudSettingsDispatchPending) {
            queuedCloudSettingsDispatchPending = true;
            scheduleDispatch = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedCloudSettingsStats.dispatchPosts;
#endif
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateCloudSettingsStatsLocked();
#endif
    }

    if (scheduleDispatch) {
        QMetaObject::invokeMethod(
            application,
            []() { dispatchQueuedCloudSettings(); },
            Qt::QueuedConnection);
    }
    return accepted;
}

QString storedCloudSetting(
        const QString &athlete,
        const QString &key,
        const QString &defaultValue,
        bool athleteScoped)
{
    const QString stored = athleteScoped
            ? appsettings->cvalue(
                athlete, key, defaultValue).toString()
            : appsettings->value(
                nullptr, key, defaultValue).toString();
    if (stored.isEmpty() && !defaultValue.isEmpty())
        return defaultValue;
    return stored;
}

std::vector<QueuedCloudSettingsTransaction>
takeQueuedCloudSettings()
{
    std::vector<QueuedCloudSettingsTransaction> transactions;
    {
        const std::lock_guard<std::mutex> lock(
            queuedCloudSettingsMutex);
        transactions.reserve(queuedCloudSettings.size());
        while (!queuedCloudSettings.empty()) {
            transactions.push_back(
                std::move(queuedCloudSettings.front()));
            queuedCloudSettings.pop_front();
        }
        queuedCloudSettingsBytes = 0;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateCloudSettingsStatsLocked();
#endif
    }
    return transactions;
}

void applyCloudSettingsTransactions(
        const std::vector<QueuedCloudSettingsTransaction> &transactions)
{
    for (const QueuedCloudSettingsTransaction &transaction
         : transactions) {
        const QString athlete = QString::fromUtf8(
            transaction.athlete.data(),
            qsizetype(transaction.athlete.size()));
        bool matches = true;
        for (const QueuedCloudSettingExpectation &setting
             : transaction.expectations) {
            const QString key = QString::fromUtf8(
                setting.key.data(), qsizetype(setting.key.size()));
            const QString expectedValue = QString::fromUtf8(
                setting.expectedValue.data(),
                qsizetype(setting.expectedValue.size()));
            const QString defaultValue = QString::fromUtf8(
                setting.defaultValue.data(),
                qsizetype(setting.defaultValue.size()));
            if (storedCloudSetting(
                    athlete, key, defaultValue,
                    setting.athleteScoped) != expectedValue) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;

        for (const QueuedCloudSettingUpdate &setting
             : transaction.updates) {
            const QString key = QString::fromUtf8(
                setting.key.data(), qsizetype(setting.key.size()));
            const QString value = QString::fromUtf8(
                setting.value.data(), qsizetype(setting.value.size()));
            if (setting.athleteScoped)
                appsettings->setCValue(athlete, key, value);
            else
                appsettings->setValue(key, value);
#ifdef GC_WANT_ALLDEBUG
            qDebug() << "factory save setting:"
                     << key << value;
#endif
        }
    }
}

void applyQueuedCloudSettings()
{
    applyCloudSettingsTransactions(takeQueuedCloudSettings());
}

void dispatchQueuedCloudSettings()
{
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    {
        const std::lock_guard<std::mutex> lock(
            queuedCloudSettingsMutex);
        ++queuedCloudSettingsStats.dispatchCalls;
    }
#endif
    applyQueuedCloudSettings();

    bool scheduleNext = false;
    {
        const std::lock_guard<std::mutex> lock(
            queuedCloudSettingsMutex);
        if (queuedCloudSettings.empty()) {
            queuedCloudSettingsDispatchPending = false;
        } else {
            scheduleNext = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedCloudSettingsStats.dispatchPosts;
#endif
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateCloudSettingsStatsLocked();
#endif
    }
    if (scheduleNext) {
        if (QCoreApplication *application =
                QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                application,
                []() { dispatchQueuedCloudSettings(); },
                Qt::QueuedConnection);
        }
    }
}

bool cloudSettingsMatch(
        const QString &athlete,
        const QMap<QString, QString> &expectedSettings,
        const QMap<QString, QString> &settingDefaults)
{
    for (auto setting = expectedSettings.cbegin();
         setting != expectedSettings.cend(); ++setting) {
        const bool athleteScoped =
                setting.key().startsWith(QStringLiteral("<athlete"));
        if (storedCloudSetting(
                athlete, setting.key(),
                settingDefaults.value(setting.key()),
                athleteScoped) != setting.value()) {
            return false;
        }
    }
    return true;
}

} // namespace

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
CloudGuiHandoffQueueStats cloudSettingsQueueStatsForTest()
{
    const std::lock_guard<std::mutex> lock(
        queuedCloudSettingsMutex);
    updateCloudSettingsStatsLocked();
    return queuedCloudSettingsStats;
}

CloudGuiHandoffQueueStats cloudSslWarningQueueStatsForTest()
{
    const std::lock_guard<std::mutex> lock(
        queuedSslWarningsMutex);
    updateSslWarningStatsLocked();
    return queuedSslWarningStats;
}

void resetCloudServiceHandoffQueuesForTest()
{
    {
        const std::lock_guard<std::mutex> lock(
            queuedCloudSettingsMutex);
        queuedCloudSettings.clear();
        queuedCloudSettingsBytes = 0;
        queuedCloudSettingsDispatchPending = false;
        queuedCloudSettingsStats = {};
        updateCloudSettingsStatsLocked();
    }
    {
        const std::lock_guard<std::mutex> lock(
            queuedSslWarningsMutex);
        queuedSslWarnings.clear();
        queuedSslWarningBytes = 0;
        omittedSslWarningOccurrences = 0;
        queuedSslWarningDispatchPending = false;
        queuedSslWarningStats = {};
        updateSslWarningStatsLocked();
    }
}
#endif


// nothing doing in base class, for now
CloudService::CloudService(Context *context) :
    uploadCompression(zip), downloadCompression(zip),
    filetype(JSON), useMetric(false), useEndDate(false), context(context)
{
}

// clean up on delete
CloudService::~CloudService()
{
    foreach(CloudServiceEntry *p, list_) delete p;
    list_.clear();
}

void
CloudService::abortRequests()
{
    const QList<QNetworkReply *> replies = findChildren<QNetworkReply *>();
    for (QNetworkReply *reply : replies) {
        if (reply && reply->isRunning()) reply->abort();
    }
}

// get a new filestore entry
CloudServiceEntry *
CloudService::newCloudServiceEntry()
{
    CloudServiceEntry *p = new CloudServiceEntry();
    p->initial = true;
    list_ << p;
    return p;
}

bool
CloudService::upload(QWidget *parent, Context *context, CloudService *store, RideItem *item)
{

    // open a dialog to do it
    CloudServiceUploadDialog uploader(parent, context, store, item);
    int ret = uploader.exec();

    // was it successfull ?
    if (ret == QDialog::Accepted) return true;
    else return false;
}

//
// Utility function to create a QByteArray of data in GZIP format
// This is essentially the same as qCompress but creates it in
// GZIP format (with requisite headers) instead of ZLIB's format
// which has less filename info in the header
//
static QByteArray gCompress(const QByteArray &source)
{
    // int size is source.size()
    // const char *data is source.data()
    z_stream strm;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    // note that (15+16) below means windowbits+_16_ adds the gzip header/footer
    deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, (15+16), 8, Z_DEFAULT_STRATEGY);

    // input data
    strm.avail_in = source.size();
    strm.next_in = (Bytef *)source.data();

    // output data - on stack not heap, will be released
    QByteArray dest(source.size()/2, '\0'); // should compress by 50%, if not don't bother

    strm.avail_out = source.size()/2;
    strm.next_out = (Bytef *)dest.data();

    // now compress!
    deflate(&strm, Z_FINISH);

    // return byte array on the stack
    return QByteArray(dest.data(), (source.size()/2) - strm.avail_out);
}

void
CloudService::compressRide(RideFile*ride, QByteArray &data, QString name)
{
    // compress via a temporary file
    QTemporaryFile tempfile;
    tempfile.open();
    tempfile.close();

    // write as file type requested
    QString spec;
    switch(filetype) {
        default:
        case JSON: spec="json"; break;
        case TCX: spec="tcx"; break;
        case PWX: spec="pwx"; break;
        case FIT: spec="fit"; break;
        case CSV: spec="csv"; break;
    }

    QFile jsonFile(tempfile.fileName());
    bool result;

    if (spec == "csv") {
        CsvFileReader writer;
        result = writer.writeRideFile(ride->context, ride, jsonFile, CsvFileReader::gc);
    } else {
        result = RideFileFactory::instance().writeRideFile(ride->context, ride, jsonFile, spec);
    }

    if (result == true) {
        // read the ride file
        jsonFile.open(QFile::ReadOnly);
        data = jsonFile.readAll();
        jsonFile.close();

        if (uploadCompression == zip) {
            // create a temp zip file
            QTemporaryFile zipFile;
            zipFile.open();
            zipFile.close();

            // add files using zip writer
            QString zipname = zipFile.fileName();
            ZipWriter writer(zipname);

            // add the ride file to the zip file
            writer.addFile(name, data);
            writer.close();

            // now read in the zipfile
            QFile zip(zipname);
            zip.open(QFile::ReadOnly);
            data = zip.readAll();
            zip.close();
        } else if (uploadCompression == gzip) {
            data = gCompress(data);
        }
    }
}

// name is the source name (i.e. what it is called on the file store (xxxxx.json.zip)
RideFile *
CloudService::uncompressRide(QByteArray *data, QString name, QStringList &errors)
{
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    if (cloudAutoDownloadProviderAccessed(this)) return nullptr;
#endif

    if (!data) {
        errors << tr("activity data is missing.");
        return nullptr;
    }
    return uncompressRide(
        context, downloadCompression, *data, std::move(name), errors);
}

RideFile *
CloudService::uncompressRide(
        Context *context,
        CompressionType compression,
        const QByteArray &data,
        QString name,
        QStringList &errors)
{
    // make sure its named as we expect
    if ((compression == zip && !name.endsWith(".zip")) ||
        (compression == gzip && !name.endsWith(".gz"))) {
        errors << tr("expected compressed activity file.");
        return NULL;
    }

    // some services will offer file as compressed or uncompressed
    // data. In which case they must add .zip or .gz to the end of the
    // filename to indicate it. The file format must still be included
    // in the name e.g. .pwx.gz or .fit.zip
    enum class PayloadEncoding { Plain, Zip, Gzip };
    PayloadEncoding encoding = PayloadEncoding::Plain;
    if (name.endsWith(".zip")) {
        encoding = PayloadEncoding::Zip;
        name.chop(4);
    } else if (name.endsWith(".gz")) {
        encoding = PayloadEncoding::Gzip;
        name.chop(3);
    }

    if (!context || !context->athlete || !context->athlete->home) {
        errors << tr("activity storage is unavailable.");
        return nullptr;
    }

    const QString fileTemplate = QDir(
        context->athlete->home->temp().absolutePath()).filePath(
            QStringLiteral("gc-cloud-XXXXXX.%1")
                .arg(QFileInfo(name).suffix()));
    QTemporaryFile file(fileTemplate);
    if (!file.open()) {
        errors << tr("cannot create a temporary activity file.");
        return nullptr;
    }

    bool decoded = false;
    if (encoding == PayloadEncoding::Plain) {
        decoded = file.write(data) == data.size();
    } else {
        auto compressedData = std::make_unique<QBuffer>();
        compressedData->setData(data);
        const CompressedActivityFile::Format format =
            encoding == PayloadEncoding::Zip
                ? CompressedActivityFile::Format::Zip
                : CompressedActivityFile::Format::Gzip;
        decoded = compressedData->open(QIODevice::ReadOnly)
            && CompressedActivityFile::extractSingleFile(
                std::move(compressedData), format, &file);
    }

    if (!decoded || !file.flush()) {
        errors << tr("activity file is invalid or exceeds safety limits.");
        return nullptr;
    }
    file.close();

    // read the file in using the correct ridefile reader
    RideFile *ride = RideFileFactory::instance().openRideFile(context, file, errors);

    // return whatever we got
    return ride;
}

void
CloudService::sslErrors(QWidget* parent, [[maybe_unused]] QNetworkReply* reply ,QList<QSslError> errors)
{
    QString errorString = "";
    foreach (const QSslError e, errors ) {
        if (!errorString.isEmpty())
            errorString += ", ";
        errorString += e.errorString();
    }

    const QString title = tr("HTTP");
    const QString message =
            tr("SSL error(s) has occurred: %1").arg(errorString);
    QCoreApplication *application = QCoreApplication::instance();
    if (application
        && QThread::currentThread() != application->thread()) {
        const QByteArray titleUtf8 = title.toUtf8();
        const QByteArray messageUtf8 = message.toUtf8();
        enqueueSslWarning(
            application,
            titleUtf8.toStdString(),
            messageUtf8.toStdString());
        return;
    }
    QMessageBox::warning(parent, title, message);
    //reply->ignoreSslErrors(); // disabled for security reasons
}

QString
CloudService::uploadExtension() {
    QString spec;
    switch (filetype) {
        default:
        case JSON: spec = ".json"; break;
        case TCX: spec = ".tcx"; break;
        case PWX: spec = ".pwx"; break;
        case FIT: spec = ".fit"; break;
        case CSV: spec = ".csv"; break;
    }

    switch (uploadCompression) {
        case zip: spec += ".zip"; break;
        case gzip: spec += ".gz"; break;
        default:
        case none: break;
    }
    return spec;
}

CloudServiceUploadDialog::CloudServiceUploadDialog(QWidget *parent, Context *context, CloudService *store, RideItem *item) : QDialog(parent), context(context), store(store), item(item)
{
    // setup the gui!
    QVBoxLayout *layout = new QVBoxLayout(this);
    info = new QLabel(QString(tr("Uploading %1 bytes...")).arg(data.size()));
    layout->addWidget(info);

    progress = new QProgressBar(this);
    progress->setMaximum(0);
    progress->setValue(0);
    layout->addWidget(progress);

    okcancel = new QPushButton(tr("Cancel"));
    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(okcancel);
    layout->addLayout(buttons);

    // lets open the store
    QStringList errors;
    status = store->open(errors);

    // compress and upload if opened successfully.
    if (status == true) {

        // check for unsaved changes
        if (item->isDirty()) {

               QMessageBox msgBox;
                msgBox.setWindowTitle(tr("Upload to ") + store->uiName());
                msgBox.setText(tr("The activity you want to upload has unsaved changes."));
                msgBox.setDetailedText(tr("Unsaved changes in activities will be uploaded as well. \n\n"
                                          "This may lead to inconsistencies between your local activities "
                                          "and the uploaded activities if you do not save the activity in GoldenCheetah. "
                                          "We recommend to save the changed activity before proceeding."));
                msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Ignore | QMessageBox::Cancel);
                msgBox.setIcon(QMessageBox::Question);
                int ret = msgBox.exec();
                switch (ret) {
                case QMessageBox::Save:
                    // save
                    context->notifyMetadataFlush();
                    context->ride->notifyRideMetadataChanged();
                    {
                        QString error;
                        if (!MainWindow::saveSilent(
                                context, item, &error)) {
                            QMessageBox::warning(
                                this, tr("Save Activity"), error);
                            QMetaObject::invokeMethod(
                                this, "close", Qt::QueuedConnection);
                            return;
                        }
                    }
                    break;
                case QMessageBox::Ignore:
                    // just proceed
                    break;
                case QMessageBox::Cancel:
                    QApplication::processEvents();
                    QMetaObject::invokeMethod(this, "close", Qt::QueuedConnection);
                    return;
                default:
                    // should never be reached
                    break;
                }

        }

        // get a compressed version
        store->compressRide(item->ride(), data, QFileInfo(item->fileName).baseName() + ".json");

        // ok, so now we can kickoff the upload
        status = store->writeFile(data, QFileInfo(item->fileName).baseName() + store->uploadExtension(), item->ride());
    }

    // if the upload failed in any way, bail out
    if (status == false) {

        // didn't work dude
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Upload Failed") + store->uiName());
        msgBox.setText(tr("Unable to upload, check your configuration in preferences."));

        msgBox.setIcon(QMessageBox::Critical);
        msgBox.exec();

        QWidget::hide(); // don't show just yet...
        QApplication::processEvents();

        return;
    }

    // get notification when done
    connect(store, SIGNAL(writeComplete(QString,QString)), this, SLOT(completed(QString,QString)));

}

int
CloudServiceUploadDialog::exec()
{
    if (status) return QDialog::exec();
    else {
        QDialog::accept();
        return 0;
    }
}

void
CloudServiceUploadDialog::completed(QString file, QString message)
{
    info->setText(file + "\n" + message);
    progress->setMaximum(1);
    progress->setValue(1);
    okcancel->setText(tr("OK"));
    connect(okcancel, SIGNAL(clicked()), this, SLOT(accept()));
}

CloudServiceDialog::CloudServiceDialog(QWidget *parent, CloudService *store, QString title, QString pathname, bool dironly) :
    QDialog(parent), store(store), title(title), pathname(pathname), dironly(dironly)
{
    //setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(350*dpiXFactor, 400*dpiYFactor);
    setWindowTitle(title + " (" + store->uiName() + ")");
    QVBoxLayout *layout = new QVBoxLayout(this);

    pathEdit = new QLineEdit(this);
    pathEdit->setText(pathname);
    layout->addWidget(pathEdit);

    splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);

    folders = new QTreeWidget(this);
    folders->headerItem()->setText(0, tr("Folder"));
    folders->setColumnCount(1);
    folders->setSelectionMode(QAbstractItemView::SingleSelection);
    folders->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    folders->setIndentation(0);

    files = new QTreeWidget(this);
    files->headerItem()->setText(0, tr("Name"));
    files->headerItem()->setText(1, tr("Type"));
    files->headerItem()->setText(2, tr("Modified"));
    files->setColumnCount(3);
    files->setSelectionMode(QAbstractItemView::SingleSelection);
    files->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    files->setIndentation(0);

    splitter->addWidget(folders);
    splitter->addWidget(files);

    splitter->setStretchFactor(0,30);
    splitter->setStretchFactor(1,70);

    layout->addWidget(splitter);

    QHBoxLayout *buttons = new QHBoxLayout;
    create = new QPushButton(tr("Create Folder"), this);
    cancel = new QPushButton(tr("Cancel"), this);
    open = new QPushButton(tr("Open"), this);

    buttons->addWidget(create);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(open);
    layout->addLayout(buttons);

    // want selection or not ?
    connect(create, SIGNAL(clicked()), this, SLOT(createFolderClicked()));
    connect(cancel, SIGNAL(clicked()), this, SLOT(reject()));
    connect(open, SIGNAL(clicked()), this, SLOT(accept()));
    connect(pathEdit, SIGNAL(returnPressed()), this, SLOT(returnPressed()));
    connect(folders, SIGNAL(itemSelectionChanged()), this, SLOT(folderSelectionChanged()));
    connect(files, SIGNAL(itemDoubleClicked(QTreeWidgetItem*,int)), this, SLOT(fileDoubleClicked(QTreeWidgetItem*,int)));

    // trap return key pressed for a file dialog
    installEventFilter(this);

    // set path to selected
    setPath(pathname);
}

void
CloudServiceDialog::returnPressed()
{
    setPath(pathEdit->text());
}

// set path
void 
CloudServiceDialog::setPath(QString path, bool refresh)
{
    QStringList errors; // keep a track of errors
    QString pathing; // keeping a track of the path we have followed

    // get root
    CloudServiceEntry *fse = store->root();

    // is it NULL!?
    if (fse == NULL) return;

    // get list of paths to travers
    QStringList paths = path.split("/");

    // remove first and last blanks that are caused
    // by path beginning and ending in a "/"
    if (paths.count() && paths.first() == "") paths.removeAt(0);
    if (paths.count() && paths.last() == "") paths.removeAt(paths.count()-1);

    // start at root
    pathing = "/";
    if (refresh || fse->initial == true) {
        fse->children = store->readdir(pathing, errors);
        if (errors.count() == 0) {
            fse->initial = false;

            // initialise the folders list
            setFolders(fse);
        }
    }

    // traverse the paths to the destination
    foreach(QString directory, paths) {

        // find the directory in children
        int index = fse->child(directory);

        // not found!
        if (index == -1) break;

        // update pathing
        if (!pathing.endsWith("/")) pathing += "/";
        pathing += directory;

        // drop into directory and refresh if needed
        fse = fse->children[index];
        if (refresh || fse->initial == true) {
            fse->children = store->readdir(pathing, errors);
            if (errors.count() == 0) fse->initial = false;
        }
    }

    // reset to where we got
    pathEdit->setText(pathing);
    pathname=pathing;
    setFiles(fse);
}

bool CloudServiceDialog::eventFilter(QObject *obj, QEvent *evt)
{
    if (obj != this) return false;

    if(evt->type() == QEvent::KeyPress) {

        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(evt);

        // ignore it !
        if(keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Return )
            return true; 
    }

    // do the usual thing.
    return false;
}

void 
CloudServiceDialog::folderSelectionChanged()
{
    // is there a selected item?
    if (folders->selectedItems().count()) folderClicked(folders->selectedItems().first(), 0);
}

void
CloudServiceDialog::folderClicked(QTreeWidgetItem *item, int)
{
    // user clicked on a folder so set path
    int index = folders->invisibleRootItem()->indexOfChild(item);

    // set folder path to whatever was clicked
    if (index == 0) setPath("/");
    else if (index > 0) setPath("/" + item->text(0));
}

void 
CloudServiceDialog::fileDoubleClicked(QTreeWidgetItem*item, int)
{
    // try and set the path to the item double clicked
    if (pathname.endsWith("/")) setPath(pathname + item->text(0));
    else setPath(pathname + "/" + item->text(0));
}

void
CloudServiceDialog::setFolders(CloudServiceEntry *fse)
{
    // icons
    QFileIconProvider provider;

    // set the folders tree widget
    folders->clear();

    // Add ROOT
    QTreeWidgetItem *rootitem = new QTreeWidgetItem(folders);
    rootitem->setText(0, "/");
    rootitem->setIcon(0, provider.icon(QFileIconProvider::Folder));

    // add each FOLDER from the list
    foreach(CloudServiceEntry *p, fse->children) {
        if (p->isDir) {
            QTreeWidgetItem *item = new QTreeWidgetItem(folders);
            item->setText(0, p->name);
            item->setIcon(0, provider.icon(QFileIconProvider::Folder));
        }
    }
}

void
CloudServiceDialog::setFiles(CloudServiceEntry *fse)
{
    // icons
    QFileIconProvider provider;

    // set the files tree widget
    files->clear();

    // add each FOLDER from the list
    foreach(CloudServiceEntry *p, fse->children) {

        QTreeWidgetItem *item = new QTreeWidgetItem(files);

        // if only directories disable files for selection (but show for context)
        if (dironly && !p->isDir) item->setFlags(item->flags() & ~(Qt::ItemIsSelectable|Qt::ItemIsEnabled));


        // type
        if (p->isDir) {
            item->setText(1, tr("Folder"));
            item->setText(0, p->name);
            item->setIcon(0, provider.icon(QFileIconProvider::Folder));
        } else {
            item->setText(0, QFileInfo(p->name).baseName()); // no need for extensions
            item->setText(1, QFileInfo(p->name).suffix().toLower());
            item->setIcon(0, provider.icon(QFileIconProvider::File));
        }

        // modified - time or date?
        if (p->modified.date() == QDate::currentDate())
            item->setText(2, p->modified.toString("hh:mm:ss"));
        else
            item->setText(2, p->modified.toString(tr("d MMM yyyy")));
    }
}

void
CloudServiceDialog::createFolderClicked()
{
    FolderNameDialog dialog(this);
    int ret = dialog.exec();
    if (ret == QDialog::Accepted && dialog.name() != "") {
        // go and create it ! special treatment for / root
        if (pathname == "/") {
            store->createFolder(pathname + dialog.name());
        } else {
            store->createFolder(pathname + "/" + dialog.name());
        }

        // refresh !
        setPath(pathname, true);
    }
}

FolderNameDialog::FolderNameDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Folder Name"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    nameEdit = new QLineEdit(this);
    layout->addWidget(nameEdit);

    cancel = new QPushButton(tr("Cancel"));
    create = new QPushButton(tr("Create"));

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(create);
    layout->addLayout(buttons);

    connect(cancel, SIGNAL(clicked()), this, SLOT(reject()));
    connect(create, SIGNAL(clicked()), this, SLOT(accept()));
}

CloudServiceSyncDialog::CloudServiceSyncDialog(Context *context, CloudService *store)
    : QDialog(context->mainWindow, Qt::Dialog), context(context), store(store), downloading(false), aborted(false)
{
    setWindowTitle(tr("Synchronise ") + store->uiName());
    setMinimumSize(850 *dpiXFactor,450 *dpiYFactor);

    QStringList errors;
    if (store->open(errors) == false) {
        QWidget::hide(); // meh

        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Sync with ") + store->uiName());
        msgBox.setText(tr("Unable to connect, check your configuration in preferences."));
        msgBox.setDetailedText(errors.join("\n"));

        msgBox.setIcon(QMessageBox::Critical);
        msgBox.exec();

        QWidget::hide(); // don't show just yet...
        QApplication::processEvents();

        QMetaObject::invokeMethod(this, "close", Qt::QueuedConnection);

        return;
    }

    // setup tabs
    tabs = new QTabWidget(this);
    QWidget * upload = new QWidget(this);
    QWidget * download = new QWidget(this);
    QWidget * sync = new QWidget(this);
    tabs->addTab(download, tr("Download"));
    if (store->capabilities() & CloudService::Upload) tabs->addTab(upload, tr("Upload"));
    tabs->addTab(sync, tr("Synchronize"));
    tabs->setCurrentIndex(2);
    QVBoxLayout *downloadLayout = new QVBoxLayout(download);
    QVBoxLayout *uploadLayout = new QVBoxLayout(upload);
    QVBoxLayout *syncLayout = new QVBoxLayout(sync);

    // notification when upload/download completes
    connect (store, SIGNAL(writeComplete(QString,QString)), this, SLOT(completedWrite(QString,QString)));
    connect (store, SIGNAL(readComplete(QByteArray*,QString,QString)), this, SLOT(completedRead(QByteArray*,QString,QString)));

    // combo box
    athleteCombo = new QComboBox(this);
    athleteCombo->addItem(context->athlete->cyclist);
    athleteCombo->setCurrentIndex(0);

    QLabel *fromLabel = new QLabel(tr("From:"), this);
    QLabel *toLabel = new QLabel(tr("To:"), this);

    from = new QDateEdit(this);
    from->setDate(QDate::currentDate().addMonths(-1));
    from->setCalendarPopup(true);
    to = new QDateEdit(this);
    to->setDate(QDate::currentDate());
    to->setCalendarPopup(true);

    // Buttons
    refreshButton = new QPushButton(tr("Refresh List"), this);
    cancelButton = new QPushButton(tr("Close"),this);
    downloadButton = new QPushButton(tr("Download"),this);

    selectAll = new QCheckBox(tr("Select all"), this);
    selectAll->setChecked(Qt::Unchecked);

    // ride list
    rideListDown = new QTreeWidget(this);
    rideListDown->headerItem()->setText(0, " ");
    rideListDown->headerItem()->setText(1, tr("Workout Name"));
    rideListDown->headerItem()->setText(2, tr("Date"));
    rideListDown->headerItem()->setText(3, tr("Time"));
    rideListDown->headerItem()->setText(4, tr("Exists"));
    rideListDown->headerItem()->setText(5, tr("Status"));
    rideListDown->headerItem()->setText(6, tr("Workout Id"));
    rideListDown->setColumnCount(6);
    rideListDown->setSelectionMode(QAbstractItemView::SingleSelection);
    rideListDown->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    rideListDown->setUniformRowHeights(true);
    rideListDown->setIndentation(0);
    rideListDown->header()->resizeSection(0,20*dpiXFactor);
    rideListDown->header()->resizeSection(1,90*dpiXFactor);
    rideListDown->header()->resizeSection(2,100*dpiXFactor);
    rideListDown->header()->resizeSection(3,100*dpiXFactor);
    rideListDown->header()->resizeSection(6,50*dpiXFactor);
    rideListDown->setSortingEnabled(true);

    downloadLayout->addWidget(selectAll);
    downloadLayout->addWidget(rideListDown);

    selectAllUp = new QCheckBox(tr("Select all"), this);
    selectAllUp->setChecked(Qt::Unchecked);

    // ride list
    rideListUp = new QTreeWidget(this);
    rideListUp->headerItem()->setText(0, " ");
    rideListUp->headerItem()->setText(1, tr("File"));
    rideListUp->headerItem()->setText(2, tr("Date"));
    rideListUp->headerItem()->setText(3, tr("Time"));
    rideListUp->headerItem()->setText(4, tr("Duration"));
    rideListUp->headerItem()->setText(5, tr("Distance"));
    rideListUp->headerItem()->setText(6, tr("Exists"));
    rideListUp->headerItem()->setText(7, tr("Status"));
    rideListUp->setColumnCount(8);
    rideListUp->setSelectionMode(QAbstractItemView::SingleSelection);
    rideListUp->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    rideListUp->setUniformRowHeights(true);
    rideListUp->setIndentation(0);
    rideListUp->header()->resizeSection(0,20*dpiXFactor);
    rideListUp->header()->resizeSection(1,200*dpiXFactor);
    rideListUp->header()->resizeSection(2,100*dpiXFactor);
    rideListUp->header()->resizeSection(3,100*dpiXFactor);
    rideListUp->header()->resizeSection(4,100*dpiXFactor);
    rideListUp->header()->resizeSection(5,70*dpiXFactor);
    rideListUp->header()->resizeSection(6,50*dpiXFactor);
    rideListUp->setSortingEnabled(true);

    uploadLayout->addWidget(selectAllUp);
    uploadLayout->addWidget(rideListUp);

    selectAllSync = new QCheckBox(tr("Select all"), this);
    selectAllSync->setChecked(Qt::Unchecked);
    syncMode = new QComboBox(this);
    syncMode->addItem(tr("Keep all do not delete"));
    syncMode->addItem(tr("Keep %1 but delete Local").arg(store->uiName()));
    syncMode->addItem(tr("Keep Local but delete %1").arg(store->uiName()));
    QHBoxLayout *syncList = new QHBoxLayout;
    syncList->addWidget(selectAllSync);
    syncList->addStretch();
    syncList->addWidget(syncMode);


    // ride list
    rideListSync = new QTreeWidget(this);
    rideListSync->headerItem()->setText(0, " ");
    rideListSync->headerItem()->setText(1, tr("Source"));
    rideListSync->headerItem()->setText(2, tr("Date"));
    rideListSync->headerItem()->setText(3, tr("Time"));
    rideListSync->headerItem()->setText(4, tr("Duration"));
    rideListSync->headerItem()->setText(5, tr("Distance"));
    rideListSync->headerItem()->setText(6, tr("Action"));
    rideListSync->headerItem()->setText(7, tr("Status"));
    rideListSync->headerItem()->setText(8, tr("Workout Id"));
    rideListSync->setColumnCount(8);
    rideListSync->setSelectionMode(QAbstractItemView::SingleSelection);
    rideListSync->setEditTriggers(QAbstractItemView::SelectedClicked); // allow edit
    rideListSync->setUniformRowHeights(true);
    rideListSync->setIndentation(0);
    rideListSync->header()->resizeSection(0,20*dpiXFactor);
    rideListSync->header()->resizeSection(1,200*dpiXFactor);
    rideListSync->header()->resizeSection(2,100*dpiXFactor);
    rideListSync->header()->resizeSection(3,100*dpiXFactor);
    rideListSync->header()->resizeSection(4,100*dpiXFactor);
    rideListSync->header()->resizeSection(5,70*dpiXFactor);
    rideListSync->header()->resizeSection(6,100*dpiXFactor);
    rideListSync->setSortingEnabled(true);

    syncLayout->addLayout(syncList);
    syncLayout->addWidget(rideListSync);

    // show progress
    progressBar = new QProgressBar(this);
    progressLabel = new QLabel(tr("Initial"), this);

    overwrite = new QCheckBox(tr("Overwrite existing files"), this);

    // layout the widget now...
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QHBoxLayout *topline = new QHBoxLayout;
    topline->addWidget(athleteCombo);
    topline->addStretch();
    topline->addWidget(fromLabel);
    topline->addWidget(from);
    topline->addWidget(toLabel);
    topline->addWidget(to);
    topline->addStretch();
    topline->addWidget(refreshButton);


    QHBoxLayout *botline = new QHBoxLayout;
    botline->addWidget(progressLabel);
    botline->addStretch();
    botline->addWidget(overwrite);
    botline->addWidget(cancelButton);
    botline->addWidget(downloadButton);

    mainLayout->addLayout(topline);
    mainLayout->addWidget(tabs);
    mainLayout->addWidget(progressBar);
    mainLayout->addLayout(botline);


    connect (cancelButton, SIGNAL(clicked()), this, SLOT(cancelClicked()));
    connect (refreshButton, SIGNAL(clicked()), this, SLOT(refreshClicked()));
    connect (selectAll, SIGNAL(stateChanged(int)), this, SLOT(selectAllChanged(int)));
    connect (selectAllUp, SIGNAL(stateChanged(int)), this, SLOT(selectAllUpChanged(int)));
    connect (selectAllSync, SIGNAL(stateChanged(int)), this, SLOT(selectAllSyncChanged(int)));
    connect (downloadButton, SIGNAL(clicked()), this, SLOT(downloadClicked()));
    connect (tabs, SIGNAL(currentChanged(int)), this, SLOT(tabChanged(int)));
    QWidget::show();

    // check for any unsaved rides - since synchronize takes data from the stored files,
    // any unstored changes would not be uploaded
    QList<RideItem*> dirtyList;
    foreach (RideItem *rideItem, context->athlete->rideCache->rides())
        if (rideItem->isDirty() == true)
            dirtyList.append(rideItem);

    if (dirtyList.count() > 0 ) {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Sync with ") + store->uiName());
        if (dirtyList.count() == 1) {
            msgBox.setText(tr("One of your activities has unsaved changes."));
        } else {
            msgBox.setText(tr("%1 of your activities have unsaved changes.").arg(dirtyList.count()));
        }
        msgBox.setDetailedText(tr("Changes in activities which are not saved, will not be synchronized. \n\n"
                                  "This may lead to inconsistencies between your local GoldenCheetah activities "
                                  "and the uploaded activities. We recommend to save the changed activities "
                                  "before proceeding."));
        msgBox.setStandardButtons(QMessageBox::SaveAll | QMessageBox::Ignore | QMessageBox::Cancel);
        msgBox.setIcon(QMessageBox::Question);
        int ret = msgBox.exec();
        switch (ret) {
        case QMessageBox::SaveAll: {
            context->notifyMetadataFlush();
            context->ride->notifyRideMetadataChanged();
            QString error;
            if (!context->athlete->rideCache->saveActivities(
                    dirtyList, error)) {
                QMessageBox::warning(
                    this, tr("Save Activity"), error);
                QMetaObject::invokeMethod(
                    this, "close", Qt::QueuedConnection);
                return;
            }
            break;
        }
        case QMessageBox::Ignore:
            // just proceed
            break;
        case QMessageBox::Cancel:
            QApplication::processEvents();
            QMetaObject::invokeMethod(this, "close", Qt::QueuedConnection);
            return;
        default:
            // should never be reached
            break;
        }

    }

    // refresh anyway
    refreshClicked();

}

void
CloudServiceSyncDialog::cancelClicked()
{
    reject();
}

void
CloudServiceSyncDialog::refreshClicked()
{
    double distanceFactor = GlobalContext::context()->useMetricUnits ? 1.0 : MILES_PER_KM;
    QString distanceUnits = GlobalContext::context()->useMetricUnits ? tr("km") : tr("mi");

    progressLabel->setText(tr(""));
    progressBar->setMinimum(0);
    progressBar->setMaximum(1);
    progressBar->setValue(0);

    // wipe out current
    foreach (QTreeWidgetItem *curr, rideListDown->invisibleRootItem()->takeChildren()) {
        QCheckBox *check = (QCheckBox*)rideListDown->itemWidget(curr, 0);
        QCheckBox *exists = (QCheckBox*)rideListDown->itemWidget(curr, 4);
        delete check;
        delete exists;
        delete curr;
    }
    foreach (QTreeWidgetItem *curr, rideListUp->invisibleRootItem()->takeChildren()) {
        QCheckBox *check = (QCheckBox*)rideListUp->itemWidget(curr, 0);
        QCheckBox *exists = (QCheckBox*)rideListUp->itemWidget(curr, 6);
        delete check;
        delete exists;
        delete curr;
    }
    foreach (QTreeWidgetItem *curr, rideListSync->invisibleRootItem()->takeChildren()) {
        QCheckBox *check = (QCheckBox*)rideListSync->itemWidget(curr, 0);
        delete check;
        delete curr;
    }

    // get a list of all rides in the home directory
    QStringList errors;
    workouts = store->readdir(store->home(), errors, from->dateTime(), to->dateTime());

    // clear current
    rideFiles.clear();

    Specification specification;
    specification.setDateRange(DateRange(from->date(), to->date()));
    foreach(RideItem *item, context->athlete->rideCache->rides()) {
        if (specification.pass(item))
            rideFiles << QFileInfo(item->fileName).baseName().mid(0,14);
    }

    //
    // Setup the Download list
    //
    QChar zero = QLatin1Char('0');
    uploadFiles.clear();
    for(int i=0; i<workouts.count(); i++) {

        QDateTime ridedatetime;

        // skip files that aren't ride files
        if (!RideFile::parseRideFileName(workouts[i]->name, &ridedatetime)) continue;

        // skip files that aren't in range
        if (ridedatetime.date() < from->date() || ridedatetime.date() > to->date()) continue;

        QTreeWidgetItem *add;

        add = new QTreeWidgetItem(rideListDown->invisibleRootItem());
        add->setFlags(add->flags() & ~Qt::ItemIsEditable);

        QCheckBox *check = new QCheckBox("", this);
        connect (check, SIGNAL(stateChanged(int)), this, SLOT(refreshCount()));
        rideListDown->setItemWidget(add, 0, check);

        add->setText(1, workouts[i]->name);
        add->setTextAlignment(1, Qt::AlignCenter);

        add->setText(2, ridedatetime.toString(tr("MMM d, yyyy")));
        add->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter);
        add->setText(3, ridedatetime.toString("hh:mm:ss"));
        add->setTextAlignment(3, Qt::AlignCenter);

        QString targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                           .arg ( ridedatetime.date().year(), 4, 10, zero )
                           .arg ( ridedatetime.date().month(), 2, 10, zero )
                           .arg ( ridedatetime.date().day(), 2, 10, zero )
                           .arg ( ridedatetime.time().hour(), 2, 10, zero )
                           .arg ( ridedatetime.time().minute(), 2, 10, zero )
                           .arg ( ridedatetime.time().second(), 2, 10, zero );

        // if the filestore uses enddate we need to compare date the ride finished
        // rather than date the ride started!

        if (store->useEndDate) {

            // this is fucking painful, we need to look at every ride we have
            // and add on the duration - if it ends at the same time as this
            // then adjust the target no suffix to the start time
            foreach(RideItem *item, context->athlete->rideCache->rides()) {

                QDateTime end = item->dateTime.addSecs(item->getForSymbol("workout_time"));
                long diff = end.toMSecsSinceEpoch() - ridedatetime.toMSecsSinceEpoch();

                // account for rounding so +/- 2 seconds is close enough
                if (diff < 2000 && diff > -2000) {

                    targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                           .arg ( item->dateTime.date().year(), 4, 10, zero )
                           .arg ( item->dateTime.date().month(), 2, 10, zero )
                           .arg ( item->dateTime.date().day(), 2, 10, zero )
                           .arg ( item->dateTime.time().hour(), 2, 10, zero )
                           .arg ( item->dateTime.time().minute(), 2, 10, zero )
                           .arg ( item->dateTime.time().second(), 2, 10, zero );
                     break;
                 }
             }
        }
        uploadFiles << targetnosuffix.mid(0,14);

        // exists? - we ignore seconds, since TP seems to do odd
        //           things to date times and loses seconds (?)
        QCheckBox *exists = new QCheckBox("", this);
        exists->setEnabled(false);
        rideListDown->setItemWidget(add, 4, exists);
        add->setTextAlignment(4, Qt::AlignCenter);
        add->setText(6, workouts[i]->id); // download_id

        if (rideFiles.contains(targetnosuffix.mid(0,14))) exists->setChecked(true);
        else {
            exists->setChecked(Qt::Unchecked);

            // doesn't exist -- add it to the sync list too then
            QTreeWidgetItem *sync = new QTreeWidgetItem(rideListSync->invisibleRootItem());

            QCheckBox *check = new QCheckBox("", this);
            connect (check, SIGNAL(stateChanged(int)), this, SLOT(refreshSyncCount()));
            rideListSync->setItemWidget(sync, 0, check);

            sync->setText(1, workouts[i]->name);
            sync->setTextAlignment(1, Qt::AlignCenter);
            sync->setText(2, ridedatetime.toString(tr("MMM d, yyyy")));
            sync->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter);
            sync->setText(3, ridedatetime.toString("hh:mm:ss"));
            sync->setTextAlignment(3, Qt::AlignCenter);

            if (store->useMetric) { // Only for Today's Plan
                long secs = workouts[i]->duration;
                QChar zero = QLatin1Char ( '0' );
                QString duration = QString("%1:%2:%3").arg(secs/3600,2,10,zero)
                                                  .arg(secs%3600/60,2,10,zero)
                                                  .arg(secs%60,2,10,zero);
                sync->setText(4, duration);
                sync->setTextAlignment(4, Qt::AlignCenter);

                double distance = workouts[i]->distance;
                sync->setText(5, QString("%1 %2").arg(distance*distanceFactor, 0, 'f', 1).arg(distanceUnits));
                sync->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
            }
            sync->setText(6, tr("Download"));
            sync->setTextAlignment(6, Qt::AlignLeft | Qt::AlignVCenter);
            sync->setText(7, "");

            sync->setText(8, workouts[i]->id); // download_id
        }
    }

    //
    // Now setup the upload list
    //
    bool uploadEnabled = (store->capabilities() & CloudService::Upload);
    for(int i=0; uploadEnabled && i<context->athlete->rideCache->rides().count(); i++) {

        RideItem *ride = context->athlete->rideCache->rides().at(i);
        if (!specification.pass(ride) || ride->planned) continue;

        QTreeWidgetItem *add;

        add = new QTreeWidgetItem(rideListUp->invisibleRootItem());
        add->setFlags(add->flags() & ~Qt::ItemIsEditable);

        QCheckBox *check = new QCheckBox("", this);
        connect (check, SIGNAL(stateChanged(int)), this, SLOT(refreshUpCount()));
        rideListUp->setItemWidget(add, 0, check);

        add->setText(1, ride->fileName);
        add->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        add->setText(2, ride->dateTime.toString(tr("MMM d, yyyy")));
        add->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter);
        add->setText(3, ride->dateTime.toString("hh:mm:ss"));
        add->setTextAlignment(3, Qt::AlignCenter);

        long secs = ride->getForSymbol("workout_time");
        QChar zero = QLatin1Char ( '0' );
        QString duration = QString("%1:%2:%3").arg(secs/3600,2,10,zero)
                                          .arg(secs%3600/60,2,10,zero)
                                          .arg(secs%60,2,10,zero);
        add->setText(4, duration);
        add->setTextAlignment(4, Qt::AlignCenter);

        double distance = ride->getForSymbol("total_distance");
        add->setText(5, QString("%1 %2").arg(distance*distanceFactor, 0, 'f', 1).arg(distanceUnits));
        add->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);

        // exists? - we ignore seconds, since TP seems to do odd
        //           things to date times and loses seconds (?)
        QCheckBox *exists = new QCheckBox("", this);
        exists->setEnabled(false);
        rideListUp->setItemWidget(add, 6, exists);
        add->setTextAlignment(6, Qt::AlignCenter);

        QString targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                           .arg ( ride->dateTime.date().year(), 4, 10, zero )
                           .arg ( ride->dateTime.date().month(), 2, 10, zero )
                           .arg ( ride->dateTime.date().day(), 2, 10, zero )
                           .arg ( ride->dateTime.time().hour(), 2, 10, zero )
                           .arg ( ride->dateTime.time().minute(), 2, 10, zero )
                           .arg ( ride->dateTime.time().second(), 2, 10, zero );

        // check if on <CloudService> already
        if (uploadFiles.contains(targetnosuffix.mid(0,14))) exists->setChecked(true);
        else {
            exists->setChecked(Qt::Unchecked);

            // doesn't exist -- add it to the sync list too then
            QTreeWidgetItem *sync = new QTreeWidgetItem(rideListSync->invisibleRootItem());

            QCheckBox *check = new QCheckBox("", this);
            connect (check, SIGNAL(stateChanged(int)), this, SLOT(refreshSyncCount()));
            rideListSync->setItemWidget(sync, 0, check);

            sync->setText(1, ride->fileName);
            sync->setTextAlignment(1, Qt::AlignCenter);
            sync->setText(2, ride->dateTime.toString(tr("MMM d, yyyy")));
            sync->setTextAlignment(2, Qt::AlignLeft | Qt::AlignVCenter );
            sync->setText(3, ride->dateTime.toString("hh:mm:ss"));
            sync->setTextAlignment(3, Qt::AlignCenter);
            sync->setText(4, duration);
            sync->setTextAlignment(4, Qt::AlignCenter);
            sync->setText(5, QString("%1 %2").arg(distance*distanceFactor, 0, 'f', 1).arg(distanceUnits));
            sync->setTextAlignment(5, Qt::AlignRight | Qt::AlignVCenter);
            sync->setText(6, tr("Upload"));
            sync->setTextAlignment(6, Qt::AlignLeft | Qt::AlignVCenter);
            sync->setText(7, "");
        }
        add->setText(7, "");
    }

    // adjust column widths to content text length
    for (int j=0; j<rideListUp->columnCount(); j++) {
        rideListUp->resizeColumnToContents(j);
    }
    for (int j=0; j<rideListSync->columnCount(); j++) {
        rideListSync->resizeColumnToContents(j);
    }
    for (int j=0; j<rideListDown->columnCount(); j++) {
        rideListDown->resizeColumnToContents(j);
    }

    // refresh the progress label
    tabChanged(tabs->currentIndex());
}

void
CloudServiceSyncDialog::tabChanged(int idx)
{
    if (downloadButton->text() == tr("Abort")) return;

    switch (idx) {

    case 0 : // download
        downloadButton->setText(tr("Download"));
        refreshCount();
        break;
    case 1 : // upload
        downloadButton->setText(tr("Upload"));
        refreshUpCount();
        break;
    case 2 : // synchronise
        downloadButton->setText(tr("Synchronize"));
        refreshSyncCount();
        break;
    }
}


void
CloudServiceSyncDialog::selectAllChanged(int state)
{
    for (int i=0; i<rideListDown->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListDown->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListDown->itemWidget(curr, 0);
        check->setChecked(state);
    }
}

void
CloudServiceSyncDialog::selectAllUpChanged(int state)
{
    for (int i=0; i<rideListUp->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListUp->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListUp->itemWidget(curr, 0);
        check->setChecked(state);
    }
}

void
CloudServiceSyncDialog::selectAllSyncChanged(int state)
{
    for (int i=0; i<rideListSync->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListSync->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListSync->itemWidget(curr, 0);
        check->setChecked(state);
    }
}

void
CloudServiceSyncDialog::refreshUpCount()
{
    int selected = 0;

    for (int i=0; i<rideListUp->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListUp->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListUp->itemWidget(curr, 0);
        if (check->isChecked()) selected++;
    }
    progressLabel->setText(QString(tr("%1 of %2 selected")).arg(selected)
                            .arg(rideListUp->invisibleRootItem()->childCount()));
}

void
CloudServiceSyncDialog::refreshSyncCount()
{
    int selected = 0;

    for (int i=0; i<rideListSync->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListSync->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListSync->itemWidget(curr, 0);
        if (check->isChecked()) selected++;
    }
    progressLabel->setText(QString(tr("%1 of %2 selected")).arg(selected)
                            .arg(rideListSync->invisibleRootItem()->childCount()));
}

void
CloudServiceSyncDialog::refreshCount()
{
    int selected = 0;

    for (int i=0; i<rideListDown->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListDown->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListDown->itemWidget(curr, 0);
        if (check->isChecked()) selected++;
    }
    progressLabel->setText(QString(tr("%1 of %2 selected")).arg(selected)
                            .arg(rideListDown->invisibleRootItem()->childCount()));
}

void
CloudServiceSyncDialog::downloadClicked()
{
    if (downloading == true) {
        rideListDown->setSortingEnabled(true);
        rideListUp->setSortingEnabled(true);
        progressLabel->setText("");
        downloadButton->setText(tr("Download"));
        downloading=false;
        aborted=true;
        cancelButton->show();
        return;
    } else {
        rideListDown->setSortingEnabled(false);
        rideListUp->setSortingEnabled(true);
        downloading=true;
        aborted=false;
        downloadButton->setText(tr("Abort"));
        cancelButton->hide();
    }

    // keeping track of progress...
    downloadcounter = 0;
    successful = 0;
    downloadtotal = 0;
    listindex = 0;

    QTreeWidget *which = NULL;
    switch(tabs->currentIndex()) {
        case 0 : which = rideListDown; break;
        case 1 : which = rideListUp; break;
        default:
        case 2 : which = rideListSync; break;
    }

    for (int i=0; i<which->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = which->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)which->itemWidget(curr, 0);
        if (check->isChecked()) {
            downloadtotal++;
        }
    }

    if (downloadtotal) {
        progressBar->setMaximum(downloadtotal);
        progressBar->setMinimum(0);
        progressBar->setValue(0);
    }

    // even if nothing to download this
    // cleans up variables et al
    sync = false;
    switch(tabs->currentIndex()) {
        case 0 : downloadNext(); break;
        case 1 : uploadNext(); break;
        case 2 : sync = true; syncNext(); break;
    }
}

bool
CloudServiceSyncDialog::syncNext()
{
    // the actual download/upload is kicked off using the uploader / downloader
    // if in sync mode the completedRead / completedWrite functions
    // just call completedSync to get the next Sync done
    for (int i=listindex; i<rideListSync->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListSync->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListSync->itemWidget(curr, 0);

        if (check->isChecked()) {

            listindex = i+1; // start from the next one

            progressLabel->setText(QString(tr("Processed %1 of %2")).arg(downloadcounter).arg(downloadtotal));
            if (curr->text(6) == tr("Download")) {
                curr->setText(7, tr("Downloading"));
                rideListSync->setCurrentItem(curr);

                QByteArray *data = new QByteArray;
                store->readFile(data, curr->text(1), curr->text(8)); // filename
                QApplication::processEvents();

            } else {
                curr->setText(7, tr("Uploading"));
                rideListSync->setCurrentItem(curr);

                // read in the file
                QStringList errors;
                QFile file(context->athlete->home->activities().canonicalPath() + "/" + curr->text(1));
                RideFile *ride = RideFileFactory::instance().openRideFile(context, file, errors);

                if (ride) {

                    // get a compressed version
                    QByteArray data;
                    store->compressRide(ride, data, QFileInfo(curr->text(1)).baseName() + ".json");

                    store->writeFile(data, QFileInfo(curr->text(1)).baseName() + store->uploadExtension(), ride);
                    QApplication::processEvents();
                    delete ride; // clean up!
                    return true;

                } else {
                    curr->setText(7, tr("Parse failure"));
                    QApplication::processEvents();
                }

            }
            return true;
        }
    }

    //
    // Our work is done!
    //
    rideListDown->setSortingEnabled(true);
    rideListUp->setSortingEnabled(true);
    rideListSync->setSortingEnabled(true);
    progressLabel->setText(tr("Sync complete"));
    downloadButton->setText(tr("Synchronize"));
    downloading=false;
    aborted=false;
    sync=false;
    cancelButton->show();
    selectAllSync->setChecked(Qt::Unchecked);
    for (int i=0; i<rideListSync->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListSync->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListSync->itemWidget(curr, 0);
        check->setChecked(false);
    }
    progressLabel->setText(QString(tr("Processed %1 of %2 successfully")).arg(successful).arg(downloadtotal));

    // save the ride cache, we don't want to lose that if we crash etc.
    context->athlete->rideCache->save();

    return false;
}

bool
CloudServiceSyncDialog::downloadNext()
{
    for (int i=listindex; i<rideListDown->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListDown->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListDown->itemWidget(curr, 0);
        QCheckBox *exists = (QCheckBox*)rideListDown->itemWidget(curr, 4);

        // skip existing if overwrite not set
        if (check->isChecked() && exists->isChecked() && !overwrite->isChecked()) {
            curr->setText(5, tr("File exists"));
            progressBar->setValue(++downloadcounter);
            continue;
        }

        if (check->isChecked()) {

            listindex = i+1; // start from the next one
            curr->setText(5, tr("Downloading"));
            rideListDown->setCurrentItem(curr);
            progressLabel->setText(QString(tr("Downloaded %1 of %2")).arg(downloadcounter).arg(downloadtotal));

            QByteArray *data = new QByteArray; // gets deleted when read completes
            store->readFile(data, curr->text(1), curr->text(6));
            QApplication::processEvents();
            //delete data;
            return true;
        }
    }

    //
    // Our work is done!
    //
    rideListDown->setSortingEnabled(true);
    rideListUp->setSortingEnabled(true);
    progressLabel->setText(tr("Downloads complete"));
    downloadButton->setText(tr("Download"));
    downloading=false;
    aborted=false;
    cancelButton->show();
    selectAll->setChecked(Qt::Unchecked);
    for (int i=0; i<rideListDown->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListDown->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListDown->itemWidget(curr, 0);
        check->setChecked(false);
    }
    progressLabel->setText(QString(tr("Downloaded %1 of %2 successfully")).arg(successful).arg(downloadtotal));

    // save the ride cache, we don't want to lose that if we crash etc.
    context->athlete->rideCache->save();

    return false;
}

void
CloudServiceSyncDialog::completedRead(QByteArray *data, QString name, QString /*message*/)
{
    QTreeWidget *which = sync ? rideListSync : rideListDown;
    int col = sync ? 7 : 5;

    // was abort pressed?
    if (aborted == true) {
        QTreeWidgetItem *curr = which->invisibleRootItem()->child(listindex-1);
        curr->setText(col, tr("Aborted"));
        return;
    }

    // uncompress and parse, note the filename is passed and may be
    // different to what we asked for (sometimes the data is converted
    // from one file format to another).
    QStringList errors;
    RideFile *ride = store->uncompressRide(data, name, errors);

    // was allocated in before calling readfile
    delete data;

    progressBar->setValue(++downloadcounter);

    QTreeWidgetItem *curr = which->invisibleRootItem()->child(listindex-1);
    if (ride) {
        if (saveRide(ride, errors) == true) {
            curr->setText(col, tr("Saved"));
            successful++;
        } else {
            curr->setText(col, errors.join(" "));
        }

        // delete once saved
        delete ride;
    } else {
        curr->setText(col, errors.join(" "));
    }

    QApplication::processEvents();

    if (sync)
        syncNext();
    else
        downloadNext();
}

bool
CloudServiceSyncDialog::uploadNext()
{
    for (int i=listindex; i<rideListUp->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListUp->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListUp->itemWidget(curr, 0);
        QCheckBox *exists = (QCheckBox*)rideListUp->itemWidget(curr, 6);

        // skip existing if overwrite not set
        if (check->isChecked() && exists->isChecked() && !overwrite->isChecked()) {
            curr->setText(7, tr("File exists"));
            progressBar->setValue(++downloadcounter);
            continue;
        }

        if (check->isChecked()) {

            listindex = i+1; // start from the next one
            curr->setText(7, tr("Uploading"));
            rideListUp->setCurrentItem(curr);
            progressLabel->setText(QString(tr("Uploaded %1 of %2")).arg(downloadcounter).arg(downloadtotal));

            // read in the file - TEMPORARILY *** WE DON'T USE IN MEMORY VERSION ***
            QStringList errors;
            QFile file(context->athlete->home->activities().canonicalPath() + "/" + curr->text(1));
            RideFile *ride = RideFileFactory::instance().openRideFile(context, file, errors);

            if (ride) {

                    // get a compressed version
                    QByteArray data;
                    store->compressRide(ride, data, QFileInfo(curr->text(1)).baseName() + ".json");
                    store->writeFile(data, QFileInfo(curr->text(1)).baseName() + store->uploadExtension(), ride);
                    QApplication::processEvents();
                    delete ride; // clean up!
                    return true;

            } else {
                curr->setText(7, tr("Parse failure"));
                QApplication::processEvents();
            }
        }
    }

    //
    // Our work is done!
    //
    rideListDown->setSortingEnabled(true);
    rideListUp->setSortingEnabled(true);
    progressLabel->setText(tr("Uploads complete"));
    downloadButton->setText(tr("Upload"));
    downloading=false;
    aborted=false;
    cancelButton->show();
    selectAllUp->setChecked(Qt::Unchecked);
    for (int i=0; i<rideListUp->invisibleRootItem()->childCount(); i++) {
        QTreeWidgetItem *curr = rideListUp->invisibleRootItem()->child(i);
        QCheckBox *check = (QCheckBox*)rideListUp->itemWidget(curr, 0);
        check->setChecked(false);
    }
    progressLabel->setText(QString(tr("Uploaded %1 of %2 successfully")).arg(successful).arg(downloadtotal));
    return false;
}

void
CloudServiceSyncDialog::completedWrite(QString, QString result)
{
    QTreeWidget *which = sync ? rideListSync : rideListUp;

    // was abort pressed?
    if (aborted == true) {
        QTreeWidgetItem *curr = which->invisibleRootItem()->child(listindex-1);
        curr->setText(7, tr("Aborted"));
        return;
    }

    progressBar->setValue(++downloadcounter);

    QTreeWidgetItem *curr = which->invisibleRootItem()->child(listindex-1);
    curr->setText(7, result);
    if (result == tr("Completed.")) successful++;
    QApplication::processEvents();

    if (sync)
        syncNext();
    else
        uploadNext();
}

bool
CloudServiceSyncDialog::saveRide(RideFile *ride, QStringList &errors)
{
    QDateTime ridedatetime = ride->startTime();

    QChar zero = QLatin1Char ( '0' );
    QString targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                           .arg ( ridedatetime.date().year(), 4, 10, zero )
                           .arg ( ridedatetime.date().month(), 2, 10, zero )
                           .arg ( ridedatetime.date().day(), 2, 10, zero )
                           .arg ( ridedatetime.time().hour(), 2, 10, zero )
                           .arg ( ridedatetime.time().minute(), 2, 10, zero )
                           .arg ( ridedatetime.time().second(), 2, 10, zero );

    QString filename = context->athlete->home->activities().canonicalPath() + "/" + targetnosuffix + ".json";

    // exists?
    QFileInfo fileinfo(filename);
    if (fileinfo.exists() && overwrite->isChecked() == false) {
        errors << tr("File exists");
        return false;
    }

    // process linked defaults
    GlobalContext::context()->rideMetadata->setLinkedDefaults(ride);

    // run the processor first... import
    DataProcessorFactory::instance().autoProcess(ride, "Auto", "Import");
    ride->recalculateDerivedSeries();
    // now metrics have been calculated
    DataProcessorFactory::instance().autoProcess(ride, "Save", "ADD");

    JsonFileReader reader;
    QFile file(filename);
    QString writeError;
    if (!publishActivityBeforeCacheUpdate(
            [&](QString &stepError) {
                return reader.writeRideFile(
                    context, ride, file, stepError,
                    overwrite->isChecked());
            },
            [&]() {
                rideFiles << targetnosuffix;
                context->athlete->addRide(fileinfo.fileName(), true);
            },
            writeError)) {
        errors << (writeError.isEmpty()
                       ? tr("Failed to save downloaded activity")
                       : writeError);
        return false;
    }

    return true;
}


//
// Upgrade settings now we have migrated to a cloud service factory
// and notion of setting up "accounts" etc
//
void
CloudServiceFactory::upgrade(QString name)
{
    foreach(QString servicename, CloudServiceFactory::instance().serviceNames()) {

        QString sname; // setting name
        bool active = false;

        const CloudService *s = CloudServiceFactory::instance().service(servicename);
        if (s == NULL) continue;

        // look at config and see if it has been configured
        // if it needs a user, pass or token make sure its there
        if ((sname=s->settings.value(CloudService::OAuthToken, "")) != "") {
            if (appsettings->cvalue(name, sname, "").toString() != "") active = true;
        }
        if ((sname=s->settings.value(CloudService::Username, "")) != "") {
            if (appsettings->cvalue(name, sname, "").toString() != "") active = true;
            else active = false;
        }
        if ((sname=s->settings.value(CloudService::Password, "")) != "") {
            if (appsettings->cvalue(name, sname, "").toString() != "") active = true;
            else active = false;
        }

        // so now we can set it
        appsettings->setCValue(name, s->activeSettingName(), active ? "true" : "false");
    }
}

void
CloudServiceFactory::saveSettings(
        CloudService *service,
        Context *context)
{
    if (!service) return;

    QString athlete =
            service->property("_gcAthleteName").toString();
    if (athlete.isEmpty()
        && context
        && QThread::currentThread() == context->thread()
        && Context::isValid(context)
        && context->athlete) {
        athlete = context->athlete->cyclist;
    }
    if (athlete.isEmpty()) return;

    const QVariant initialProperty =
            service->property("_gcInitialConfiguration");
    const bool hasInitialConfiguration = initialProperty.isValid();
    const QVariantMap initialConfiguration = initialProperty.toMap();
    auto changedFromInitial =
            [&initialConfiguration, hasInitialConfiguration](
                    const QString &key, const QString &value) {
        return !hasInitialConfiguration
            || !initialConfiguration.contains(key)
            || initialConfiguration.value(key).toString() != value;
    };

    QHash<QString, QString> updates;
    QString clearedUrlKey;
    QString clearedUrlDefault;
    QHashIterator<CloudService::CloudServiceSetting, QString> wanted(
        service->settings);
    while (wanted.hasNext()) {
        wanted.next();
        const QString key = wanted.value().contains(QStringLiteral("::"))
                ? wanted.value().split(QStringLiteral("::")).at(0)
                : wanted.value();
        const QString value =
                service->getSetting(key, QString()).toString();
        const bool clearsUrlToDefault =
                wanted.key() == CloudService::URL
                && value.isEmpty()
                && hasInitialConfiguration
                && initialConfiguration.contains(key)
                && !initialConfiguration.value(key)
                        .toString().isEmpty();
        if ((!value.isEmpty() || clearsUrlToDefault)
            && changedFromInitial(key, value)) {
            updates.insert(key, value);
            if (clearsUrlToDefault) {
                clearedUrlKey = key;
                clearedUrlDefault = service->settings.value(
                    CloudService::DefaultURL);
            }
        }
    }

    const QString syncStartupKey =
            service->syncOnStartupSettingName();
    const QString syncStartup =
            service->getSetting(syncStartupKey, QString()).toString();
    if (!syncStartup.isEmpty()
        && changedFromInitial(syncStartupKey, syncStartup)) {
        updates.insert(syncStartupKey, syncStartup);
    }

    const QString syncImportKey =
            service->syncOnImportSettingName();
    const QString syncImport =
            service->getSetting(syncImportKey, QString()).toString();
    if (!syncImport.isEmpty()
        && changedFromInitial(syncImportKey, syncImport)) {
        updates.insert(syncImportKey, syncImport);
    }

    if (updates.isEmpty()) return;

    QVariantMap nextConfiguration = initialConfiguration;
    for (auto update = updates.cbegin();
         update != updates.cend(); ++update) {
        nextConfiguration.insert(
            update.key(),
            update.key() == clearedUrlKey
                ? clearedUrlDefault
                : update.value());
    }
    QCoreApplication *application = QCoreApplication::instance();
    if (application
        && QThread::currentThread() != application->thread()) {
        QueuedCloudSettingsTransaction transaction;
        transaction.athlete = athlete.toUtf8().toStdString();
        transaction.source = service->id().toUtf8().toStdString();
        QSet<QString> expectedKeys;
        for (auto setting = service->settings.cbegin();
             setting != service->settings.cend(); ++setting) {
            if (setting.key() == CloudService::DefaultURL) continue;
            const QString key =
                    setting.value().contains(QStringLiteral("::"))
                    ? setting.value().split(QStringLiteral("::")).at(0)
                    : setting.value();
            const QString defaultValue =
                    setting.key() == CloudService::URL
                    ? service->settings.value(CloudService::DefaultURL)
                    : QString();
            expectedKeys.insert(key);
            transaction.expectations.push_back({
                key.toUtf8().toStdString(),
                initialConfiguration.value(key)
                    .toString().toUtf8().toStdString(),
                defaultValue.toUtf8().toStdString(),
                key.startsWith(
                    QStringLiteral("<athlete"))});
        }
        for (auto update = updates.cbegin();
             update != updates.cend(); ++update) {
            transaction.updates.push_back({
                update.key().toUtf8().toStdString(),
                update.value().toUtf8().toStdString(),
                update.key().startsWith(
                    QStringLiteral("<athlete"))});
            if (!expectedKeys.contains(update.key())) {
                const QString defaultValue =
                        update.key() == syncStartupKey
                        || update.key() == syncImportKey
                        ? QStringLiteral("false")
                        : QString();
                transaction.expectations.push_back({
                    update.key().toUtf8().toStdString(),
                    initialConfiguration.value(update.key())
                        .toString().toUtf8().toStdString(),
                    defaultValue.toUtf8().toStdString(),
                    update.key().startsWith(
                        QStringLiteral("<athlete"))});
            }
        }
        if (!enqueueCloudSettingsTransaction(
                application, std::move(transaction))) {
            qWarning() << "Cloud settings handoff queue is full;"
                       << "discarding a provider update";
            return;
        }
        if (!clearedUrlKey.isEmpty())
            service->setSetting(clearedUrlKey, clearedUrlDefault);
        service->setProperty(
            "_gcInitialConfiguration", nextConfiguration);
        return;
    }

    for (auto update = updates.cbegin();
         update != updates.cend(); ++update) {
        if (update.key().startsWith(QStringLiteral("<athlete")))
            appsettings->setCValue(
                athlete, update.key(), update.value());
        else
            appsettings->setValue(update.key(), update.value());
#ifdef GC_WANT_ALLDEBUG
        qDebug() << "factory save setting:"
                 << update.key() << update.value();
#endif
    }
    if (!clearedUrlKey.isEmpty())
        service->setSetting(clearedUrlKey, clearedUrlDefault);
    service->setProperty(
        "_gcInitialConfiguration", nextConfiguration);
}

//
// Auto download
//
namespace {

struct CloudAutoDownloadPlan
{
    QList<CloudService *> providers;
    QSet<QString> rideFiles;
    QDateTime now;
    int requestTimeoutMs = 30000;
    quint64 generation = 0;
};

} // namespace

class CloudServiceAutoDownloadWorker final : public QObject
{
public:
    CloudServiceAutoDownloadWorker(
            CloudServiceAutoDownload *owner,
            CloudAutoDownloadPlan plan)
        : owner(owner),
          providers(std::move(plan.providers)),
          rideFiles(std::move(plan.rideFiles)),
          now(std::move(plan.now)),
          requestTimeoutMs(plan.requestTimeoutMs),
          generation(plan.generation),
          timeoutTimer(this),
          interruptionTimer(this),
          nextRequestTimer(this)
    {
        timeoutTimer.setSingleShot(true);
        connect(
            &timeoutTimer, &QTimer::timeout, this,
            [this]() { requestTimedOut(); });

        interruptionTimer.setInterval(10);
        connect(
            &interruptionTimer, &QTimer::timeout, this,
            [this]() {
                if (isCancelled()) cancel();
            });

        nextRequestTimer.setSingleShot(true);
        connect(
            &nextRequestTimer, &QTimer::timeout, this,
            [this]() { startNextRequest(); });
    }

    ~CloudServiceAutoDownloadWorker() override
    {
        cleanup();
    }

    bool needsEventLoop() const
    {
        return running;
    }

    void begin()
    {
        if (running) return;
        running = true;
        interruptionTimer.start();

        if (isCancelled()) {
            finish(true);
            return;
        }

        const QDate firstDate = now.addDays(-30).date();
        const QDate lastDate = now.date();

        for (int providerIndex = 0;
             providerIndex < providers.size();
             ++providerIndex) {
            if (isCancelled()) {
                finish(true);
                return;
            }

            CloudService *service = providers.at(providerIndex);
            if (!service) continue;

            connect(
                service, &CloudService::readComplete, this,
                [this](
                        QByteArray *data,
                        const QString &name,
                        const QString &message) {
                    readCompleted(data, name, message);
                });

            QStringList errors;
            beginProviderCall(service);
            const bool opened = service->open(errors);
            endProviderCall(service);

            if (isCancelled()) {
                finish(true);
                return;
            }
            if (!opened) {
                deleteProvider(providerIndex);
                continue;
            }
            openedProviders.insert(service);

            beginProviderCall(service);
            const QString home = service->home();
            endProviderCall(service);
            if (isCancelled()) {
                finish(true);
                return;
            }

            beginProviderCall(service);
            const QList<CloudServiceEntry *> found =
                    service->readdir(
                        home, errors, now.addDays(-30), now);
            endProviderCall(service);
            if (isCancelled()) {
                finish(true);
                return;
            }

            bool needed = false;
            for (CloudServiceEntry *entry : found) {
                QDateTime rideDateTime;
                if (!RideFile::parseRideFileName(
                        entry->name, &rideDateTime)) {
                    continue;
                }
                if (rideDateTime.date() < firstDate
                    || rideDateTime.date() > lastDate) {
                    continue;
                }
                if (rideFiles.contains(entry->name.left(16))) continue;

                needed = true;
                requests.push_back(
                    std::make_unique<DownloadRequest>(entry, service));
            }

            if (!needed) {
                deleteProvider(providerIndex);
            }
        }

        startNextRequest();
    }

    void cancel()
    {
        cancelled = true;
        if (providerCallActive) {
            abortActiveProvider();
            return;
        }
        if (running) finish(true);
        else QThread::currentThread()->quit();
    }

private:
    struct DownloadRequest
    {
        DownloadRequest(
                CloudServiceEntry *entry,
                CloudService *provider)
            : entry(entry), provider(provider)
        {
        }

        CloudServiceEntry *entry = nullptr;
        CloudService *provider = nullptr;
        std::unique_ptr<QByteArray> data;
        bool delivered = false;
    };

    bool isCancelled() const
    {
        return cancelled
            || QThread::currentThread()->isInterruptionRequested();
    }

    void beginProviderCall(CloudService *service)
    {
        Q_ASSERT(!providerCallActive);
        providerCallActive = service;
    }

    void endProviderCall(CloudService *service)
    {
        Q_ASSERT(providerCallActive == service);
        providerCallActive = nullptr;
        if (running
            && advanceQueued
            && !nextRequestTimer.isActive()) {
            nextRequestTimer.start(0);
        }
    }

    void abortProvider(CloudService *service)
    {
        if (!service || abortingProvider) return;

        abortingProvider = true;
        service->abortRequests();
        abortingProvider = false;
    }

    void abortActiveProvider()
    {
        abortProvider(providerCallActive);
    }

    void quiesceProviders(bool abort)
    {
        for (CloudService *service : providers) {
            if (!service) continue;
            disconnect(service, nullptr, this, nullptr);
            if (abort) abortProvider(service);
        }
    }

    void startNextRequest()
    {
        if (providerCallActive) {
            advanceQueued = true;
            return;
        }
        advanceQueued = false;
        if (!running) return;
        if (isCancelled()) {
            finish(true);
            return;
        }

        const int total = int(requests.size());
        while (nextRequest < total
               && !requests.at(nextRequest)->provider) {
            ++nextRequest;
        }
        if (nextRequest >= total) {
            if (total > 0) {
                postProgress(lastServiceName, 100.0, total, total);
            }
            finish(false);
            return;
        }

        DownloadRequest *request = requests.at(nextRequest).get();
        const int current = nextRequest++;
        CloudService *provider = request->provider;
        beginProviderCall(provider);
        lastServiceName = provider->uiName();
        endProviderCall(provider);
        if (isCancelled()) {
            finish(true);
            return;
        }
        activeRequest = request;
        postProgress(
            lastServiceName,
            total > 0 ? (100.0 * double(current)) / double(total) : 0.0,
            current,
            total);

        request->data = std::make_unique<QByteArray>();
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        cloudAutoDownloadBufferAllocated(request->data.get());
#endif
        requestsByData.insert(request->data.get(), request);

        beginProviderCall(provider);
        const bool started = provider->readFile(
            request->data.get(), request->entry->name, request->entry->id);
        endProviderCall(provider);

        if (isCancelled()) {
            finish(true);
            return;
        }

        // Inline providers can complete before readFile returns.
        if (request->delivered || activeRequest != request) return;

        if (!started) {
            requestsByData.remove(request->data.get());
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            cloudAutoDownloadBufferReleased(request->data.get());
#endif
            request->data.reset();
            activeRequest = nullptr;
            queueNextRequest();
            return;
        }

        timeoutTimer.start(requestTimeoutMs);
    }

    void queueNextRequest()
    {
        if (!running || advanceQueued) return;
        advanceQueued = true;
        if (!providerCallActive)
            nextRequestTimer.start(0);
    }

    void requestTimedOut()
    {
        if (!running || !activeRequest) return;
        DownloadRequest *timedOutRequest = activeRequest;
        CloudService *timedOutProvider = timedOutRequest->provider;
        activeRequest = nullptr;
        retireProvider(timedOutProvider);
        queueNextRequest();
    }

    void readCompleted(
            QByteArray *data,
            const QString &name,
            const QString &message)
    {
        Q_UNUSED(message)
        if (!running || isCancelled()) return;

        const auto requestIt = requestsByData.constFind(data);
        if (requestIt == requestsByData.cend()) {
            qDebug() << "Autodownload: received file has no download entry";
            return;
        }

        DownloadRequest *request = requestIt.value();
        if (request->delivered) return;
        request->delivered = true;
        const CloudService::CompressionType compression =
                request->provider->downloadCompression;
        QMap<QString, QString> expectedSettings;
        QMap<QString, QString> settingDefaults;
        providerSettingsSnapshot(
            request->provider, expectedSettings, settingDefaults);
        QByteArray payload = std::move(*data);
        postDownloaded(
            std::move(payload), name, compression,
            std::move(expectedSettings),
            std::move(settingDefaults));

        if (activeRequest == request) {
            timeoutTimer.stop();
            activeRequest = nullptr;
            queueNextRequest();
        }
    }

    void postDownloaded(
            QByteArray data,
            QString name,
            CloudService::CompressionType compression,
            QMap<QString, QString> expectedSettings,
            QMap<QString, QString> settingDefaults)
    {
        CloudServiceAutoDownload::QueuedEvent event;
        event.type = CloudServiceAutoDownload::QueuedEventType::Downloaded;
        event.generation = generation;
        event.data = std::move(data);
        event.text = std::move(name);
        event.compression = compression;
        event.expectedSettings = std::move(expectedSettings);
        event.settingDefaults = std::move(settingDefaults);
        owner->enqueueEvent(std::move(event));
    }

    void providerSettingsSnapshot(
            CloudService *service,
            QMap<QString, QString> &snapshot,
            QMap<QString, QString> &defaults) const
    {
        if (!service) return;

        for (auto setting = service->settings.cbegin();
             setting != service->settings.cend(); ++setting) {
            if (setting.key() == CloudService::DefaultURL) continue;
            const QString key =
                    setting.value().contains(QStringLiteral("::"))
                    ? setting.value().split(QStringLiteral("::")).at(0)
                    : setting.value();
            snapshot.insert(
                key, service->getSetting(key, QString()).toString());
            if (setting.key() == CloudService::URL) {
                defaults.insert(
                    key,
                    service->settings.value(CloudService::DefaultURL));
            }
        }
        const QString startupKey = service->syncOnStartupSettingName();
        snapshot.insert(
            startupKey,
            service->getSetting(
                startupKey, QStringLiteral("false")).toString());
        defaults.insert(startupKey, QStringLiteral("false"));
    }

    void postProgress(
            const QString &service,
            double progress,
            int current,
            int total)
    {
        CloudServiceAutoDownload::QueuedEvent event;
        event.type = CloudServiceAutoDownload::QueuedEventType::Progress;
        event.generation = generation;
        event.text = service;
        event.progress = progress;
        event.current = current;
        event.total = total;
        owner->enqueueEvent(std::move(event));
    }

    void finish(bool wasCancelled)
    {
        if (!running) return;
        running = false;
        timeoutTimer.stop();
        interruptionTimer.stop();
        nextRequestTimer.stop();
        activeRequest = nullptr;
        quiesceProviders(wasCancelled);
        QThread::currentThread()->quit();

        CloudServiceAutoDownload::QueuedEvent event;
        event.type = CloudServiceAutoDownload::QueuedEventType::Finished;
        event.generation = generation;
        event.cancelled = wasCancelled;
        owner->enqueueEvent(std::move(event));
    }

    void deleteProvider(int providerIndex)
    {
        CloudService *service = providers.at(providerIndex);
        if (!service) return;

        disconnect(service, nullptr, this, nullptr);
        abortProvider(service);
        beginProviderCall(service);
        if (openedProviders.remove(service)) service->close();
        endProviderCall(service);
        delete service;
        providers[providerIndex] = nullptr;
    }

    void retireProvider(CloudService *service)
    {
        if (!service || retiredProviders.contains(service)) return;

        retiredProviders.insert(service);
        disconnect(service, nullptr, this, nullptr);
        abortProvider(service);

        for (const std::unique_ptr<DownloadRequest> &request : requests) {
            if (request->provider != service) continue;
            request->provider = nullptr;
            if (request->data) {
                requestsByData.remove(request->data.get());
            }
        }
    }

    void cleanup()
    {
        if (cleaning) return;
        cleaning = true;
        timeoutTimer.stop();
        interruptionTimer.stop();
        nextRequestTimer.stop();
        activeRequest = nullptr;

        for (int i = 0; i < providers.size(); ++i) {
            deleteProvider(i);
        }
        providers.clear();
        openedProviders.clear();
        retiredProviders.clear();
        requestsByData.clear();

        for (const std::unique_ptr<DownloadRequest> &request : requests) {
            if (!request->data) continue;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            cloudAutoDownloadBufferReleased(request->data.get());
#endif
            request->data.reset();
        }
        requests.clear();
        nextRequest = 0;
        advanceQueued = false;
        cleaning = false;
    }

    CloudServiceAutoDownload *owner = nullptr;
    QList<CloudService *> providers;
    QSet<CloudService *> openedProviders;
    QSet<CloudService *> retiredProviders;
    QSet<QString> rideFiles;
    QDateTime now;
    int requestTimeoutMs = 30000;
    quint64 generation = 0;
    QTimer timeoutTimer;
    QTimer interruptionTimer;
    QTimer nextRequestTimer;
    std::vector<std::unique_ptr<DownloadRequest>> requests;
    QHash<QByteArray *, DownloadRequest *> requestsByData;
    DownloadRequest *activeRequest = nullptr;
    CloudService *providerCallActive = nullptr;
    int nextRequest = 0;
    QString lastServiceName;
    bool running = false;
    bool cancelled = false;
    bool advanceQueued = false;
    bool abortingProvider = false;
    bool cleaning = false;
};

CloudServiceAutoDownload::CloudServiceAutoDownload(Context *context)
    : context(context)
{
}

bool
CloudServiceAutoDownload::enqueueEvent(QueuedEvent event)
{
    constexpr int MaximumQueuedProgressEvents = 8;

    if (event.type == QueuedEventType::Downloaded) {
        qint64 bytes = qint64(sizeof(QueuedEvent));
        bytes = saturatedAdd(bytes, qint64(event.data.capacity()));
        const auto addString = [&bytes](const QString &value) {
            const qint64 maximum =
                std::numeric_limits<qint64>::max();
            const qint64 capacity = qint64(value.capacity());
            const qint64 stringBytes =
                capacity > maximum / qint64(sizeof(QChar))
                ? maximum
                : capacity * qint64(sizeof(QChar));
            bytes = saturatedAdd(bytes, stringBytes);
        };
        addString(event.text);
        const auto addSettings = [&bytes, &addString](
                const QMap<QString, QString> &settings) {
            for (auto setting = settings.cbegin();
                 setting != settings.cend(); ++setting) {
                bytes = saturatedAdd(
                    bytes, qint64(sizeof(QString) * 2));
                addString(setting.key());
                addString(setting.value());
            }
        };
        addSettings(event.expectedSettings);
        addSettings(event.settingDefaults);
        event.accountedBytes = bytes;

    }

    bool scheduleDispatch = false;
    {
        std::unique_lock<std::mutex> lock(queuedEventsMutex);
        const auto isCurrentGeneration = [&]() {
            return event.generation
                == generation.load(std::memory_order_acquire);
        };
        if (!isCurrentGeneration()) return false;

        if (event.type == QueuedEventType::Downloaded) {
            if (event.accountedBytes > maximumQueuedDownloadBytes) {
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                ++rejectedDownloadEvents;
#endif
                lock.unlock();
                qWarning() << "Autodownload: downloaded payload exceeds"
                           << "the GUI handoff byte limit";
                return false;
            }
            bool waited = false;
            while (isCurrentGeneration()
                   && (queuedDownloadEvents
                           >= maximumQueuedDownloadEvents
                       || queuedDownloadBytes
                           > maximumQueuedDownloadBytes
                               - event.accountedBytes)) {
                if (!waited) {
                    waited = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                    ++queuedEventProducerWaits;
#endif
                }
                queuedEventSpace.wait(lock);
            }
            if (!isCurrentGeneration()) return false;
            ++queuedDownloadEvents;
            queuedDownloadBytes += event.accountedBytes;
        } else if (event.type == QueuedEventType::Progress) {
            int queuedProgressEvents = 0;
            auto newestProgress = queuedEvents.end();
            for (auto queued = queuedEvents.begin();
                 queued != queuedEvents.end(); ++queued) {
                if (queued->type == QueuedEventType::Progress
                    && queued->generation == event.generation) {
                    ++queuedProgressEvents;
                    newestProgress = queued;
                }
            }
            if (queuedProgressEvents >= MaximumQueuedProgressEvents) {
                *newestProgress = std::move(event);
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
                ++coalescedProgressEvents;
                updateQueuedEventPeaksLocked();
#endif
                return true;
            }
        }

        queuedEvents.push_back(std::move(event));
        if (!queuedEventDispatchPending) {
            queuedEventDispatchPending = true;
            scheduleDispatch = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
            ++queuedEventDispatchPosts;
#endif
        }
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateQueuedEventPeaksLocked();
#endif
    }

    if (scheduleDispatch) {
        QMetaObject::invokeMethod(
            this, "dispatchQueuedEvents", Qt::QueuedConnection);
    }
    return true;
}

quint64
CloudServiceAutoDownload::advanceGenerationAndDiscardQueuedEvents()
{
    quint64 nextGeneration = 0;
    {
        const std::lock_guard<std::mutex> lock(queuedEventsMutex);
        nextGeneration = generation.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        discardQueuedEventsLocked();
    }
    queuedEventSpace.notify_all();
    return nextGeneration;
}

void
CloudServiceAutoDownload::discardQueuedEventsLocked()
{
    for (const QueuedEvent &event : queuedEvents) {
        if (event.type != QueuedEventType::Downloaded) continue;
        --queuedDownloadEvents;
        queuedDownloadBytes -= event.accountedBytes;
    }
    queuedEvents.clear();
    queuedDownloadEvents = std::max(0, queuedDownloadEvents);
    queuedDownloadBytes = std::max(qint64(0), queuedDownloadBytes);
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    updateQueuedEventPeaksLocked();
#endif
}

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
void
CloudServiceAutoDownload::updateQueuedEventPeaksLocked()
{
    int queuedProgressEvents = 0;
    for (const QueuedEvent &event : queuedEvents) {
        if (event.type == QueuedEventType::Progress)
            ++queuedProgressEvents;
    }
    peakQueuedEvents = std::max(
        peakQueuedEvents, int(queuedEvents.size()));
    peakQueuedDownloadEvents = std::max(
        peakQueuedDownloadEvents, queuedDownloadEvents);
    peakQueuedProgressEvents = std::max(
        peakQueuedProgressEvents, queuedProgressEvents);
    peakQueuedDownloadBytes = std::max(
        peakQueuedDownloadBytes, queuedDownloadBytes);
}

CloudAutoDownloadQueueStats
CloudServiceAutoDownload::queuedEventStatsForTest()
{
    constexpr int MaximumQueuedProgressEvents = 8;
    const std::lock_guard<std::mutex> lock(queuedEventsMutex);
    int queuedProgressEvents = 0;
    for (const QueuedEvent &event : queuedEvents) {
        if (event.type == QueuedEventType::Progress)
            ++queuedProgressEvents;
    }
    updateQueuedEventPeaksLocked();

    CloudAutoDownloadQueueStats stats;
    stats.currentEvents = int(queuedEvents.size());
    stats.peakEvents = peakQueuedEvents;
    stats.currentDownloadEvents = queuedDownloadEvents;
    stats.peakDownloadEvents = peakQueuedDownloadEvents;
    stats.maximumDownloadEvents = maximumQueuedDownloadEvents;
    stats.currentProgressEvents = queuedProgressEvents;
    stats.peakProgressEvents = peakQueuedProgressEvents;
    stats.maximumProgressEvents = MaximumQueuedProgressEvents;
    stats.currentDownloadBytes = queuedDownloadBytes;
    stats.peakDownloadBytes = peakQueuedDownloadBytes;
    stats.maximumDownloadBytes = maximumQueuedDownloadBytes;
    stats.dispatchPosts = queuedEventDispatchPosts;
    stats.dispatchCalls = queuedEventDispatchCalls;
    stats.coalescedProgress = coalescedProgressEvents;
    stats.producerWaits = queuedEventProducerWaits;
    stats.rejectedDownloads = rejectedDownloadEvents;
    return stats;
}
#endif

void
CloudServiceAutoDownload::dispatchQueuedEvents()
{
    QueuedEvent event;
    {
        const std::lock_guard<std::mutex> lock(queuedEventsMutex);
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        ++queuedEventDispatchCalls;
#endif
        if (queuedEvents.empty()) {
            queuedEventDispatchPending = false;
            return;
        }
        event = std::move(queuedEvents.front());
        queuedEvents.pop_front();
    }

    QPointer<CloudServiceAutoDownload> guard(this);
    switch (event.type) {
    case QueuedEventType::Downloaded:
        downloaded(
            event.generation, std::move(event.data),
            std::move(event.text), event.compression,
            std::move(event.expectedSettings),
            std::move(event.settingDefaults));
        break;
    case QueuedEventType::Progress:
        downloadProgress(
            event.generation, std::move(event.text),
            event.progress, event.current, event.total);
        break;
    case QueuedEventType::Finished:
        downloadFinished(event.generation, event.cancelled);
        break;
    }
    if (!guard) return;

    bool scheduleNext = false;
    bool releasedDownloadSpace = false;
    {
        const std::lock_guard<std::mutex> lock(queuedEventsMutex);
        if (event.type == QueuedEventType::Downloaded) {
            --queuedDownloadEvents;
            queuedDownloadBytes -= event.accountedBytes;
            queuedDownloadEvents = std::max(
                0, queuedDownloadEvents);
            queuedDownloadBytes = std::max(
                qint64(0), queuedDownloadBytes);
            releasedDownloadSpace = true;
        }
        if (queuedEvents.empty())
            queuedEventDispatchPending = false;
        else
            scheduleNext = true;
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
        updateQueuedEventPeaksLocked();
#endif
    }
    if (releasedDownloadSpace) queuedEventSpace.notify_all();
    if (scheduleNext) {
        QMetaObject::invokeMethod(
            this, "dispatchQueuedEvents", Qt::QueuedConnection);
    }
}

void
CloudServiceAutoDownload::autoDownload()
{
    if (!initial) return;
    initial = false;
    startDownload();
}

void
CloudServiceAutoDownload::checkDownload()
{
    startDownload();
}

CloudServiceAutoDownload::~CloudServiceAutoDownload()
{
    cancelAndWait();
}

void
CloudServiceAutoDownload::cancelAndWait()
{
    advanceGenerationAndDiscardQueuedEvents();
    downloadActive = false;

    if (!isRunning()) {
        if (QThread::currentThread() != this) wait();
        worker = nullptr;
        QCoreApplication *application = QCoreApplication::instance();
        if (application
            && QThread::currentThread() == application->thread()) {
            applyQueuedCloudSettings();
        }
        return;
    }

    requestInterruption();
    if (QThread::currentThread() == this) {
        if (worker) worker->cancel();
        return;
    }

    wait();
    worker = nullptr;
    QCoreApplication *application = QCoreApplication::instance();
    if (application
        && QThread::currentThread() == application->thread()) {
        applyQueuedCloudSettings();
    }
}

void
CloudServiceAutoDownload::run()
{
    CloudServiceAutoDownloadWorker *activeWorker = worker;
    if (!activeWorker) return;

    activeWorker->begin();
    if (activeWorker->needsEventLoop()) exec();
    delete activeWorker;
}

void
CloudServiceAutoDownload::startDownload()
{
    if (downloadActive || isRunning() || !Context::isValid(context)) return;

#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    maximumQueuedDownloadEvents = std::max(
        1, cloudAutoDownloadMaximumQueuedDownloadsForTest());
    maximumQueuedDownloadBytes = std::max(
        qint64(1),
        cloudAutoDownloadMaximumQueuedDownloadBytesForTest());
#endif

    CloudAutoDownloadPlan plan;
    plan.now = QDateTime::currentDateTime();
#ifdef GC_TEST_CLOUD_AUTODOWNLOAD_PROBE
    plan.requestTimeoutMs = cloudAutoDownloadRequestTimeoutMs();
#endif

    CloudServiceFactory &factory = CloudServiceFactory::instance();
    const QString athlete = context->athlete->cyclist;
    bool hasEnabledService = false;

    for (const QString &name : factory.serviceNames()) {
        const CloudService *definition = factory.service(name);
        if (!definition) continue;
        if (!definition->supportsStartupActivityDownload()) continue;
        if (appsettings->cvalue(
                athlete,
                definition->syncOnStartupSettingName(),
                QStringLiteral("false")).toString()
            != QStringLiteral("true")) {
            continue;
        }

        if (CloudService *service =
                factory.newAutoDownloadService(name, context)) {
            plan.providers.append(service);
            hasEnabledService = true;
        }
    }

    if (!hasEnabledService) return;

    if (context->athlete->rideCache) {
        Specification specification;
        specification.setDateRange(DateRange(
            plan.now.addDays(-30).date(), plan.now.date()));
        const QVector<RideItem *> rides =
            context->athlete->rideCache->rides();
        for (RideItem *item : rides) {
            if (specification.pass(item)) {
                plan.rideFiles.insert(
                    QFileInfo(item->fileName).baseName().left(16));
            }
        }
    }

    for (auto service = plan.providers.begin();
         service != plan.providers.end();) {
        (*service)->moveToThread(this);
        if ((*service)->thread() != this) {
            qWarning() << "Autodownload: cannot move provider to worker";
            delete *service;
            service = plan.providers.erase(service);
        } else {
            ++service;
        }
    }

    plan.generation = advanceGenerationAndDiscardQueuedEvents();
    worker = new CloudServiceAutoDownloadWorker(this, std::move(plan));
    worker->moveToThread(this);

    downloadActive = true;
    start();
    context->notifyAutoDownloadStart();
}

void
CloudServiceAutoDownload::downloadProgress(
        quint64 runGeneration,
        QString service,
        double progress,
        int current,
        int total)
{
    if (runGeneration != generation.load(std::memory_order_acquire)
        || !Context::isValid(context)) return;
    context->notifyAutoDownloadProgress(
        service, progress, current, total);
}

void
CloudServiceAutoDownload::downloadFinished(
        quint64 runGeneration,
        bool cancelled)
{
    if (runGeneration != generation.load(std::memory_order_acquire))
        return;
    if (QThread::currentThread() != this) wait();

    worker = nullptr;
    downloadActive = false;
    applyQueuedCloudSettings();
    if (!cancelled && Context::isValid(context)) {
        context->notifyAutoDownloadEnd();
    }
}

void
CloudServiceAutoDownload::readComplete(
        QByteArray *data,
        QString name,
        QString message)
{
    Q_UNUSED(data)
    Q_UNUSED(name)
    Q_UNUSED(message)
}

void
CloudServiceAutoDownload::downloaded(
        quint64 runGeneration,
        QByteArray data,
        QString name,
        CloudService::CompressionType compression,
        QMap<QString, QString> expectedSettings,
        QMap<QString, QString> settingDefaults)
{
    if (runGeneration != generation.load(std::memory_order_acquire))
        return;
    QPointer<Context> activeContext(context);
    const auto contextIsValid = [&activeContext]() {
        Context *current = activeContext.data();
        return current
            && Context::isValid(current)
            && current->athlete;
    };
    if (!contextIsValid()) return;

    applyQueuedCloudSettings();
    if (!cloudSettingsMatch(
            activeContext->athlete->cyclist,
            expectedSettings, settingDefaults)) {
        qWarning() << "Autodownload: cloud settings changed during download";
        return;
    }

    // The provider and its worker-owned buffer can already be gone here.
    QStringList errors;
    RideFile *ride = CloudService::uncompressRide(
        activeContext.data(), compression, data, std::move(name), errors);

    // can't process the content received.
    if (!ride) return;
    if (!contextIsValid()) {
        delete ride;
        return;
    }

    // lets save this one away as json with the right filename
    QDateTime ridedatetime = ride->startTime();

    QChar zero = QLatin1Char ('0');
    QString targetnosuffix = QString ( "%1_%2_%3_%4_%5_%6" )
                           .arg ( ridedatetime.date().year(), 4, 10, zero )
                           .arg ( ridedatetime.date().month(), 2, 10, zero )
                           .arg ( ridedatetime.date().day(), 2, 10, zero )
                           .arg ( ridedatetime.time().hour(), 2, 10, zero )
                           .arg ( ridedatetime.time().minute(), 2, 10, zero )
                           .arg ( ridedatetime.time().second(), 2, 10, zero );

    const QString filename =
            activeContext->athlete->home->activities().canonicalPath()
            + QLatin1Char('/') + targetnosuffix + QStringLiteral(".json");

    // exists? -- totally should never happen unless readdir timestamp mismatches actual ride
    //            could happen if same file available at two services XXX should check above... XXX
    QFileInfo fileinfo(filename);
    if (fileinfo.exists()) {
        qDebug()<<"auto download got a duplicate:"<<filename;
        delete ride;
        return;
    }

    // process linked defaults
    GlobalContext::context()->rideMetadata->setLinkedDefaults(ride);
    if (!contextIsValid()) {
        delete ride;
        return;
    }

    // run the processor first... import
    DataProcessorFactory::instance().autoProcess(ride, "Auto", "Import");
    if (!contextIsValid()) {
        delete ride;
        return;
    }
    ride->recalculateDerivedSeries();
    // now metrics have been calculated
    DataProcessorFactory::instance().autoProcess(ride, "Save", "ADD");
    if (!contextIsValid()) {
        delete ride;
        return;
    }

    JsonFileReader reader;
    QFile file(filename);
    QString writeError;
    const bool saved = publishActivityBeforeCacheUpdate(
        [activeContext, &reader, ride, &file](QString &stepError) {
            Context *current = activeContext.data();
            if (!current
                || !Context::isValid(current)
                || !current->athlete) {
                stepError = QObject::tr(
                    "Athlete closed during cloud import.");
                return false;
            }
            return reader.writeRideFile(
                current, ride, file, stepError, false);
        },
        [activeContext, &fileinfo]() {
            Context *current = activeContext.data();
            if (current
                && Context::isValid(current)
                && current->athlete) {
                current->athlete->addRide(
                    fileinfo.fileName(), true, false);
            }
        },
        writeError);
    delete ride;
    if (!saved) {
        qWarning() << "Autodownload: cannot save activity:"
                   << writeError;
        return;
    }

}

CloudServiceAutoDownloadWidget::CloudServiceAutoDownloadWidget(Context *context,QWidget *parent) :
    QWidget(parent), context(context), state(Dormant)
{
    connect(context, SIGNAL(autoDownloadStart()), this, SLOT(downloadStart()));
    connect(context, SIGNAL(autoDownloadEnd()), this, SLOT(downloadFinish()));
    connect(context, SIGNAL(autoDownloadProgress(QString,double,int,int)), this, SLOT(downloadProgress(QString,double,int,int)));

    // just a small little thing
    setFixedHeight(dpiYFactor * 50);
    hide();

    // animating checking
    animator= new QPropertyAnimation(this, "transition");
    animator->setStartValue(0);
    animator->setEndValue(100);
    animator->setDuration(1000);
    animator->setEasingCurve(QEasingCurve::Linear);
}

void
CloudServiceAutoDownloadWidget::downloadStart()
{
    state = Checking;
    animator->start();
    show();
}

void
CloudServiceAutoDownloadWidget::downloadFinish()
{
    state = Dormant;
    animator->stop();
    hide();
}

void
CloudServiceAutoDownloadWidget::downloadProgress(QString s, double x, int i, int n)
{
    state = Downloading;
    animator->stop();
    show();
    progress = x;
    oneof=i;
    total=n;
    servicename=s;
    repaint();
}

void
CloudServiceAutoDownloadWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    QBrush brush(GColor(CPLOTBACKGROUND));
    painter.fillRect(0,0,width(),height(), brush);

    QString statusstring;
    switch(state) {
    case Dormant: statusstring=""; break;
    case Downloading: statusstring=tr("Downloading"); break;
    case Checking: statusstring=tr("Checking"); break;
    }

    // smallest font we can
    QFont font;
    QFontMetrics fm(font);
    painter.setFont(font);
    painter.setPen(GCColor::invertColor(GColor(CPLOTBACKGROUND)));
    QRectF textbox = QRectF(0,0, fm.horizontalAdvance(statusstring), height() / 2.0f);
    painter.drawText(textbox, Qt::AlignVCenter | Qt::AlignCenter, statusstring);

    // rectangle
    QRectF pr(textbox.width()+(5.0f*dpiXFactor), textbox.top()+(8.0f*dpiXFactor), width()-(10.0f*dpiXFactor)-textbox.width(), (height()/2.0f)-(16*dpiXFactor));

    // progress rect
    QColor col = GColor(CPLOTMARKER);
    col.setAlpha(150);
    brush= QBrush(col);

    if (state == Downloading) {
        QRectF bar(pr.left(), pr.top(), (pr.width() / 100.00f * progress), pr.height());
        painter.fillRect(bar, brush);

        // what's being downloaded?
        QRectF bottom(0, height()/2.0f, width(), height()/2.0f);
        painter.drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter, QString("%1 of %2").arg(oneof).arg(total));
        painter.drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, servicename);

    } else if (state == Checking) {
        // bounce
        QRectF lbar(pr.left()+ ((pr.width() *0.8f) / 100.0f * transition), pr.top(), pr.width() * 0.2f, pr.height());
        QRectF rbar(pr.left()+ (pr.width()*0.8f) - ((pr.width() *0.8f) / 100.0f * transition), pr.top(), pr.width() * 0.2f, pr.height());
        painter.fillRect(lbar, brush);
        painter.fillRect(rbar, brush);

        QRectF bottom(0, height()/2.0f, width(), height()/2.0f);
        painter.drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter, tr("Last 30 days"));

        // if we ran out of juice start again
        if (transition == 100) { animator->stop(); animator->start(); }
    }

    // border of progress bar
    painter.drawRect(pr);
}
