import QtQuick
import QtQuick3D

Item {
    width: 960
    height: 540

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: "#79a9bd"
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        PerspectiveCamera {
            fieldOfView: 46
            clipNear: 0.1
            clipFar: 100
            position: Qt.vector3d(4.2, 2.5, 5.0)
            lookAtNode: target
        }

        Node {
            id: target
            position: Qt.vector3d(0, -0.28, 10.0)
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-50, -25, 0)
            brightness: 1.2
            ambientColor: "#839083"
        }

        Wg_Drop_Greybox {
        }
    }
}
