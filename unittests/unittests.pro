TEMPLATE = subdirs

exists(unittests.pri) {
	include(unittests.pri)
}

equals(GC_UNITTESTS, active) {
	SUBDIRS += Core/athleteMigrationSafety \
			   Core/credentialSettings \
			   Core/dataFilterSafety \
			   Core/localApiSecurity \
			   Cloud/credentialTransportSafety \
			   Cloud/oauthCallbackPolicy \
			   Cloud/openDataEndpointPolicy \
			   Charts/mapPageSecurity \
			   Train/libraryImportFileStager \
			   Train/webDownloadImportPolicy \
			   Train/trainDbVersionSafety \
			   Core/measuresAtomicSave \
			   Core/rideCacheAtomicSave \
			   Core/rideCacheRemoval \
			   Core/seasonOffset \
			   Core/season \
			   Core/seasonParser \
			   Core/units \
			   Core/utils \
			   Core/signalSafety \
			   Core/splineCrash \
			   FileIO/atomicActivitySave \
			   FileIO/archiveSecurity \
			   FileIO/athleteBackupArchive \
			   FileIO/powerTapBounds \
			   FileIO/rideFileOwnership \
			   FileIO/tacxCafBounds \
			   FileIO/ttsReaderBounds \
			   FileIO/wkoBounds \
			   Gui/calendarData \
			   Gui/mergeActivityAlignment \
			   Gui/iconBundleSecurity \
			   Gui/splitActivitySave \
			   Gui/splitRideData \
			   Gui/perspectiveStateSource \
			   Gui/trainPerspectiveState \
			   Metrics/rideMetadataAtomicSave \
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
			   Train/ftmsTargetReadiness \
			   Train/bluetoothTelemetryRouter
	!win32:SUBDIRS += Train/usbXpressSafety
	CONFIG += ordered
} else {
	message("Unittests are disabled; to enable copy unittests/unittests.pri.in to unittests/unittests.pri")
}
