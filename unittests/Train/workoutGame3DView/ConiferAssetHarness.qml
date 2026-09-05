pragma ComponentBehavior: Bound

import QtQuick
import QtQuick3D
import ".." as WorkoutGame

Item {
    id: root
    width: 1280
    height: 720
    property int cameraAngle: 0
    readonly property var catalog: [
        { "label": "NARROW SPRUCE", "variant": 0 },
        { "label": "LAYERED SPRUCE", "variant": 1 },
        { "label": "SILVER BIRCH", "variant": 2 },
        { "label": "SCOTS PINE", "variant": 3 }
    ]

    Row {
        anchors.fill: parent

        Repeater {
            model: root.catalog

            delegate: Item {
                id: catalogCell
                required property var modelData
                width: root.width / root.catalog.length
                height: root.height

                View3D {
                    anchors.fill: parent

                    environment: SceneEnvironment {
                        backgroundMode: SceneEnvironment.Color
                        clearColor: "#78a9bf"
                        antialiasingMode: SceneEnvironment.MSAA
                        antialiasingQuality: SceneEnvironment.High
                    }

                    PerspectiveCamera {
                        fieldOfView: 43
                        clipNear: 0.1
                        clipFar: 100
                        position: Qt.vector3d(0, 2.9, -11.5)
                        lookAtNode: target
                    }

                    Node {
                        id: target
                        position: Qt.vector3d(0, 2.65, 0)
                    }

                    DirectionalLight {
                        eulerRotation: Qt.vector3d(-48, -28, 0)
                        brightness: 1.2
                        ambientColor: "#839083"
                    }

                    Model {
                        source: "#Rectangle"
                        position: Qt.vector3d(0, -0.02, 0)
                        eulerRotation.x: -90
                        scale: Qt.vector3d(5, 8, 1)
                        materials: PrincipledMaterial {
                            baseColor: "#638455"
                            roughness: 1
                        }
                    }

                    WorkoutGame.WorkoutGameConifer {
                        variant: catalogCell.modelData.variant
                        eulerRotation.y: root.cameraAngle === 1
                                         ? 45 : root.cameraAngle === 2 ? 135 : 0
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: "#406474"
                    opacity: 0.45
                }

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 20
                    text: catalogCell.modelData.label
                    color: "#edf2df"
                    font.pixelSize: 18
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    style: Text.Outline
                    styleColor: "#26382c"
                }
            }
        }
    }
}
