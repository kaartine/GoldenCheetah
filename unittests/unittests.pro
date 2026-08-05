TEMPLATE = subdirs

exists(unittests.pri) {
	include(unittests.pri)
}

equals(GC_UNITTESTS, active) {
	SUBDIRS += Build/ciTestRunner \
			   Core/athleteMigrationSafety \
			   Core/credentialSettings \
			   Core/dataFilterResources \
			   Core/dataFilterSafety \
			   Core/dataFilterZones \
			   Core/localApiSecurity \
			   Cloud/credentialTransportSafety \
			   Cloud/oauthCallbackPolicy \
			   Cloud/oauthTokenReplyController \
			   Cloud/stravaAccountRemoval \
			   Cloud/stravaAuthenticatedSession \
			   Cloud/stravaOAuthPolicy \
			   Cloud/stravaApiReplyPolicy \
			   Cloud/stravaTokenRefresh \
			   Cloud/stravaTokenPublication \
			   Cloud/openDataEndpointPolicy \
			   Cloud/openDataCaptureUtils \
			   Cloud/openDataCaptureStateMachine \
			   Cloud/openDataExport \
			   Cloud/openDataSummaryStatistics \
			   Cloud/openDataTransport \
			   Charts/indendPlotMarkerMatrix \
			   Charts/mapPageSecurity \
			   Charts/mapRoutePointIndex \
			   Charts/voronoiSafety \
			   Train/libraryImportFileStager \
			   Train/stravaRoutesClient \
			   Train/webDownloadImportPolicy \
			   Train/trainDbVersionSafety \
			   Core/measuresAtomicSave \
			   Core/plannedActivityFileStager \
			   Core/rideCacheAtomicSave \
			   Core/rideCacheCallbackGuard \
			   Core/rideCacheSaveSnapshot \
			   Core/rideCachePerformance \
			   Core/rideCacheRemoval \
			   Core/seasonOffset \
			   Core/season \
			   Core/seasonParser \
			   Core/units \
			   Core/utils \
			   Core/signalSafety \
			   Core/splineCrash \
			   FileIO/atomicActivitySave \
			   FileIO/anchoredFilesystem \
			   FileIO/archiveSecurity \
			   FileIO/athleteBackupArchive \
			   FileIO/durableFilesystem \
			   FileIO/cpCsvImport \
			   FileIO/fitImportIntegrity \
			   FileIO/fitReaderIntegrity \
			   FileIO/fixGpsSmoothingSafety \
			   FileIO/jsonImportIntegrity \
			   FileIO/powerTapBounds \
			   FileIO/rideFileCacheIntegrity \
			   FileIO/rideFileCacheRefresh \
			   FileIO/rideFileCacheWriteError \
			   FileIO/rideFileCrc \
			   FileIO/rideFileOwnership \
			   FileIO/tacxCafBounds \
			   FileIO/tcxPointBudget \
			   FileIO/xmlImportIntegrity \
			   FileIO/ttsReaderBounds \
			   FileIO/wkoBounds \
			   Gui/activityDeletionWorkflow \
			   Gui/activitySaveWorkflow \
			   Gui/cacheWriteWarning \
			   Gui/calendarModalWorkflow \
			   Gui/calendarData \
			   Gui/mergeActivityDistanceCursor \
			   Gui/mergeActivityRidePreparation \
			   Gui/mergeActivityTimeOffset \
			   Gui/mergeActivityXData \
			   Gui/rideImportBatch \
			   Gui/repeatPlanWorkflow \
			   Gui/mergeActivityAlignment \
			   Gui/iconBundleSecurity \
			   Gui/splitActivitySave \
			   Gui/splitRideData \
			   Gui/perspectiveStateSource \
			   Gui/rideNavigatorSearchFilter \
			   Gui/trainPerspectiveState \
			   Metrics/rideMetadataAtomicSave \
			   Metrics/estimatorThreadControl \
			   Planning/planBundleImportJournal \
			   Planning/planReplacementJournal \
			   Planning/planBundleReaderLifetime \
			   Python/pythonDataSeriesOwnership \
			   Python/pythonChartLifecycle \
			   Train/antBurstBounds \
			   Train/antLifecycle \
			   Train/antThreadSafety \
			   Train/kineticPacketBounds \
			   Train/vmProWidgetLifecycle \
			   Train/virtualPowerTrainerOwnership \
			   Train/bt40Lifecycle \
			   Train/deviceSelection \
			   Train/trainingStopPolicy \
			   Train/trainingRecordingIo \
			   Train/trainingTelemetryTimeline \
			   Train/trainRuntime \
			   Train/ftmsTargetReadiness \
			   Train/bluetoothTelemetryRouter
	linux:SUBDIRS += Build/appImagePackaging
	!win32:SUBDIRS += Train/usbXpressSafety
	CONFIG += ordered
} else {
	message("Unittests are disabled; to enable copy unittests/unittests.pri.in to unittests/unittests.pri")
}
