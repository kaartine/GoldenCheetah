import QtQuick

Item {
    id: root
    objectName: "landingDustBurst"

    required property var triggerId
    required property real strength
    property real progress: 1
    property real capturedStrength: 0
    property bool acceptingTriggers: false
    property var handledTriggerId: 0
    width: 150
    height: 84
    opacity: progress < 1
             ? (0.55 + capturedStrength * 0.40)
               * (1 - Math.pow(progress, 1.6)) : 0
    visible: opacity > 0.01
    z: 5

    Component.onCompleted: {
        handledTriggerId = triggerId
        acceptingTriggers = true
    }

    onTriggerIdChanged: {
        if (acceptingTriggers && triggerId > 0
                && triggerId !== handledTriggerId) {
            handledTriggerId = triggerId
            Qt.callLater(startBurst)
        }
    }

    function startBurst() {
        capturedStrength = Math.max(0.32, Math.min(1, strength))
        progress = 0
        burst.restart()
    }

    NumberAnimation {
        id: burst
        objectName: "landingDustAnimation"
        target: root
        property: "progress"
        from: 0
        to: 1
        duration: 460
        easing.type: Easing.OutCubic
    }

    component DustPuff: Item {
        required property real side
        width: 40 + root.progress * 28
        height: 38 + root.progress * 24
        x: root.width / 2 - width / 2
           + side * (18 + root.progress * 38)
        y: root.height * 0.50 - height / 2
           - root.progress * (14 + Math.abs(side) * 10)

        Rectangle {
            x: 0
            y: parent.height * 0.34
            width: parent.width * 0.52
            height: parent.height * 0.42
            color: "#b07f48"
        }
        Rectangle {
            x: parent.width * 0.34
            y: 0
            width: parent.width * 0.48
            height: parent.height * 0.54
            color: "#f0d38b"
        }
        Rectangle {
            x: parent.width * 0.56
            y: parent.height * 0.48
            width: parent.width * 0.44
            height: parent.height * 0.36
            color: "#8d6038"
        }
    }

    DustPuff {
        objectName: "landingDustLeft"
        side: -1
    }
    DustPuff {
        objectName: "landingDustCentre"
        side: 0
    }
    DustPuff {
        objectName: "landingDustRight"
        side: 1
    }
}
