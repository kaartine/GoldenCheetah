#ifndef _GC_CredentialStoreQtKeychain_h
#define _GC_CredentialStoreQtKeychain_h 1

#include "CredentialSettings.h"

#include <QByteArray>
#include <qtkeychain/keychain.h>

namespace CredentialStoreQtKeychainDetail {

CredentialStore::Status statusForError(QKeychain::Error error);
void configureJob(QKeychain::Job *job, const QString &key);
QByteArray linuxRuntimeStatusReport(bool compileSupport,
                                    bool runtimeAvailable);
QString bundledLinuxRuntimePath(const QString &applicationDir);
void configureBundledLinuxRuntime(const QString &applicationDir);
bool linuxLibSecretCompileSupport();
bool linuxLibSecretRuntimeAvailable();

} // namespace CredentialStoreQtKeychainDetail

std::unique_ptr<CredentialStore> createQtKeychainCredentialStore();

#endif
