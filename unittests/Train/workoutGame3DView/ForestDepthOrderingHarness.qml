import QtQuick
import QtQuick3D
import ".." as WorkoutGame

Item {
    id: root
    width: 960
    height: 540
    property bool showRearRock: false

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
            clipFar: 50
            position: Qt.vector3d(0, 2.7, -10)
            lookAtNode: target
        }

        Node {
            id: target
            position: Qt.vector3d(0, 2.2, 0)
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-48, -28, 0)
            brightness: 1.3
            ambientColor: "#839083"
        }

        Model {
            source: "#Rectangle"
            position: Qt.vector3d(0, -0.02, 0)
            eulerRotation.x: -90
            scale: Qt.vector3d(6, 8, 1)
            materials: PrincipledMaterial {
                baseColor: "#638455"
                roughness: 1
            }
        }

        WorkoutGame.WorkoutGameConifer {
            variant: 1
            instanceTable: treeInstances
        }

        WorkoutGame.WorkoutGameForestFloorProp {
            visible: root.showRearRock
            variant: 1
            biome: 0
            instanceTable: rockInstances
        }
    }
}
