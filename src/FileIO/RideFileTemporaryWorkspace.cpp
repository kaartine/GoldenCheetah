/*
 * Copyright (c) 2026 GoldenCheetah contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "RideFileTemporaryWorkspace.h"

#include "AnchoredFileSystem.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include <utility>

namespace {

class PathBackedTemporaryFile final : public RideFileTemporaryFile
{
public:
    PathBackedTemporaryFile(
        AnchoredFileSystem::DirectoryAnchor root,
        const QString &suffix)
        : root_(std::move(root))
    {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const QString component = QStringLiteral("gc-ride-%1").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces));
            const AnchoredFileSystem::MutationResult created =
                AnchoredFileSystem::createPrivateFixedChildDirectory(
                    root_, component, workspace_);
            if (created.applied()) break;
            if (created.effect
                != AnchoredFileSystem::MutationEffect::Conflict) {
                return;
            }
        }
        if (!workspace_.isValid()) return;

        QString error;
        entry_ = workspace_.entry(
            QStringLiteral("activity.%1").arg(suffix), error);
        if (!entry_.isValid()) return;
        file_.setFileName(entry_.displayPath());
        if (!file_.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return;
#ifdef Q_OS_UNIX
        if (!file_.setPermissions(
                QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            file_.close();
            return;
        }
#endif
        ready_ = true;
    }

    ~PathBackedTemporaryFile() override
    {
        readFile_.close();
        file_.close();
        if (!pinned_.isValid() && entry_.isValid()) {
            QString ignored;
            AnchoredFileSystem::pinRegularFile(entry_, pinned_, ignored);
        }
        if (pinned_.isValid()) {
            AnchoredFileSystem::remove(pinned_);
        }
        if (workspace_.isValid()) {
            AnchoredFileSystem::removeEmptyDirectory(workspace_);
        }
    }

    bool isReady() const
    {
        return ready_;
    }

    QFile &file() override
    {
        return file_;
    }

    QFile *fileForReading() override
    {
        file_.close();
        QString error;
        if (!workspace_.pathMatches(error)
            || !AnchoredFileSystem::pinRegularFile(
                entry_, pinned_, error)) {
            return nullptr;
        }
        QString stablePath;
        if (!entry_.stableAccessPath(stablePath, error)) return nullptr;
        readFile_.setFileName(stablePath);
        return &readFile_;
    }

private:
    AnchoredFileSystem::DirectoryAnchor root_;
    AnchoredFileSystem::DirectoryAnchor workspace_;
    AnchoredFileSystem::EntryRef entry_;
    AnchoredFileSystem::PinnedFile pinned_;
    QFile file_;
    QFile readFile_;
    bool ready_ = false;
};

class PathBackedTemporaryWorkspace final : public RideFileTemporaryWorkspace
{
public:
    explicit PathBackedTemporaryWorkspace(QString rootPath)
        : rootPath_(std::move(rootPath))
    {
    }

    std::unique_ptr<RideFileTemporaryFile> createForSuffix(
        const QString &suffix) const override
    {
        if (!QDir::isAbsolutePath(rootPath_)
            || suffix.isEmpty()
            || QFileInfo(suffix).fileName() != suffix
            || suffix == QStringLiteral(".")
            || suffix == QStringLiteral("..")) {
            return {};
        }

        AnchoredFileSystem::DirectoryAnchor root;
        QString error;
        if (!AnchoredFileSystem::DirectoryAnchor::open(
                rootPath_, root, error)
            || !AnchoredFileSystem::validateCurrentUserControlledDirectory(
                root, error)) {
            return {};
        }

        auto temporary = std::make_unique<PathBackedTemporaryFile>(
            std::move(root), suffix);
        if (!temporary->isReady()) return {};
        return temporary;
    }

private:
    QString rootPath_;
};

} // namespace

std::shared_ptr<const RideFileTemporaryWorkspace>
rideFileTemporaryWorkspaceForRoot(const QString &rootPath)
{
    return std::make_shared<PathBackedTemporaryWorkspace>(rootPath);
}
