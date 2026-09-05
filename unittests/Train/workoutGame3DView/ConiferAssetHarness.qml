import QtQuick
import QtQuick3D
import ".." as WorkoutGame

Item {
    width: 1280
    height: 720

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
            position: Qt.vector3d(0, 2.65, -14)
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
            scale: Qt.vector3d(12, 10, 1)
            materials: PrincipledMaterial {
                baseColor: "#638455"
                roughness: 1
            }
        }

        WorkoutGame.WorkoutGameConifer {
            position.x: 4.5
            variant: 0
        }
        WorkoutGame.WorkoutGameConifer {
            position.x: 1.5
            variant: 1
        }
        WorkoutGame.WorkoutGameConifer {
            position.x: -1.5
            variant: 2
        }
        WorkoutGame.WorkoutGameConifer {
            position.x: -4.5
            variant: 3
        }
    }

    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        anchors.leftMargin: 94
        anchors.rightMargin: 94

        Repeater {
            model: ["NARROW SPRUCE", "LAYERED SPRUCE",
                    "BROKEN-TOP SPRUCE", "SCOTS PINE"]
            delegate: Text {
                required property string modelData
                width: parent.width / 4
                text: modelData
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
