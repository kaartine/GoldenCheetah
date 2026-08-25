import QtQuick

Item {
    id: root
    objectName: "trainingHud"
    required property var viewModel
    property bool compact: width < 720
    implicitHeight: compact ? 188 : 130

    function elapsedText(totalSeconds) {
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    function sensorText(value, unit) {
        return value > 0 ? value + " " + unit : "-- " + unit
    }

    function metricObjectName(index) {
        const names = ["powerValue", "targetValue", "cadenceValue",
                       "heartRateValue", "speedValue", "gradeValue",
                       "gearValue", "timeValue", "distanceValue", "fpsValue"]
        return names[index]
    }

    function metricValue(index) {
        if (index === 0) return Math.round(viewModel.watts) + " W"
        if (index === 1) return Math.round(viewModel.targetWatts) + " W"
        if (index === 2) return sensorText(viewModel.cadenceRpm, "RPM")
        if (index === 3) return sensorText(viewModel.heartRate, "BPM")
        if (index === 4) return viewModel.speedKph.toFixed(1) + " KM/H"
        if (index === 5) return viewModel.gradePercent.toFixed(1) + "%"
        if (index === 6) return "G " + viewModel.virtualGear
        if (index === 7) return elapsedText(viewModel.workoutTimeSeconds)
        if (index === 8) return (viewModel.distanceMeters / 1000).toFixed(2) + " KM"
        return viewModel.fps.toFixed(1) + " FPS"
    }

    Rectangle {
        id: statsPanel
        width: parent.width
        height: root.compact ? 112 : 60
        color: "#ee131719"
        border.color: "#66838a84"
        border.width: 1

        Grid {
            id: statsGrid
            anchors.fill: parent
            anchors.margins: 6
            columns: root.compact ? 3 : 10
            property real cellWidth: width / columns
            property real cellHeight: height / (root.compact ? 4 : 1)

            Repeater {
                model: [
                    { label: qsTr("POWER"), name: "powerValue", color: "#ffffff" },
                    { label: qsTr("TARGET"), name: "targetValue", color: "#f0cf55" },
                    { label: qsTr("CADENCE"), name: "cadenceValue", color: "#ffffff" },
                    { label: qsTr("HEART"), name: "heartRateValue", color: "#ff746b" },
                    { label: qsTr("SPEED"), name: "speedValue", color: "#ffffff" },
                    { label: qsTr("GRADE"), name: "gradeValue", color: "#8ed7a3" },
                    { label: qsTr("GEAR"), name: "gearValue", color: "#85d4ef" },
                    { label: qsTr("TIME"), name: "timeValue", color: "#ffffff" },
                    { label: qsTr("DISTANCE"), name: "distanceValue", color: "#ffffff" },
                    { label: qsTr("RENDER"), name: "fpsValue", color: "#dbe8e5" }
                ]

                delegate: Item {
                    required property var modelData
                    required property int index
                    width: statsGrid.cellWidth
                    height: statsGrid.cellHeight

                    Rectangle {
                        visible: index > 0
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: "#3d4b48"
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        text: modelData.label
                        color: "#9ab5a7"
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: root.metricObjectName(index)
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        text: root.metricValue(index)
                        color: modelData.color
                        font.pixelSize: root.compact ? 13 : 15
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 9
                        font.bold: modelData.name === "powerValue"
                                   || modelData.name === "gearValue"
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    Rectangle {
        id: profilePanel
        objectName: "powerProfile"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: statsPanel.bottom
        anchors.topMargin: 8
        height: 60
        color: "#ee071012"
        border.color: "#66838a84"
        border.width: 1

        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 5
            text: qsTr("WORKOUT PROFILE")
            color: "#9ab5a7"
            font.pixelSize: 9
            font.bold: true
        }

        Item {
            id: profileGraph
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 18
            anchors.bottomMargin: 5

            Repeater {
                model: root.viewModel.powerProfileSegments
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    objectName: "powerProfileSegment"
                    x: modelData.start * profileGraph.width
                    y: (1 - modelData.height) * profileGraph.height
                    width: Math.max(1, (modelData.end - modelData.start)
                                    * profileGraph.width)
                    height: modelData.height * profileGraph.height
                    color: index % 2 ? "#4f988c" : "#62aa9a"
                }
            }

            Rectangle {
                id: cursor
                objectName: "powerProfileCursor"
                x: Math.min(profileGraph.width - width,
                            Math.max(0, root.viewModel.workoutProgress
                                     * profileGraph.width - width / 2))
                width: 3
                height: profileGraph.height
                color: "#f2d45c"
            }
        }
    }
}
