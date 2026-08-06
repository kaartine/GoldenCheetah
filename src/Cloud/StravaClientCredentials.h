/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef GC_STRAVA_CLIENT_CREDENTIALS_H
#define GC_STRAVA_CLIENT_CREDENTIALS_H

#include <QString>

namespace StravaClientCredentials {

enum class VaultStatus {
    Present,
    NotFound,
    Unavailable
};

struct VaultReadResult {
    VaultStatus status = VaultStatus::Unavailable;
    QString value;
};

class Vault
{
public:
    virtual ~Vault() = default;
    virtual VaultReadResult read() = 0;
    virtual bool write(const QString &value) = 0;
    virtual bool remove() = 0;
};

enum class Source {
    None,
    RuntimeVault,
    CompileTimeFallback
};

enum class Status {
    Available,
    Missing,
    VaultUnavailable,
    InvalidRuntimeCredentials
};

struct Credentials {
    QString clientId;
    QString clientSecret;
    Source source = Source::None;
};

struct Resolution {
    Status status = Status::Missing;
    Credentials credentials;
    QString error;

    bool isAvailable() const
    {
        return status == Status::Available;
    }
};

struct MutationResult {
    bool succeeded = false;
    QString error;
};

enum class UserAction {
    SaveIfProvided,
    Remove
};

Resolution resolve(
    Vault &vault,
    const QString &compileTimeClientId,
    const QString &compileTimeClientSecret);
MutationResult store(
    Vault &vault,
    const QString &clientId,
    const QString &clientSecret);
MutationResult clear(Vault &vault);
MutationResult applyUserEdit(
    Vault &vault,
    const QString &clientId,
    const QString &clientSecret,
    UserAction action);

Resolution resolveForAccount(const QString &accountKey);
Resolution runtimeForAccount(const QString &accountKey);
MutationResult storeForAccount(
    const QString &accountKey,
    const QString &clientId,
    const QString &clientSecret);
MutationResult clearForAccount(const QString &accountKey);
MutationResult applyUserEditForAccount(
    const QString &accountKey,
    const QString &clientId,
    const QString &clientSecret,
    UserAction action);
bool compileTimeFallbackIsConfigured();

} // namespace StravaClientCredentials

#endif
