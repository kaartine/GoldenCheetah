/*
 * Copyright (c) 2026 Jukka Kaartinen
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#ifndef _GC_ModalWorkflowGuard_h
#define _GC_ModalWorkflowGuard_h 1

#include <QDialog>
#include <QList>
#include <QPointer>

#include <functional>
#include <initializer_list>
#include <utility>


using ModalWorkflowValidator = std::function<bool()>;


class ModalWorkflowGuard
{
public:
    explicit ModalWorkflowGuard(
        const QList<QObject*> &owners,
        ModalWorkflowValidator topologyValidator = {})
        : topologyValidator(std::move(topologyValidator))
    {
        for (QObject *owner : owners)
            guardedOwners.append(QPointer<QObject>(owner));
    }

    ModalWorkflowGuard(
        std::initializer_list<QObject*> owners,
        ModalWorkflowValidator topologyValidator = {})
        : topologyValidator(std::move(topologyValidator))
    {
        for (QObject *owner : owners)
            guardedOwners.append(QPointer<QObject>(owner));
    }

    bool ownersAlive() const
    {
        if (guardedOwners.isEmpty()) return false;
        for (const QPointer<QObject> &owner : guardedOwners) {
            if (!owner) return false;
        }
        return true;
    }

    bool canCommit() const
    {
        return ownersAlive()
            && (!topologyValidator || topologyValidator());
    }

    void rejectOnOwnerLoss(QDialog *dialog) const
    {
        if (!dialog) return;
        const QPointer<QDialog> guardedDialog(dialog);
        const auto rejectDialog = [guardedDialog] {
            if (!guardedDialog) return;
            if (guardedDialog->isVisible()) {
                guardedDialog->reject();
                return;
            }
            QMetaObject::invokeMethod(
                guardedDialog.data(),
                [guardedDialog] {
                    if (guardedDialog)
                        guardedDialog->reject();
                },
                Qt::QueuedConnection);
        };
        for (const QPointer<QObject> &owner : guardedOwners) {
            QObject *const object = owner.data();
            if (!object) {
                rejectDialog();
                continue;
            }
            QObject::connect(
                object, &QObject::destroyed,
                dialog,
                rejectDialog,
                Qt::DirectConnection);
        }
    }

private:
    QList<QPointer<QObject>> guardedOwners;
    ModalWorkflowValidator topologyValidator;
};


template<typename MainWindow, typename Context, typename Tab>
bool modalWorkflowHasActiveTab(
    MainWindow *mainWindow,
    Context *context,
    Tab *tab)
{
    return mainWindow && context && tab
        && context->mainWindow == mainWindow
        && context->tab == tab
        && mainWindow->athleteTab() == tab;
}


template<typename Owner>
bool modalWorkflowHasExactPair(
    const QList<Owner*> &owners,
    Owner *first,
    Owner *second)
{
    return first && second && first != second
        && owners.size() == 2
        && owners.count(first) == 1
        && owners.count(second) == 1;
}


template<typename Owner>
class ModalOwnerSetSnapshot
{
public:
    explicit ModalOwnerSetSnapshot(const QList<Owner*> &owners)
    {
        for (Owner *owner : owners)
            guardedOwners.append(QPointer<Owner>(owner));
    }

    ModalOwnerSetSnapshot(std::initializer_list<Owner*> owners)
    {
        for (Owner *owner : owners)
            guardedOwners.append(QPointer<Owner>(owner));
    }

    bool matchesExactly(const QList<Owner*> &owners) const
    {
        if (owners.size() != guardedOwners.size()) return false;
        for (const QPointer<Owner> &guarded : guardedOwners) {
            Owner *const owner = guarded.data();
            if (!owner || owners.count(owner) != 1)
                return false;
        }
        for (Owner *owner : owners) {
            if (!owner) return false;
            int matches = 0;
            for (const QPointer<Owner> &guarded : guardedOwners) {
                if (guarded.data() == owner) ++matches;
            }
            if (matches != 1) return false;
        }
        return true;
    }

private:
    QList<QPointer<Owner>> guardedOwners;
};


template<typename Owner>
bool modalWorkflowCollectLiveOwners(
    const QList<QPointer<Owner>> &guardedOwners,
    QList<Owner*> &liveOwners)
{
    liveOwners.clear();
    bool allLive = true;
    for (const QPointer<Owner> &guarded : guardedOwners) {
        Owner *const owner = guarded.data();
        if (!owner) {
            allLive = false;
            continue;
        }
        if (liveOwners.contains(owner)) {
            allLive = false;
            continue;
        }
        liveOwners.append(owner);
    }
    return allLive;
}


template<typename Continuation>
bool modalWorkflowRunContinuation(
    const ModalWorkflowGuard &guard,
    Continuation continuation)
{
    if (!guard.canCommit()) return false;
    continuation();
    return true;
}


template<typename Sender, typename Signal, typename Predicate>
void modalWorkflowRejectOnMutation(
    Sender *sender,
    Signal signal,
    QDialog *dialog,
    Predicate shouldReject)
{
    if (!sender || !dialog) return;
    const QPointer<QDialog> guardedDialog(dialog);
    QObject::connect(
        sender, signal, dialog,
        [guardedDialog, shouldReject](auto &&...values) {
            if (guardedDialog && guardedDialog->isVisible()
                && std::invoke(
                    shouldReject,
                    std::forward<decltype(values)>(values)...)) {
                guardedDialog->reject();
            }
        },
        Qt::DirectConnection);
}


template<typename Sender, typename Signal>
void modalWorkflowRejectOnMutation(
    Sender *sender,
    Signal signal,
    QDialog *dialog)
{
    modalWorkflowRejectOnMutation(
        sender, signal, dialog,
        [](auto &&...) { return true; });
}


inline bool modalWorkflowCanAdvanceAfterCommit(
    bool committed,
    bool ownersCurrent)
{
    return committed || ownersCurrent;
}


template<typename Tab>
class ModalNoSwitchLease
{
public:
    explicit ModalNoSwitchLease(Tab *tab)
        : tab(tab), previousValue(
              tab ? tab->noSwitch() : false)
    {
        if (this->tab) this->tab->setNoSwitch(true);
    }

    ~ModalNoSwitchLease()
    {
        if (tab) tab->setNoSwitch(previousValue);
    }

    ModalNoSwitchLease(const ModalNoSwitchLease &) = delete;
    ModalNoSwitchLease &operator=(
        const ModalNoSwitchLease &) = delete;

private:
    QPointer<Tab> tab;
    bool previousValue;
};


template<typename Owner, typename Value>
class ModalPointerOverrideLease
{
public:
    using Getter = std::function<Value(const Owner &)>;
    using Setter = std::function<void(Owner &, const Value &)>;

    ModalPointerOverrideLease(
        Owner *owner,
        Value Owner::*member,
        const Value &temporaryValue,
        ModalWorkflowValidator ownerValidator = {},
        ModalWorkflowValidator originalValueValidator = {})
        : ModalPointerOverrideLease(
              owner,
              [member](const Owner &current) {
                  return current.*member;
              },
              [member](Owner &current, const Value &value) {
                  current.*member = value;
              },
              temporaryValue,
              std::move(ownerValidator),
              std::move(originalValueValidator))
    {
    }

    ModalPointerOverrideLease(
        Owner *owner,
        Getter getter,
        Setter setter,
        const Value &temporaryValue,
        ModalWorkflowValidator ownerValidator = {},
        ModalWorkflowValidator originalValueValidator = {})
        : owner(owner),
          getter(std::move(getter)),
          setter(std::move(setter)),
          originalValue(
              owner && this->getter
                  ? this->getter(*owner)
                  : Value()),
          installedValue(temporaryValue),
          ownerValidator(std::move(ownerValidator)),
          originalValueValidator(
              std::move(originalValueValidator)),
          active(owner != nullptr && this->getter && this->setter)
    {
        if (active)
            this->setter(*this->owner, temporaryValue);
    }

    ~ModalPointerOverrideLease()
    {
        restore();
    }

    bool publish(const Value &value)
    {
        if (!active || !owner
            || getter(*owner) != installedValue) {
            active = false;
            return false;
        }
        setter(*owner, value);
        installedValue = value;
        return true;
    }

    void restore()
    {
        if (!active || !owner
            || getter(*owner) != installedValue) {
            active = false;
            return;
        }
        const bool ownerIsCurrent =
            !ownerValidator || ownerValidator();
        const bool originalIsCurrent =
            !originalValueValidator
            || originalValueValidator();
        setter(
            *owner,
            ownerIsCurrent && originalIsCurrent
                ? originalValue
                : Value());
        active = false;
    }

    ModalPointerOverrideLease(
        const ModalPointerOverrideLease &) = delete;
    ModalPointerOverrideLease &operator=(
        const ModalPointerOverrideLease &) = delete;

private:
    QPointer<Owner> owner;
    Getter getter;
    Setter setter;
    Value originalValue;
    Value installedValue;
    ModalWorkflowValidator ownerValidator;
    ModalWorkflowValidator originalValueValidator;
    bool active;
};

#endif // _GC_ModalWorkflowGuard_h
