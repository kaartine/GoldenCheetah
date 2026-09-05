import QtQuick
import QtQuick3D

Item {
    id: root
    width: 1280
    height: 720
    property int loadedFloorProps: 0
    property int loadedVergeClusters: 0
    readonly property var catalog: [
        { "label": "GRANITE LOW", "kind": 0, "variant": 0 },
        { "label": "GRANITE UPRIGHT", "kind": 0, "variant": 1 },
        { "label": "GRANITE SLAB", "kind": 0, "variant": 2 },
        { "label": "ROOTED STUMP", "kind": 0, "variant": 3 },
        { "label": "DEADWOOD", "kind": 0, "variant": 4 },
        { "label": "FERN", "kind": 0, "variant": 5 },
        { "label": "BILBERRY", "kind": 0, "variant": 6 },
        { "label": "HEATHER", "kind": 0, "variant": 7 },
        { "label": "GRANITE VERGE", "kind": 1, "variant": 0 },
        { "label": "STUMP VERGE", "kind": 1, "variant": 1 },
        { "label": "DEADWOOD VERGE", "kind": 1, "variant": 2 }
    ]

    Grid {
        anchors.fill: parent
        columns: 4
        rows: 3

        Repeater {
            model: root.catalog

            delegate: Item {
                id: cell
                required property var modelData
                width: root.width / 4
                height: root.height / 3

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
                        clipFar: 30
                        position: Qt.vector3d(
                            cell.modelData.kind === 1 ? 1.5 : 0,
                            1.2, -4.2)
                        lookAtNode: target
                    }
                    Node {
                        id: target
                        position: Qt.vector3d(
                            cell.modelData.kind === 1 ? 1.5 : 0,
                            0.25, 0)
                    }
                    DirectionalLight {
                        eulerRotation: Qt.vector3d(-48, -28, 0)
                        brightness: 1.35
                        ambientColor: "#839083"
                    }
                    Model {
                        source: "#Rectangle"
                        position: Qt.vector3d(0, -0.015, 0)
                        eulerRotation.x: -90
                        scale: Qt.vector3d(4, 4, 1)
                        materials: PrincipledMaterial {
                            baseColor: "#638455"
                            roughness: 1
                        }
                    }
                    Loader3D {
                        id: assetLoader
                        objectName: "forestDressingAssetLoader"
                        Component.onCompleted: {
                            const sourceUrl = cell.modelData.kind === 0
                                    ? "qrc:/qml/WorkoutGameForestFloorProp.qml"
                                    : "qrc:/qml/WorkoutGameForestVergeCluster.qml"
                            setSource(sourceUrl, {
                                "variant": cell.modelData.variant
                            })
                        }
                        onLoaded: {
                            if (item.objectName === "workoutGameForestFloorProp") {
                                ++root.loadedFloorProps
                            } else if (item.objectName
                                       === "workoutGameForestVergeCluster") {
                                ++root.loadedVergeClusters
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    width: 1
                    height: parent.height
                    color: "#406474"
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: "#406474"
                }
                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: cell.modelData.label
                    color: "#edf2df"
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    style: Text.Outline
                    styleColor: "#26382c"
                }
            }
        }
    }
}
