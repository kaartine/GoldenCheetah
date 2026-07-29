#ifndef _GC_CredentialStoreQtKeychain_h
#define _GC_CredentialStoreQtKeychain_h 1

#include "CredentialSettings.h"

#include <QByteArray>
#include <qtkeychain/keychain.h>

#ifdef GC_CREDENTIAL_TEST_HOOKS
#include <functional>
#endif

namespace CredentialStoreQtKeychainDetail {

CredentialStore::Status statusForError(QKeychain::Error error);
void configureJob(QKeychain::Job *job, const QString &key);
QByteArray linuxRuntimeStatusReport(bool compileSupport,
                                    bool runtimeAvailable);
QString bundledLinuxRuntimePath(const QString &applicationDir);
void configureBundledLinuxRuntime(const QString &applicationDir);
bool linuxLibSecretCompileSupport();
bool linuxLibSecretRuntimeAvailable();

#ifdef GC_CREDENTIAL_TEST_HOOKS
using JobStartHook =
    std::function<bool(QKeychain::Job *)>;
using JobTimeoutHook =
    std::function<void(QKeychain::Job *)>;
void setJobStartHookForTest(JobStartHook hook);
void setJobTimeoutHookForTest(JobTimeoutHook hook);
void setJobTimeoutForTest(int timeoutMs);
void resetJobTestHooks();
void resetJobGateForTest();
#endif

} // namespace CredentialStoreQtKeychainDetail

std::unique_ptr<CredentialStore> createQtKeychainCredentialStore();

#endif
