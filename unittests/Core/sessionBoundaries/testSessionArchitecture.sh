#!/usr/bin/env bash

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
context_header="$root/src/Core/Context.h"
context_source="$root/src/Core/Context.cpp"
context_services_source="$root/src/Core/ContextSessionServices.cpp"
cache_source="$root/src/FileIO/RideFileCache.cpp"
ride_item_source="$root/src/Core/RideItem.cpp"
manual_activity_source="$root/src/Gui/ManualActivityWizard.cpp"

fail()
{
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

require_text()
{
    local file=$1
    local text=$2
    grep -Fq -- "$text" "$file" ||
        fail "$file does not contain required boundary: $text"
}

reject_text()
{
    local file=$1
    local text=$2
    if grep -Fq -- "$text" "$file"; then
        fail "$file still contains Context-owned state: $text"
    fi
}

require_text "$context_header" "std::unique_ptr<AthleteSession> athleteSession_"
require_text "$context_header" "std::unique_ptr<TrainingSession> trainingSession_"
require_text "$context_header" "AthleteSession &athleteSession()"
require_text "$context_header" "TrainingSession &trainingSession()"

reject_text "$context_header" "bool isRunning;"
reject_text "$context_header" "bool isPaused;"
reject_text "$context_header" "ErgFile *workout;"
reject_text "$context_header" "VideoSyncFile *videosync;"
reject_text "$context_header" "QString videoFilename;"
reject_text "$context_header" "long now;"
reject_text "$context_header" "QWebEngineProfile* webEngineProfile"
reject_text "$context_header" "HtmlTrainingBridge *m_HtmlTrainingBridge"
reject_text "$context_header" "cacheWriteErrorCoordinator_"

require_text "$context_services_source" "ContextAthleteApplicationService"
require_text "$context_services_source" "ContextAthletePersistenceService"
require_text "$context_services_source" "ContextTrainingApplicationService"
require_text "$cache_source" "context->reportCacheWriteFailure"
require_text "$cache_source" "athleteSession().persistenceService()"
require_text "$ride_item_source" "athleteSession().persistenceService()"
reject_text "$manual_activity_source" "&Context::workout"

for header in AthleteSession.h TrainingSession.h SessionServices.h; do
    path="$root/src/Core/$header"
    test -f "$path" || fail "missing session boundary header: $path"
    if grep -Eq '#include[[:space:]]+[<\"](Gui|Train|Cloud)/' "$path"; then
        fail "$path imports an outer application layer"
    fi
done

printf 'PASS: Context session ownership and dependency boundaries are enforced\n'
