#ifndef _GC_CredentialStoreQtKeychain_h
#define _GC_CredentialStoreQtKeychain_h 1

#include "CredentialSettings.h"

#include <qtkeychain/keychain.h>

namespace CredentialStoreQtKeychainDetail {

CredentialStore::Status statusForError(QKeychain::Error error);
void configureJob(QKeychain::Job *job, const QString &key);

} // namespace CredentialStoreQtKeychainDetail

std::unique_ptr<CredentialStore> createQtKeychainCredentialStore();

#endif
