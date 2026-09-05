import QtQuick
import QtQuick3D
import "assets" as WorkoutGameAssets

Item {
    id: root
    width: 1280
    height: 720
    property bool sessionRunning: false
    property bool rendererPrewarming: false

    FrameAnimation {
        objectName: "presentationFrameAnimation"
        running: (root.sessionRunning || root.rendererPrewarming) && root.visible
    }

    readonly property vector3d riderGroundScreen: {
        if (gameView.width <= 0 || gameView.height <= 0)
            return Qt.vector3d(0, 0, 0)
        return gameView.mapFrom3DScene(Qt.vector3d(
            workoutGame3D.riderX,
            workoutGame3D.riderY - workoutGame3D.riderAirHeight,
            workoutGame3D.riderZ))
    }
    readonly property real chaseCameraFieldOfView:
        workoutGame3D.riderPoseState === "preload" ? 46.35
        : workoutGame3D.riderPoseState === "air"
          ? 47 + Math.min(1.3,
                          0.6 + workoutGame3D.riderAirHeight * 0.45)
        : workoutGame3D.riderPoseState === "land"
          ? 47 + workoutGame3D.landingImpact * 1.1
        : 47
    readonly property real cameraFieldOfView: chaseCameraFieldOfView
        - 6 * workoutGame3D.cameraPresentationBlend
    readonly property var riderWheelFrustumScreenPoints: {
        // Keep the projection binding dependent on every camera input.
        const cameraState = workoutGame3D.cameraX + workoutGame3D.cameraY
            + workoutGame3D.cameraZ + workoutGame3D.cameraTargetX
            + workoutGame3D.cameraTargetY + workoutGame3D.cameraTargetZ
            + root.cameraFieldOfView + workoutGame3D.riderX
            + workoutGame3D.riderY + workoutGame3D.riderZ
            + workoutGame3D.riderPitch + workoutGame3D.riderYaw
            + workoutGame3D.riderRoll
        if (!Number.isFinite(cameraState)) return []
        return rider.wheelFrustumScenePoints().map(function(point) {
            return gameView.mapFrom3DScene(point)
        })
    }

    function featureAccent(state) {
        if (state === 8) return "#ef7849"
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
        if (kind === 3) return qsTr("LAUNCH") + "  " + meters.toFixed(1) + " M"
        return ""
    }

    function treeEdgeOpacity(relativeMeters) {
        const behind = Math.max(0, Math.min(1, (relativeMeters + 18) / 6))
        const ahead = Math.max(0, Math.min(1, (29 - relativeMeters) / 10))
        return Math.min(behind, ahead)
    }

    function dressingEdgeOpacity(relativeMeters) {
        const behind = Math.max(0, Math.min(1, (relativeMeters + 14) / 4))
        const ahead = Math.max(0, Math.min(1, (44 - relativeMeters) / 8))
        return Math.min(behind, ahead)
    }

    View3D {
        id: gameView
        objectName: "workoutGame3DView"
        anchors.fill: parent
        camera: camera

        Texture {
            id: forestSurfaceTexture
            source: "qrc:/images/workout-game-surface-forest.png"
            tilingModeHorizontal: Texture.Repeat
            tilingModeVertical: Texture.Repeat
            minFilter: Texture.Linear
            magFilter: Texture.Nearest
            generateMipmaps: true
        }
        Texture {
            id: dirtSurfaceTexture
            source: "qrc:/images/workout-game-surface-dirt.png"
            tilingModeHorizontal: Texture.Repeat
            tilingModeVertical: Texture.Repeat
            minFilter: Texture.Linear
            magFilter: Texture.Nearest
            generateMipmaps: true
        }
        Texture {
            id: stoneSurfaceTexture
            source: "qrc:/images/workout-game-surface-stone.png"
            tilingModeHorizontal: Texture.Repeat
            tilingModeVertical: Texture.Repeat
            minFilter: Texture.Linear
            magFilter: Texture.Nearest
            generateMipmaps: true
        }
        Texture {
            id: woodSurfaceTexture
            source: "qrc:/images/workout-game-surface-wood.png"
            tilingModeHorizontal: Texture.Repeat
            tilingModeVertical: Texture.Repeat
            minFilter: Texture.Linear
            magFilter: Texture.Nearest
            generateMipmaps: true
        }

        Component.onCompleted: {
            renderStats.extendedDataCollectionEnabled =
                    workoutGame3D.extendedRenderStatsEnabled
        }

        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: "#78a9bf"
            fog: Fog {
                objectName: "workoutGameDepthFog"
                enabled: true
                color: "#78a9bf"
                density: 0.55
                depthEnabled: true
                depthNear: 68
                depthFar: 260
                depthCurve: 1.3
                heightEnabled: false
                transmitEnabled: false
            }
            antialiasingMode: SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
            temporalAAEnabled: false
        }

        PerspectiveCamera {
            id: camera
            objectName: "workoutGameCamera"
            fieldOfView: root.cameraFieldOfView
            clipNear: 0.15
            clipFar: 650
            position: Qt.vector3d(
                workoutGame3D.cameraX,
                workoutGame3D.cameraY,
                workoutGame3D.cameraZ)
            lookAtNode: cameraTarget
            Behavior on fieldOfView {
                NumberAnimation {
                    duration: 120
                    easing.type: Easing.OutCubic
                }
            }
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

        WorkoutGameDistantTerrain {
            position: Qt.vector3d(
                workoutGame3D.riderX,
                workoutGame3D.groundY - 1.2,
                workoutGame3D.riderZ)
            surfaceTexture: forestSurfaceTexture
        }

        Model {
            geometry: workoutGame3D.floorGeometry
            materials: PrincipledMaterial {
                baseColor: "white"
                baseColorMap: forestSurfaceTexture
                roughness: 1
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Model {
            objectName: "forestDressingModel"
            geometry: workoutGame3D.forestDressingGeometry
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
                baseColorMap: dirtSurfaceTexture
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
                baseColorMap: stoneSurfaceTexture
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
                baseColorMap: woodSurfaceTexture
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
                baseColorMap: stoneSurfaceTexture
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
                baseColorMap: stoneSurfaceTexture
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
                baseColorMap: woodSurfaceTexture
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
                baseColorMap: dirtSurfaceTexture
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
                baseColorMap: dirtSurfaceTexture
                roughness: 0.95
                vertexColorsEnabled: true
                lighting: PrincipledMaterial.FragmentLighting
                cullMode: Material.NoCulling
            }
            castsShadows: false
            receivesShadows: false
        }

        Repeater3D {
            model: workoutGame3D.treeRenderModel
            delegate: Node {
                required property var modelData
                objectName: "workoutGameTree"
                readonly property string vegetationId: modelData.stableId
                readonly property real relativeDistance:
                    modelData.distance - workoutGame3D.distanceMeters
                readonly property real targetOpacity:
                    root.treeEdgeOpacity(relativeDistance)
                property real presentedOpacity: 0
                position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                scale: Qt.vector3d(modelData.scale, modelData.scale, modelData.scale)
                opacity: presentedOpacity

                Component.onCompleted: presentedOpacity = targetOpacity
                onTargetOpacityChanged: presentedOpacity = targetOpacity

                Behavior on presentedOpacity {
                    NumberAnimation {
                        duration: 320
                        easing.type: Easing.OutCubic
                    }
                }

                WorkoutGameConifer {
                    variant: modelData.variant
                }
            }
        }

        Repeater3D {
            model: workoutGame3D.forestFloorRenderModel
            delegate: WorkoutGameForestFloorProp {
                required property var modelData
                variant: modelData.variant
                readonly property string vegetationId: modelData.stableId
                readonly property real relativeDistance:
                    modelData.distance - workoutGame3D.distanceMeters
                position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                eulerRotation: Qt.vector3d(
                    modelData.pitch, modelData.yaw, modelData.terrainRoll)
                scale: Qt.vector3d(
                    modelData.mirror ? -modelData.scale : modelData.scale,
                    modelData.scale,
                    modelData.scale)
                opacity: root.dressingEdgeOpacity(relativeDistance)
            }
        }

        Repeater3D {
            model: workoutGame3D.forestVergeRenderModel
            delegate: WorkoutGameForestVergeCluster {
                required property var modelData
                variant: modelData.variant
                readonly property string vegetationId: modelData.stableId
                readonly property real relativeDistance:
                    modelData.distance - workoutGame3D.distanceMeters
                position: Qt.vector3d(modelData.x, modelData.y, modelData.z)
                eulerRotation: Qt.vector3d(
                    modelData.pitch, modelData.yaw, modelData.terrainRoll)
                scale: Qt.vector3d(
                    modelData.mirror ? -modelData.scale : modelData.scale,
                    modelData.scale,
                    modelData.scale)
                opacity: root.dressingEdgeOpacity(relativeDistance)
            }
        }

        Repeater3D {
            model: workoutGame3D.gapJumpFeatures
            delegate: WorkoutGameAssets.Wg_GapJumpThreeLine {
                required property var modelData
                objectName: "gapJumpAssetInstance"
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

        WorkoutGameRiderBike {
            id: rider
            position: Qt.vector3d(
                workoutGame3D.riderX,
                workoutGame3D.riderY,
                workoutGame3D.riderZ)
            distanceMeters: workoutGame3D.distanceMeters
            pedalAngle: workoutGame3D.pedalAngle
            airHeight: workoutGame3D.riderAirHeight
            pump: workoutGame3D.riderPump
            standingBlend: workoutGame3D.riderStandingBlend
            pedalEffort: workoutGame3D.riderPedalEffort
            rearSuspensionCompression:
                workoutGame3D.rearSuspensionCompression
            frontSuspensionCompression:
                workoutGame3D.frontSuspensionCompression
            walking: workoutGame3D.riderWalking
            poseState: workoutGame3D.riderPoseState
            // Box2D and Qt Quick 3D use opposite signs for nose-up pitch.
            riderPitch: -workoutGame3D.riderPitch
            riderYaw: workoutGame3D.riderYaw
            riderRoll: workoutGame3D.riderRoll
        }

        DirectionalLight {
            objectName: "riderReadabilityLight"
            scope: rider
            eulerRotation: Qt.vector3d(-34, 148, 0)
            color: "#d9e2de"
            ambientColor: "#35413d"
            brightness: 0.62
            castsShadow: false
        }
    }

    WorkoutGameLandingDust {
        x: Number.isFinite(root.riderGroundScreen.x)
           ? root.riderGroundScreen.x - width / 2 : -width
        y: Number.isFinite(root.riderGroundScreen.y)
           ? root.riderGroundScreen.y - height * 0.58 : -height
        triggerId: workoutGame3D.landingEffectId
        strength: workoutGame3D.landingEffectStrength
    }

    WorkoutGameSuccessFeedback {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: trainingHud.y + trainingHud.height
                           + (diagnosticHud.visible
                              ? diagnosticHud.height + 12 : 8)
        triggerId: workoutGame3D.successEffectId
        effectText: workoutGame3D.successEffectText
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
        id: diagnosticHud
        objectName: "diagnosticHud"
        visible: workoutGame3D.diagnosticsEnabled
        anchors.left: trainingHud.left
        anchors.right: trainingHud.right
        anchors.top: trainingHud.bottom
        anchors.topMargin: 6
        height: 26
        color: "#e60a1012"
        border.color: "#77838a84"
        border.width: 1

        Text {
            objectName: "diagnosticText"
            anchors.fill: parent
            anchors.leftMargin: 7
            anchors.rightMargin: 7
            text: workoutGame3D.diagnosticsText
            color: "#dbe8e5"
            font.pixelSize: 11
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 7
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
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
                    objectName: "featureNameLabel"
                    width: Math.max(70, parent.width - featureStatePill.width
                                    - featureDistance.width - 20)
                    height: parent.height
                    text: workoutGame3D.featureName
                    color: "white"
                    font.pixelSize: 18
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 11
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
