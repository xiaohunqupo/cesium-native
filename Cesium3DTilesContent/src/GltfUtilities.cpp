#include <Cesium3DTilesContent/GltfUtilities.h>
#include <Cesium3DTilesContent/SkirtMeshMetadata.h>
#include <CesiumGeometry/Axis.h>
#include <CesiumGeometry/Transforms.h>
#include <CesiumGeospatial/BoundingRegionBuilder.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ExtensionCesiumRTC.h>

#include <vector>

namespace Cesium3DTilesContent {
/*static*/ glm::dmat4x4 GltfUtilities::applyRtcCenter(
    const CesiumGltf::Model& gltf,
    const glm::dmat4x4& rootTransform) {
  const CesiumGltf::ExtensionCesiumRTC* cesiumRTC =
      gltf.getExtension<CesiumGltf::ExtensionCesiumRTC>();
  if (cesiumRTC == nullptr) {
    return rootTransform;
  }
  const std::vector<double>& rtcCenter = cesiumRTC->center;
  if (rtcCenter.size() != 3) {
    return rootTransform;
  }
  const double x = rtcCenter[0];
  const double y = rtcCenter[1];
  const double z = rtcCenter[2];
  const glm::dmat4x4 rtcTransform(
      glm::dvec4(1.0, 0.0, 0.0, 0.0),
      glm::dvec4(0.0, 1.0, 0.0, 0.0),
      glm::dvec4(0.0, 0.0, 1.0, 0.0),
      glm::dvec4(x, y, z, 1.0));
  return rootTransform * rtcTransform;
}

/*static*/ glm::dmat4x4 GltfUtilities::applyGltfUpAxisTransform(
    const CesiumGltf::Model& model,
    const glm::dmat4x4& rootTransform) {
  auto gltfUpAxisIt = model.extras.find("gltfUpAxis");
  if (gltfUpAxisIt == model.extras.end()) {
    // The default up-axis of glTF is the Y-axis, and no other
    // up-axis was specified. Transform the Y-axis to the Z-axis,
    // to match the 3D Tiles specification
    return rootTransform * CesiumGeometry::Transforms::Y_UP_TO_Z_UP;
  }
  const CesiumUtility::JsonValue& gltfUpAxis = gltfUpAxisIt->second;
  int gltfUpAxisValue = static_cast<int>(gltfUpAxis.getSafeNumberOrDefault(1));
  if (gltfUpAxisValue == static_cast<int>(CesiumGeometry::Axis::X)) {
    return rootTransform * CesiumGeometry::Transforms::X_UP_TO_Z_UP;
  } else if (gltfUpAxisValue == static_cast<int>(CesiumGeometry::Axis::Y)) {
    return rootTransform * CesiumGeometry::Transforms::Y_UP_TO_Z_UP;
  } else if (gltfUpAxisValue == static_cast<int>(CesiumGeometry::Axis::Z)) {
    // No transform required
  }
  return rootTransform;
}

/*static*/ CesiumGeospatial::BoundingRegion
GltfUtilities::computeBoundingRegion(
    const CesiumGltf::Model& gltf,
    const glm::dmat4& transform) {
  glm::dmat4 rootTransform = transform;
  rootTransform = applyRtcCenter(gltf, rootTransform);
  rootTransform = applyGltfUpAxisTransform(gltf, rootTransform);

  // When computing the tile's bounds, ignore tiles that are less than 1/1000th
  // of a tile width from the North or South pole. Longitudes cannot be trusted
  // at such extreme latitudes.
  CesiumGeospatial::BoundingRegionBuilder computedBounds;

  gltf.forEachPrimitiveInScene(
      -1,
      [&rootTransform, &computedBounds](
          const CesiumGltf::Model& gltf_,
          const CesiumGltf::Node& /*node*/,
          const CesiumGltf::Mesh& /*mesh*/,
          const CesiumGltf::MeshPrimitive& primitive,
          const glm::dmat4& nodeTransform) {
        auto positionIt = primitive.attributes.find("POSITION");
        if (positionIt == primitive.attributes.end()) {
          return;
        }

        const int positionAccessorIndex = positionIt->second;
        if (positionAccessorIndex < 0 ||
            positionAccessorIndex >= static_cast<int>(gltf_.accessors.size())) {
          return;
        }

        const glm::dmat4 fullTransform = rootTransform * nodeTransform;

        const CesiumGltf::AccessorView<glm::vec3> positionView(
            gltf_,
            positionAccessorIndex);
        if (positionView.status() != CesiumGltf::AccessorViewStatus::Valid) {
          return;
        }

        std::optional<SkirtMeshMetadata> skirtMeshMetadata =
            SkirtMeshMetadata::parseFromGltfExtras(primitive.extras);
        int64_t vertexBegin, vertexEnd;
        if (skirtMeshMetadata.has_value()) {
          vertexBegin = skirtMeshMetadata->noSkirtVerticesBegin;
          vertexEnd = skirtMeshMetadata->noSkirtVerticesBegin +
                      skirtMeshMetadata->noSkirtVerticesCount;
        } else {
          vertexBegin = 0;
          vertexEnd = positionView.size();
        }

        for (int64_t i = vertexBegin; i < vertexEnd; ++i) {
          // Get the ECEF position
          const glm::vec3 position = positionView[i];
          const glm::dvec3 positionEcef =
              glm::dvec3(fullTransform * glm::dvec4(position, 1.0));

          // Convert it to cartographic
          std::optional<CesiumGeospatial::Cartographic> cartographic =
              CesiumGeospatial::Ellipsoid::WGS84.cartesianToCartographic(
                  positionEcef);
          if (!cartographic) {
            continue;
          }

          computedBounds.expandToIncludePosition(*cartographic);
        }
      });

  return computedBounds.toRegion();
}

std::vector<std::string_view>
GltfUtilities::parseGltfCopyright(const CesiumGltf::Model& gltf) {
  std::vector<std::string_view> result;
  if (gltf.asset.copyright) {
    const std::string_view copyright = *gltf.asset.copyright;
    if (copyright.size() > 0) {
      size_t start = 0;
      size_t end;
      size_t ltrim;
      size_t rtrim;
      do {
        ltrim = copyright.find_first_not_of(" \t", start);
        end = copyright.find(';', ltrim);
        rtrim = copyright.find_last_not_of(" \t", end - 1);
        result.emplace_back(copyright.substr(ltrim, rtrim - ltrim + 1));
        start = end + 1;
      } while (end != std::string::npos);
    }
  }

  return result;
}
} // namespace Cesium3DTilesContent
