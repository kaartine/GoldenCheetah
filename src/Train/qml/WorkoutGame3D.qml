import QtQuick
import QtQuick3D
import "assets" as WorkoutGameAssets

Item {
    id: root
    width: 1280
    height: 720

    function featureAccent(state) {
        if (state === 4) return "#ef7849"
        if (state === 5) return "#70c985"
        if (state === 6) return "#aeb8b5"
        if (state === 7) return "#e5c151"
        if (state === 3) return "#74c9e8"
        return "#e5c151"
    }

    function featureDistanceText(kind, meters) {
        if (kind === 1) return qsTr("DECISION") + "  " + meters.toFixed(1) + " M"
        if (kind === 2) return qsTr("FEATURE") + "  " + meters.toFixed(1) + " M"
        return ""
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

        Model {
            objectName: "riderGroundShadow"
            source: "#Sphere"
            position: Qt.vector3d(
                workoutGame3D.riderX,
                workoutGame3D.groundY + 0.025,
                workoutGame3D.riderZ)
            scale: Qt.vector3d(
                0.010 * Math.max(0.45,
                    1.0 - workoutGame3D.riderAirHeight * 0.55),
                0.00018,
                0.016 * Math.max(0.45,
                    1.0 - workoutGame3D.riderAirHeight * 0.55))
            opacity: Math.max(0.18,
                0.48 - workoutGame3D.riderAirHeight * 0.28)
            materials: PrincipledMaterial {
                baseColor: "#141817"
                roughness: 1
                alphaMode: PrincipledMaterial.Blend
            }
            castsShadows: false
            receivesShadows: false
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

        Model {
            objectName: "climbGeometryModel"
            geometry: workoutGame3D.climbGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                metalness: 0
                roughness: 0.94
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "rootsGeometryModel"
            geometry: workoutGame3D.rootsGeometry
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
            objectName: "rockGardenGeometryModel"
            geometry: workoutGame3D.rockGardenGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                metalness: 0
                roughness: 0.92
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "rockSlabGeometryModel"
            geometry: workoutGame3D.rockSlabGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                metalness: 0
                roughness: 0.90
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "skinnyGeometryModel"
            geometry: workoutGame3D.skinnyGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                metalness: 0
                roughness: 0.92
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "bermGeometryModel"
            geometry: workoutGame3D.bermGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                roughness: 0.95
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "bypassGeometryModel"
            geometry: workoutGame3D.bypassGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                roughness: 0.95
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
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
            delegate: WorkoutGameAssets.Wg_BunnyHop_Greybox {
                required property var modelData
                visible: modelData.kind === 5
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

        Repeater3D {
            model: workoutGame3D.features
            delegate: WorkoutGameAssets.Wg_LogOver_Greybox {
                required property var modelData
                visible: modelData.kind === 9
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

        Repeater3D {
            model: workoutGame3D.features
            delegate: WorkoutGameAssets.Wg_Drop_Greybox {
                required property var modelData
                visible: modelData.kind === 6
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
            objectName: "riderNode"
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
                id: riderBody
                objectName: "riderBodyNode"
                y: 1.23 + workoutGame3D.riderPump
                   + 0.14 * workoutGame3D.riderStandingBlend
                   - (workoutGame3D.riderWalking ? 0.10 : 0)
                z: -0.03 + 0.12 * workoutGame3D.riderStandingBlend
                eulerRotation.x: 12
                    + 8 * workoutGame3D.riderStandingBlend
                    - (workoutGame3D.riderWalking ? 5 : 0)
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

    WorkoutGameTrainingHud {
        id: trainingHud
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        height: implicitHeight
        viewModel: workoutGame3D
    }

    Rectangle {
        id: featureHud
        objectName: "featureHud"
        visible: workoutGame3D.featureHudVisible
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(parent.width - 32, 620)
        height: width < 480 ? 166 : 112
        color: "#e3191e20"
        border.color: root.featureAccent(workoutGame3D.featureState)
        border.width: 2

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            Row {
                width: parent.width
                height: 25
                spacing: 10

                Rectangle {
                    id: featureStatePill
                    width: Math.min(118, parent.width * 0.30)
                    height: 24
                    color: root.featureAccent(workoutGame3D.featureState)
                    Text {
                        objectName: "featureStateLabel"
                        anchors.centerIn: parent
                        width: parent.width - 8
                        text: workoutGame3D.featureActionText.toUpperCase()
                        color: "#101616"
                        font.pixelSize: 13
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 9
                        font.bold: true
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                Text {
                    width: Math.max(70, parent.width - featureStatePill.width
                                    - featureDistance.width - 20)
                    height: parent.height
                    text: workoutGame3D.featureName
                    color: "white"
                    font.pixelSize: 18
                    font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                Text {
                    id: featureDistance
                    objectName: "featureDistanceLabel"
                    visible: parent.width >= 430
                    width: visible ? 162 : 0
                    height: parent.height
                    text: root.featureDistanceText(
                              workoutGame3D.featureDistanceKind,
                              workoutGame3D.featureDistanceMeters)
                    color: "#dbe8e5"
                    font.pixelSize: 14
                    font.bold: true
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Grid {
                width: parent.width
                height: featureHud.width < 480 ? 106 : 49
                columns: featureHud.width < 480 ? 1 : 2
                columnSpacing: 14
                rowSpacing: 8

                Item {
                    width: featureHud.width < 480
                           ? parent.width : (parent.width - 14) / 2
                    height: parent.height

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        text: qsTr("POWER")
                        color: "#c7d2cf"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    Text {
                        objectName: "featurePowerValue"
                        anchors.right: parent.right
                        anchors.top: parent.top
                        text: workoutGame3D.powerRequired
                              ? Math.round(workoutGame3D.watts) + " / "
                                + Math.round(workoutGame3D.requiredPowerWatts) + " W"
                              : Math.round(workoutGame3D.watts) + " W  "
                                + qsTr("NO TARGET")
                        color: "white"
                        font.pixelSize: 13
                        font.bold: true
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 13
                        color: "#35403e"
                        Rectangle {
                            objectName: "featurePowerBar"
                            width: parent.width * (workoutGame3D.powerRequired
                                   ? Math.min(1,
                                      workoutGame3D.powerReadinessPercent / 100)
                                   : 1)
                            height: parent.height
                            color: !workoutGame3D.powerRequired
                                   || workoutGame3D.powerReadinessPercent >= 100
                                   ? "#70c985" : "#e5c151"
                        }
                    }
                }

                Item {
                    width: featureHud.width < 480
                           ? parent.width : (parent.width - 14) / 2
                    height: parent.height

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        text: qsTr("CADENCE")
                        color: "#c7d2cf"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    Text {
                        objectName: "featureCadenceValue"
                        anchors.right: parent.right
                        anchors.top: parent.top
                        text: workoutGame3D.cadenceRequired
                              ? workoutGame3D.cadenceRpm + " / "
                                + Math.round(workoutGame3D.requiredCadenceRpm)
                                + " RPM"
                              : workoutGame3D.cadenceRpm + " RPM  "
                                + qsTr("NO TARGET")
                        color: "white"
                        font.pixelSize: 13
                        font.bold: true
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 13
                        color: "#35403e"
                        Rectangle {
                            objectName: "featureCadenceBar"
                            width: parent.width * (workoutGame3D.cadenceRequired
                                   ? Math.min(1,
                                      workoutGame3D.cadenceReadinessPercent / 100)
                                   : 1)
                            height: parent.height
                            color: !workoutGame3D.cadenceRequired
                                   || workoutGame3D.cadenceReadinessPercent >= 100
                                   ? "#70c985" : "#74c9e8"
                        }
                    }
                }
            }
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
