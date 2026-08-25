import QtQuick
import QtQuick3D
import "assets" as WorkoutGameAssets

Item {
    id: root
    width: 1280
    height: 720

    function elapsedText(totalSeconds) {
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: "#78a9bf"
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            temporalAAEnabled: false
        }

        PerspectiveCamera {
            id: camera
            fieldOfView: 47
            clipNear: 0.15
            clipFar: 650
            position: Qt.vector3d(
                workoutGame3D.cameraX,
                workoutGame3D.cameraY,
                workoutGame3D.cameraZ)
            lookAtNode: cameraTarget
        }

        Node {
            id: cameraTarget
            position: Qt.vector3d(
                workoutGame3D.cameraTargetX,
                workoutGame3D.cameraTargetY,
                workoutGame3D.cameraTargetZ)
        }

        DirectionalLight {
            eulerRotation.x: -52
            eulerRotation.y: -28
            brightness: 1.15
            ambientColor: "#809080"
            castsShadow: false
        }

        Model {
            geometry: workoutGame3D.floorGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                roughness: 1
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            geometry: workoutGame3D.trailGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                roughness: 0.95
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
            }
            castsShadows: false
            receivesShadows: false
        }

        Repeater3D {
            model: workoutGame3D.trees
            delegate: Node {
                required property var modelData
                position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                scale: Qt.vector3d(modelData.scale, modelData.scale, modelData.scale)

                Model {
                    source: "#Cylinder"
                    y: 1.45
                    scale: Qt.vector3d(0.0035, 0.029, 0.0035)
                    materials: PrincipledMaterial {
                        baseColor: "#5b3a22"
                        roughness: 1
                    }
                }
                Model {
                    source: "#Cone"
                    y: 3.25
                    scale: Qt.vector3d(0.022, 0.045, 0.022)
                    materials: PrincipledMaterial {
                        baseColor: modelData.variant % 2 ? "#245b35" : "#2f6b3d"
                        roughness: 1
                    }
                }
            }
        }

        Repeater3D {
            model: workoutGame3D.features
            delegate: Node {
                required property var modelData
                position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                eulerRotation.y: modelData.yaw

                Model {
                    visible: modelData.kind === 1 || modelData.kind === 5
                             || modelData.kind === 9
                    source: "#Cylinder"
                    y: 0.19
                    eulerRotation.z: 90
                    scale: Qt.vector3d(0.0038, 0.018, 0.0038)
                    materials: PrincipledMaterial {
                        baseColor: "#64401f"
                        roughness: 1
                    }
                }
                Model {
                    visible: modelData.kind === 4 || modelData.kind === 11
                    source: "#Sphere"
                    y: 0.26
                    scale: Qt.vector3d(0.012, 0.0065, 0.010)
                    materials: PrincipledMaterial {
                        baseColor: "#77766e"
                        roughness: 1
                    }
                }
                Model {
                    visible: modelData.kind === 7
                    source: "#Cube"
                    y: 0.20
                    z: 2.2
                    scale: Qt.vector3d(0.005, 0.003, 0.05)
                    materials: PrincipledMaterial {
                        baseColor: "#6d4929"
                        roughness: 1
                    }
                }
            }
        }

        Repeater3D {
            model: workoutGame3D.features
            delegate: WorkoutGameAssets.Wg_Tabletop_Greybox {
                required property var modelData
                visible: modelData.kind === 10
                         && modelData.assetScaleY !== undefined
                position: Qt.vector3d(
                    modelData.assetX || 0,
                    modelData.assetY || 0,
                    modelData.assetZ || 0)
                eulerRotation: Qt.vector3d(
                    modelData.assetPitch || 0,
                    modelData.assetYaw || 0,
                    0)
                scale: Qt.vector3d(
                    1,
                    modelData.assetScaleY || 1,
                    modelData.assetScaleZ || 1)
            }
        }

        Node {
            id: rider
            position: Qt.vector3d(
                workoutGame3D.riderX,
                workoutGame3D.riderY,
                workoutGame3D.riderZ)
            eulerRotation: Qt.vector3d(
                workoutGame3D.riderPitch,
                workoutGame3D.riderYaw,
                workoutGame3D.riderRoll)

            Node {
                z: -0.52
                y: 0.40
                eulerRotation.x: workoutGame3D.pedalAngle * 2.6
                Model {
                    source: "#Cylinder"
                    eulerRotation.z: 90
                    scale: Qt.vector3d(0.0065, 0.00065, 0.0065)
                    materials: PrincipledMaterial {
                        baseColor: "#202326"
                        metalness: 0.1
                        roughness: 0.65
                    }
                }
            }
            Node {
                z: 0.52
                y: 0.40
                eulerRotation.x: workoutGame3D.pedalAngle * 2.6
                Model {
                    source: "#Cylinder"
                    eulerRotation.z: 90
                    scale: Qt.vector3d(0.0065, 0.00065, 0.0065)
                    materials: PrincipledMaterial {
                        baseColor: "#202326"
                        metalness: 0.1
                        roughness: 0.65
                    }
                }
            }
            Model {
                source: "#Cube"
                y: 0.48
                z: -0.26
                scale: Qt.vector3d(0.0007, 0.0007, 0.0054)
                eulerRotation.x: -16
                materials: PrincipledMaterial {
                    baseColor: "#e0b52f"
                    metalness: 0.35
                    roughness: 0.5
                }
            }
            Model {
                source: "#Cube"
                y: 0.75
                z: -0.06
                scale: Qt.vector3d(0.0007, 0.0007, 0.0042)
                eulerRotation.x: -107
                materials: PrincipledMaterial {
                    baseColor: "#e0b52f"
                    metalness: 0.35
                    roughness: 0.5
                }
            }
            Model {
                source: "#Cube"
                y: 0.94
                z: 0.13
                scale: Qt.vector3d(0.0007, 0.0007, 0.0050)
                eulerRotation.x: 2
                materials: PrincipledMaterial {
                    baseColor: "#e0b52f"
                    metalness: 0.35
                    roughness: 0.5
                }
            }
            Model {
                source: "#Cube"
                y: 0.74
                z: 0.19
                scale: Qt.vector3d(0.0007, 0.0007, 0.0054)
                eulerRotation.x: -45
                materials: PrincipledMaterial {
                    baseColor: "#e0b52f"
                    metalness: 0.35
                    roughness: 0.5
                }
            }
            Model {
                source: "#Cube"
                y: 0.67
                z: 0.45
                scale: Qt.vector3d(0.0007, 0.0007, 0.0055)
                eulerRotation.x: 75
                materials: PrincipledMaterial {
                    baseColor: "#d8d9d2"
                    metalness: 0.65
                    roughness: 0.35
                }
            }
            Model {
                source: "#Cube"
                y: 0.99
                z: -0.13
                scale: Qt.vector3d(0.0023, 0.00035, 0.0010)
                materials: PrincipledMaterial {
                    baseColor: "#242729"
                    roughness: 0.8
                }
            }
            Model {
                source: "#Cube"
                y: 0.96
                z: 0.41
                scale: Qt.vector3d(0.0038, 0.00035, 0.00045)
                materials: PrincipledMaterial {
                    baseColor: "#242729"
                    metalness: 0.55
                    roughness: 0.45
                }
            }
            Node {
                y: 1.23
                z: -0.03
                eulerRotation.x: 12
                Model {
                    source: "#Cylinder"
                    scale: Qt.vector3d(0.0033, 0.0062, 0.0033)
                    materials: PrincipledMaterial {
                        baseColor: "#cf3e35"
                        roughness: 0.85
                    }
                }
                Model {
                    source: "#Sphere"
                    y: 0.55
                    scale: Qt.vector3d(0.0026, 0.0029, 0.0026)
                    materials: PrincipledMaterial {
                        baseColor: "#f0c49b"
                        roughness: 0.9
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 14
        width: Math.min(parent.width - 28, 670)
        height: 62
        color: "#d9131719"
        border.color: "#55838a84"
        border.width: 1

        Row {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 22
            Text { text: Math.round(workoutGame3D.watts) + " W"; color: "white"; font.pixelSize: 19; font.bold: true }
            Text { text: "TARGET " + Math.round(workoutGame3D.targetWatts) + " W"; color: "#f0cf55"; font.pixelSize: 15 }
            Text { text: workoutGame3D.cadenceRpm + " RPM"; color: "white"; font.pixelSize: 15 }
            Text { text: workoutGame3D.heartRate + " BPM"; color: "#ff746b"; font.pixelSize: 15 }
            Text { text: workoutGame3D.speedKph.toFixed(1) + " KM/H"; color: "white"; font.pixelSize: 15 }
            Text { text: "G " + workoutGame3D.virtualGear; color: "#85d4ef"; font.pixelSize: 17; font.bold: true }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        width: 150
        height: 76
        color: "#d9131719"
        border.color: "#55838a84"
        Text {
            anchors.centerIn: parent
            text: workoutGame3D.fps.toFixed(1) + " FPS\n"
                  + root.elapsedText(workoutGame3D.workoutTimeSeconds) + "\n"
                  + (workoutGame3D.distanceMeters / 1000).toFixed(2) + " KM"
            color: "#dbe8e5"
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 14
        }
    }

    Rectangle {
        visible: workoutGame3D.featureStatus.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(parent.width - 40, 430)
        height: 58
        color: "#e3191e20"
        border.color: workoutGame3D.readinessPercent >= 100 ? "#77d07b" : "#e5c151"
        border.width: 2
        Text {
            anchors.centerIn: parent
            text: workoutGame3D.featureStatus + "  " + workoutGame3D.readinessPercent + "%"
            color: "white"
            font.pixelSize: 18
            font.bold: true
        }
    }

    Text {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 14
        text: workoutGame3D.terrainName
        color: "#e9f0e7"
        font.pixelSize: 15
    }
}
