TEMPLATE = subdirs

exists(unittests.pri) {
	include(unittests.pri)
}

equals(GC_UNITTESTS, active) {
	SUBDIRS += Core/athleteMigrationSafety \
			   Train/libraryImportFileStager \
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
			   Gui/iconBundleSecurity \
			   Gui/splitActivitySave \
			   Gui/splitRideData \
			   Gui/perspectiveStateSource \
			   Gui/trainPerspectiveState \
			   Metrics/rideMetadataAtomicSave \
			   Python/pythonDataSeriesOwnership \
			   Train/antBurstBounds \
			   Train/kineticPacketBounds \
			   Train/vmProWidgetLifecycle \
			   Train/virtualPowerTrainerOwnership \
			   Train/bt40Lifecycle \
			   Train/deviceSelection \
			   Train/trainingStopPolicy \
			   Train/trainingRecordingIo
	CONFIG += ordered
} else {
	message("Unittests are disabled; to enable copy unittests/unittests.pri.in to unittests/unittests.pri")
}
