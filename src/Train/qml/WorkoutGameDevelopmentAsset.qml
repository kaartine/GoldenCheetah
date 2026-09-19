import QtQuick3D
import QtQuick3D.AssetUtils

Node {
    id: root
    required property string assetId
    property string variantKey: ""
    property var instanceTable: null
    property bool allowed: true
    readonly property var assetService:
        typeof workoutGameDevelopmentAssets === "undefined"
        ? null : workoutGameDevelopmentAssets
    readonly property url requestedSource:
        root.allowed && root.assetService !== null && root.assetService.enabled
        ? root.assetService.sourceForAsset(
              root.assetId,
              root.variantKey,
              root.assetService.revision)
        : ""
    readonly property bool ready:
        loader.status === RuntimeLoader.Success
        && root.requestedSource.toString().length > 0

    RuntimeLoader {
        id: loader
        objectName: "workoutGameDevelopmentRuntimeLoader"
        source: root.requestedSource
        instancing: root.instanceTable
        visible: root.ready
        onStatusChanged: {
            if (status === RuntimeLoader.Error) {
                if (root.assetService !== null) {
                    root.assetService.reportRuntimeError(
                        root.assetId, errorString)
                }
            }
        }
    }
}
