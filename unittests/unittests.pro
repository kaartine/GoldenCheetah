TEMPLATE = subdirs

exists(unittests.pri) {
	include(unittests.pri)
}

equals(GC_UNITTESTS, active) {
	SUBDIRS += Core/seasonOffset \
			   Core/season \
			   Core/seasonParser \
			   Core/units \
			   Core/utils \
			   Core/signalSafety \
			   Core/splineCrash \
			   FileIO/archiveSecurity \
			   FileIO/powerTapBounds \
			   FileIO/tacxCafBounds \
			   FileIO/wkoBounds \
			   Gui/calendarData \
			   Gui/iconBundleSecurity \
			   Gui/perspectiveStateSource \
			   Gui/trainPerspectiveState \
			   Python/pythonDataSeriesOwnership \
			   Train/antBurstBounds \
			   Train/kineticPacketBounds \
			   Train/virtualPowerTrainerOwnership
	CONFIG += ordered
} else {
	message("Unittests are disabled; to enable copy unittests/unittests.pri.in to unittests/unittests.pri")
}
