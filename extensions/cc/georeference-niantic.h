/*
MIT License

Copyright (c) 2026 Niantic Labs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef SPZ_GEOREFERENCE_NIANTIC_H_
#define SPZ_GEOREFERENCE_NIANTIC_H_

#include <array>
#include <limits>
#include <string>

#include "splat-extensions.h"

namespace spz {

// Records a similarity transform that georeferences the asset: positions in the asset's local
// frame map into a body-fixed geocentric CRS (identified by crsId, e.g. "EPSG:4978" = WGS84 ECEF)
// as
//
//   p_crs = scale * (q * p_local * q^-1) + origin
//
// where q is `rotation` (unit quaternion), `scale` is a dimensionless uniform factor, and `origin`
// is in meters. The transform applies to positions after any coordinate-system extension
// resolution; if SPZ_ADOBE_coordinate_system is absent, the local frame is the SPZ default (RUB).
// There is no runtime dependency on the coordinate-system extension — the interaction is
// spec-level only.
//
// crsId is authority-qualified rather than a bare EPSG code because the EPSG registry is
// Earth-only; other bodies are registered elsewhere (e.g. "IAU_2015:30100" for the Moon).
//
// `scale` is uniform (isotropic) only, matching the classic 7-parameter Helmert/similarity
// transform used for local-to-global CRS registration: it corrects for the scale ambiguity
// inherent to unreferenced reconstruction (e.g. monocular SfM), which is always isotropic under a
// calibrated-camera model. There is no anisotropic scale or shear field, and none should be
// added — those describe a different problem (a deformed local frame) that this extension does
// not address.
//
// Post-transform heights are ellipsoidal by construction. `wkt` optionally records the source
// compound CRS (including vertical datum) as provenance only; readers must not use it to
// transform data.
//
// This extension is a descriptor: the library never applies the transform to Gaussian data.
struct SpzExtensionGeoreferenceNiantic : public SpzExtensionBase {
  static constexpr uint8_t kExtVersion = 1;
  static constexpr uint8_t kFlagHasEpoch = 1 << 0;
  static constexpr uint8_t kFlagHasWkt = 1 << 1;
  // Maximum WKT size accepted on write. Keeps the u32 record length far from overflow; real WKT2
  // strings are a few KB. An oversized wkt is omitted (with a warning), never truncated.
  static constexpr uint32_t kMaxWktBytes = 1u << 20;
  static constexpr uint16_t kMaxCrsIdBytes = 256;

  // Target CRS as "AUTHORITY:CODE", e.g. "EPSG:4978". Required; invalid rejects the payload.
  std::string crsId;
  std::array<double, 3> origin = {0.0, 0.0, 0.0};   // Meters, translation local origin -> CRS
  std::array<double, 4> rotation = {0.0, 0.0, 0.0, 1.0};  // Unit quaternion x, y, z, w, local -> CRS
  double scale = 1.0;  // Dimensionless uniform scale, local -> CRS; must be finite and > 0
  double epoch = std::numeric_limits<double>::quiet_NaN();  // Decimal year; NaN when absent
  std::string wkt;  // UTF-8 WKT2 string of the source compound CRS; empty when absent

  // Printable ASCII, exactly one ':', both parts non-empty, <= kMaxCrsIdBytes. No registry lookup.
  static bool isValidCrsId(const std::string& id);

  SpzExtensionGeoreferenceNiantic();
  uint32_t payloadBytes() const override;
  void write(std::ostream& os) const override;
  SpzExtensionBase* copyAsRawData() const override;
  std::optional<std::shared_ptr<SpzExtensionBase>> tryReadFromPly(
      std::istream& in, const std::unordered_set<std::string>& elementNames) const override;
  void writePlyHeader(std::ostream& out) const override;
  void writePlyData(std::ostream& out) const override;
  static std::optional<SpzExtensionBasePtr> read(std::istream& is);
  static SpzExtensionType type();
};

}  // namespace spz

#endif  // SPZ_GEOREFERENCE_NIANTIC_H_
