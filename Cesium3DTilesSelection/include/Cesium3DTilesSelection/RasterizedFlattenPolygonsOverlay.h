#pragma once

#include "Library.h"
#include "RasterOverlay.h"
#include "RasterOverlayTileProvider.h"

#include <CesiumAsync/AsyncSystem.h>
#include <CesiumGeospatial/CartographicPolygon.h>
#include <CesiumGeospatial/Ellipsoid.h>
#include <CesiumGeospatial/Projection.h>

#include <spdlog/fwd.h>

#include <memory>
#include <string>
#include <vector>

namespace Cesium3DTilesSelection {

class CESIUM3DTILESSELECTION_API RasterizedFlattenPolygonsOverlay final
    : public RasterOverlay {

public:
  RasterizedFlattenPolygonsOverlay(
      const std::string& name,
      const std::vector<CesiumGeospatial::CartographicPolygon>& polygons,
      const std::vector<float>& flattenHeights,
      const CesiumGeospatial::Ellipsoid& ellipsoid,
      const CesiumGeospatial::Projection& projection,
      const RasterOverlayOptions& overlayOptions = {});
  virtual ~RasterizedFlattenPolygonsOverlay() override;

  virtual CesiumAsync::Future<CreateTileProviderResult> createTileProvider(
      const CesiumAsync::AsyncSystem& asyncSystem,
      const std::shared_ptr<CesiumAsync::IAssetAccessor>& pAssetAccessor,
      const std::shared_ptr<CreditSystem>& pCreditSystem,
      const std::shared_ptr<IPrepareRendererResources>&
          pPrepareRendererResources,
      const std::shared_ptr<spdlog::logger>& pLogger,
      CesiumUtility::IntrusivePointer<const RasterOverlay> pOwner)
      const override;

  const std::vector<CesiumGeospatial::CartographicPolygon>&
  getPolygons() const noexcept {
    return this->_polygons;
  }

  std::vector<float> getFlattenHeights() const noexcept {
    return _flattenHeights;
  };

private:
  std::vector<CesiumGeospatial::CartographicPolygon> _polygons;
  std::vector<float> _flattenHeights;
  CesiumGeospatial::Ellipsoid _ellipsoid;
  CesiumGeospatial::Projection _projection;
};
} // namespace Cesium3DTilesSelection
