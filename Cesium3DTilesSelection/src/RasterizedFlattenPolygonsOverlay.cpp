#include "Cesium3DTilesSelection/RasterizedFlattenPolygonsOverlay.h"

#include "Cesium3DTilesSelection/BoundingVolume.h"
#include "Cesium3DTilesSelection/RasterOverlayTileProvider.h"
#include "Cesium3DTilesSelection/spdlog-cesium.h"
#include "TileUtilities.h"

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumGeospatial/GlobeRectangle.h>
#include <CesiumUtility/IntrusivePointer.h>

#include <memory>
#include <string>

using namespace CesiumGeometry;
using namespace CesiumGeospatial;
using namespace CesiumUtility;

namespace Cesium3DTilesSelection {
namespace {

uint32_t float32ToFloat24(float value) {
  uint32_t float32Bits = *(uint32_t*)&value;
  uint32_t float24Bits;

  // Extract sign bit, exponent bits, and mantissa bits
  uint32_t sign = (float32Bits >> 31) & 0x1;
  uint32_t exponent = (float32Bits >> 23) & 0xFF;
  uint32_t mantissa = (float32Bits & 0x7FFFFF);

  // Adjust exponent bits
  if (value != 0) { // Skip adjustment for zero
    exponent = exponent - 127 + 63;
  }

  // Prevent overflow and underflow
  if (exponent > 127) {
    // Overflow, set to maximum value
    exponent = 127;
    mantissa = 0xFFFFF;
  } else if (exponent < 0) {
    // Underflow, set to minimum value
    exponent = 0;
    mantissa = 0;
  }

  // Combine sign, exponent, and mantissa bits into float24
  float24Bits = (sign << 23) | (exponent << 15) |
                (mantissa >> 8); // Shift right by 8 bits to fit 15 bits

  return float24Bits;
}

// Convert float24 to float32
float float24ToFloat32(uint32_t float24Bits) {
  uint32_t sign = (float24Bits >> 23) & 0x1;
  uint32_t exponent = (float24Bits >> 15) & 0xFF;
  uint32_t mantissa = (float24Bits & 0x7FFF)
                      << 8; // Shift left by 8 bits to restore 23 bits

  uint32_t float32Bits = 0;

  if (exponent == 0) {
    // Denormalized number
    if (mantissa != 0) {
      uint32_t shift = 23 - 15;
      mantissa <<= shift;
      while ((mantissa & 0x800000) == 0) {
        mantissa <<= 1;
        exponent--;
      }
      mantissa &= 0x7FFFFF; // Clear the leading 1
    }
  } else if (exponent == 127) {
    // Infinity or NaN
    float32Bits = (sign << 31) | 0x7F800000 | mantissa;
  } else {
    // Normalized number
    exponent = exponent + (127 - 63);
    float32Bits = (sign << 31) | (exponent << 23) | mantissa;
  }

  return *(float*)&float32Bits;
}

void rasterizePolygons(
    LoadedRasterOverlayImage& loaded,
    const CesiumGeospatial::GlobeRectangle& rectangle,
    const glm::dvec2& textureSize,
    const std::vector<CartographicPolygon>& cartographicPolygons,
    const std::vector<float>& flattenHeights) {

  CesiumGltf::ImageCesium& image = loaded.image.emplace();

  std::byte insideColorBytes = static_cast<std::byte>(0xff);
  std::byte outsideColorBytes = static_cast<std::byte>(0);

  std::tuple<bool, int> withinPolygonsResult =
      Cesium3DTilesSelection::CesiumImpl::withinPolygonsAndReturnIndex(
          rectangle,
          cartographicPolygons);
  // create a 1x1 mask if the rectangle is completely inside a polygon
  if (std::get<0>(withinPolygonsResult)) {
    loaded.moreDetailAvailable = false;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.bytesPerChannel = 1;
    image.pixelData.resize(image.channels * image.bytesPerChannel);
    uint32_t flattenHeightBytes =
        float32ToFloat24(flattenHeights[std::get<1>(withinPolygonsResult)]);
    image.pixelData.resize(image.channels * image.bytesPerChannel);
    image.pixelData[3] = insideColorBytes;
    image.pixelData[0] = std::byte(flattenHeightBytes >> 16 & 0xFF);
    image.pixelData[1] = std::byte(flattenHeightBytes >> 8 & 0xFF);
    image.pixelData[2] = std::byte(flattenHeightBytes & 0xFF);
    return;
  }

  bool completelyOutsidePolygons = true;
  for (const CartographicPolygon& selection : cartographicPolygons) {
    const std::optional<CesiumGeospatial::GlobeRectangle>& boundingRectangle =
        selection.getBoundingRectangle();

    if (boundingRectangle &&
        rectangle.computeIntersection(*boundingRectangle)) {
      completelyOutsidePolygons = false;
      break;
    }
  }

  // create a 1x1 mask if the rectangle is completely outside all polygons
  if (completelyOutsidePolygons) {
    loaded.moreDetailAvailable = false;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.bytesPerChannel = 1;
    // uint32_t flattenHeightBytes =
    //     float32ToFloat24(flattenHeights[std::get<1>(withinPolygonsResult)]);
    image.pixelData.resize(image.channels * image.bytesPerChannel);
    image.pixelData[3] = outsideColorBytes;
    // image.pixelData[0] = std::byte(flattenHeightBytes >> 16 & 0xFF);
    // image.pixelData[1] = std::byte(flattenHeightBytes >> 8 & 0xFF);
    // image.pixelData[2] = std::byte(flattenHeightBytes & 0xFF);

    image.pixelData[0] = std::byte(0);
    image.pixelData[1] = std::byte(0);
    image.pixelData[2] = std::byte(0);
    return;
  }

  const double rectangleWidth = rectangle.computeWidth();
  const double rectangleHeight = rectangle.computeHeight();

  // create source image
  loaded.moreDetailAvailable = true;
  image.width = int32_t(glm::round(textureSize.x));
  image.height = int32_t(glm::round(textureSize.y));
  image.channels = 4;
  image.bytesPerChannel = 1;
  image.pixelData.resize(
      size_t(
          image.width * image.height * image.channels * image.bytesPerChannel),
      std::byte(0));

  // TODO: this is naive approach, use line-triangle
  // intersections to rasterize one row at a time
  // NOTE: also completely ignores antimeridian (really these
  // calculations should be normalized to the first vertex)
  int polygonIndex = -1;
  for (const CartographicPolygon& polygon : cartographicPolygons) {
    polygonIndex++;
    const std::vector<glm::dvec2>& vertices = polygon.getVertices();
    const std::vector<uint32_t>& indices = polygon.getIndices();
    for (size_t triangle = 0; triangle < indices.size() / 3; ++triangle) {
      const glm::dvec2& a = vertices[indices[3 * triangle]];
      const glm::dvec2& b = vertices[indices[3 * triangle + 1]];
      const glm::dvec2& c = vertices[indices[3 * triangle + 2]];

      // TODO: deal with the corner cases here
      const double minX = glm::min(a.x, glm::min(b.x, c.x));
      const double minY = glm::min(a.y, glm::min(b.y, c.y));
      const double maxX = glm::max(a.x, glm::max(b.x, c.x));
      const double maxY = glm::max(a.y, glm::max(b.y, c.y));

      const CesiumGeospatial::GlobeRectangle triangleBounds(
          minX,
          minY,
          maxX,
          maxY);

      // skip this triangle if it is entirely outside the tile bounds
      if (!rectangle.computeIntersection(triangleBounds)) {
        continue;
      }

      const glm::dvec2 ab = b - a;
      const glm::dvec2 ab_perp(-ab.y, ab.x);
      const glm::dvec2 bc = c - b;
      const glm::dvec2 bc_perp(-bc.y, bc.x);
      const glm::dvec2 ca = a - c;
      const glm::dvec2 ca_perp(-ca.y, ca.x);

      size_t width = size_t(image.width);
      size_t height = size_t(image.height);

      for (size_t j = 0; j < height; ++j) {
        const double pixelY =
            rectangle.getSouth() +
            rectangleHeight * (1.0 - (double(j) + 0.5) / double(height));
        for (size_t i = 0; i < width; ++i) {
          const double pixelX = rectangle.getWest() + rectangleWidth *
                                                          (double(i) + 0.5) /
                                                          double(width);
          const glm::dvec2 v(pixelX, pixelY);

          const glm::dvec2 av = v - a;
          const glm::dvec2 cv = v - c;

          const double v_proj_ab_perp = glm::dot(av, ab_perp);
          const double v_proj_bc_perp = glm::dot(cv, bc_perp);
          const double v_proj_ca_perp = glm::dot(cv, ca_perp);

          // will determine in or out, irrespective of winding
          if ((v_proj_ab_perp >= 0.0 && v_proj_ca_perp >= 0.0 &&
               v_proj_bc_perp >= 0.0) ||
              (v_proj_ab_perp <= 0.0 && v_proj_ca_perp <= 0.0 &&
               v_proj_bc_perp <= 0.0)) {
            uint32_t flattenHeightBytes =
                float32ToFloat24(flattenHeights[polygonIndex]);

            image.pixelData[(width * j + i) * 4 + 3] = insideColorBytes;
            image.pixelData[(width * j + i) * 4 + 0] =
                std::byte(flattenHeightBytes >> 16 & 0xFF);
            image.pixelData[(width * j + i) * 4 + 1] =
                std::byte(flattenHeightBytes >> 8 & 0xFF);
            image.pixelData[(width * j + i) * 4 + 2] =
                std::byte(flattenHeightBytes & 0xFF);
          }
        }
      }
    }
  }
}
} // namespace

class CESIUM3DTILESSELECTION_API RasterizedFlattenPolygonsTileProvider final
    : public RasterOverlayTileProvider {

private:
  std::vector<CartographicPolygon> _polygons;
  std::vector<float> _flattenHeights;

public:
  RasterizedFlattenPolygonsTileProvider(
      const IntrusivePointer<const RasterOverlay>& pOwner,
      const CesiumAsync::AsyncSystem& asyncSystem,
      const std::shared_ptr<CesiumAsync::IAssetAccessor>& pAssetAccessor,
      const std::shared_ptr<IPrepareRendererResources>&
          pPrepareRendererResources,
      const std::shared_ptr<spdlog::logger>& pLogger,
      const CesiumGeospatial::Projection& projection,
      const std::vector<CartographicPolygon>& polygons,
      const std::vector<float>& flattenHeights)
      : RasterOverlayTileProvider(
            pOwner,
            asyncSystem,
            pAssetAccessor,
            std::nullopt,
            pPrepareRendererResources,
            pLogger,
            projection,
            // computeCoverageRectangle(projection, polygons)),
            projectRectangleSimple(
                projection,
                CesiumGeospatial::GlobeRectangle(
                    -CesiumUtility::Math::OnePi,
                    -CesiumUtility::Math::PiOverTwo,
                    CesiumUtility::Math::OnePi,
                    CesiumUtility::Math::PiOverTwo))),
        _polygons(polygons),
        _flattenHeights(flattenHeights) {}

  virtual CesiumAsync::Future<LoadedRasterOverlayImage>
  loadTileImage(RasterOverlayTile& overlayTile) override {
    // Choose the texture size according to the geometry screen size and raster
    // SSE, but no larger than the maximum texture size.
    const RasterOverlayOptions& options = this->getOwner().getOptions();
    glm::dvec2 textureSize = glm::min(
        overlayTile.getTargetScreenPixels() / options.maximumScreenSpaceError,
        glm::dvec2(options.maximumTextureSize));

    return this->getAsyncSystem().runInWorkerThread(
        [&polygons = this->_polygons,
         flattenHeights = this->_flattenHeights,
         projection = this->getProjection(),
         rectangle = overlayTile.getRectangle(),
         textureSize]() -> LoadedRasterOverlayImage {
          const CesiumGeospatial::GlobeRectangle tileRectangle =
              CesiumGeospatial::unprojectRectangleSimple(projection, rectangle);

          LoadedRasterOverlayImage result;
          result.rectangle = rectangle;

          rasterizePolygons(
              result,
              tileRectangle,
              textureSize,
              polygons,
              flattenHeights);

          return result;
        });
  }
};

RasterizedFlattenPolygonsOverlay::RasterizedFlattenPolygonsOverlay(
    const std::string& name,
    const std::vector<CartographicPolygon>& polygons,
    const std::vector<float>& flattenHeights,
    const CesiumGeospatial::Ellipsoid& ellipsoid,
    const CesiumGeospatial::Projection& projection,
    const RasterOverlayOptions& overlayOptions)
    : RasterOverlay(name, overlayOptions),
      _polygons(polygons),
      _flattenHeights(flattenHeights),
      _ellipsoid(ellipsoid),
      _projection(projection) {}

RasterizedFlattenPolygonsOverlay::~RasterizedFlattenPolygonsOverlay() {}

CesiumAsync::Future<RasterOverlay::CreateTileProviderResult>
RasterizedFlattenPolygonsOverlay::createTileProvider(
    const CesiumAsync::AsyncSystem& asyncSystem,
    const std::shared_ptr<CesiumAsync::IAssetAccessor>& pAssetAccessor,
    const std::shared_ptr<CreditSystem>& /*pCreditSystem*/,
    const std::shared_ptr<IPrepareRendererResources>& pPrepareRendererResources,
    const std::shared_ptr<spdlog::logger>& pLogger,
    CesiumUtility::IntrusivePointer<const RasterOverlay> pOwner) const {

  pOwner = pOwner ? pOwner : this;

  return asyncSystem.createResolvedFuture<CreateTileProviderResult>(
      IntrusivePointer<RasterOverlayTileProvider>(
          new RasterizedFlattenPolygonsTileProvider(
              pOwner,
              asyncSystem,
              pAssetAccessor,
              pPrepareRendererResources,
              pLogger,
              this->_projection,
              this->_polygons,
              this->_flattenHeights)));
}

} // namespace Cesium3DTilesSelection
