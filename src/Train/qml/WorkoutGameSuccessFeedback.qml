import QtQuick

Item {
    id: root
    objectName: "successFeedback"

    required property var triggerId
    required property string effectText
    property real progress: 1
    property bool acceptingTriggers: false
    property var handledTriggerId: 0
    opacity: progress < 1
             ? Math.min(1, (1 - progress) * 2.8) : 0
    scale: 0.92 + 0.08 * Math.min(1, progress * 4)
    visible: opacity > 0.01
    width: Math.min(360, parent ? parent.width - 40 : 360)
    height: 42
    z: 100

    Component.onCompleted: {
        handledTriggerId = triggerId
        acceptingTriggers = true
    }

    onTriggerIdChanged: {
        if (acceptingTriggers && triggerId > 0
                && triggerId !== handledTriggerId) {
            handledTriggerId = triggerId
            progress = 0
            pulse.restart()
        }
    }

    NumberAnimation {
        id: pulse
        objectName: "successFeedbackAnimation"
        target: root
        property: "progress"
        from: 0
        to: 1
        duration: 900
        easing.type: Easing.OutCubic
    }

    Rectangle {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(24, parent.width * 0.16)
        height: 5
        color: "#63d98a"
    }
    Text {
        objectName: "successFeedbackText"
        anchors.centerIn: parent
        width: parent.width * 0.64
        text: root.effectText.toUpperCase()
        color: "white"
        font.pixelSize: 20
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
    }
    Rectangle {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(24, parent.width * 0.16)
        height: 5
        color: "#63d98a"
    }
}
