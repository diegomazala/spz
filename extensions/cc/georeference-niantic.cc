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

#include "georeference-niantic.h"
#include "load-spz.h"

#include <cmath>

namespace spz {
namespace {
template <class T>
bool readExact(std::istream& is, T& out) {
  return static_cast<bool>(is.read(reinterpret_cast<char*>(&out), sizeof(T)));
}

// Returns the number of bytes remaining between the current get position and the end of `is`,
// or -1 if the stream does not support positioning. Restores the original position on success.
std::streamoff remainingBytes(std::istream& is) {
  const std::streampos cur = is.tellg();
  if (cur == std::streampos(-1)) return -1;
  is.seekg(0, std::ios::end);
  const std::streampos end = is.tellg();
  is.seekg(cur);
  if (end == std::streampos(-1)) return -1;
  return end - cur;
}

constexpr double kUnitQuaternionTolerance = 1e-6;
}  // namespace

SpzExtensionGeoreferenceNiantic::SpzExtensionGeoreferenceNiantic()
    : SpzExtensionBase(SpzExtensionType::SPZ_NIANTIC_georeference) {}

uint32_t SpzExtensionGeoreferenceNiantic::payloadBytes() const {
  // Fixed section: ext_version(1) + flags(1) + reserved(2) + crs_epsg(4) + origin(24) +
  // rotation(32) + scale(8) + epoch(8) + wkt_length(4) = 84 bytes, followed by the WKT string.
  const uint64_t wktBytes = wkt.size() <= kMaxWktBytes ? wkt.size() : 0;
  return static_cast<uint32_t>(84 + wktBytes);
}

void SpzExtensionGeoreferenceNiantic::write(std::ostream& os) const {
  const uint32_t t = static_cast<uint32_t>(extensionType);
  const uint32_t len = payloadBytes();
  const uint8_t extVersion = kExtVersion;
  uint32_t wktLength = static_cast<uint32_t>(wkt.size());
  if (wkt.size() > kMaxWktBytes) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: wkt size %llu exceeds %u bytes — wkt omitted on write",
           static_cast<unsigned long long>(wkt.size()), static_cast<unsigned>(kMaxWktBytes));
    wktLength = 0;
  }
  uint8_t flags = 0;
  if (!std::isnan(epoch)) flags |= kFlagHasEpoch;
  if (wktLength > 0) flags |= kFlagHasWkt;
  const uint16_t reserved = 0;
  SpzLog("[SPZ] Writing extension: GeoreferenceNiantic (epsg=%u)", static_cast<unsigned>(crsEpsg));
  os.write(reinterpret_cast<const char*>(&t), sizeof(t));
  os.write(reinterpret_cast<const char*>(&len), sizeof(len));
  os.write(reinterpret_cast<const char*>(&extVersion), sizeof(extVersion));
  os.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
  os.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
  os.write(reinterpret_cast<const char*>(&crsEpsg), sizeof(crsEpsg));
  os.write(reinterpret_cast<const char*>(origin.data()), sizeof(origin));
  os.write(reinterpret_cast<const char*>(rotation.data()), sizeof(rotation));
  os.write(reinterpret_cast<const char*>(&scale), sizeof(scale));
  os.write(reinterpret_cast<const char*>(&epoch), sizeof(epoch));
  os.write(reinterpret_cast<const char*>(&wktLength), sizeof(wktLength));
  if (wktLength > 0)
    os.write(wkt.data(), static_cast<std::streamsize>(wktLength));
}

std::optional<SpzExtensionBasePtr> SpzExtensionGeoreferenceNiantic::read(std::istream& is) {
  SpzLog("[SPZ] Found extension: GeoreferenceNiantic");
  uint8_t extVersion{};
  uint8_t flags{};
  uint16_t reserved{};  // Ignored on read; writers must zero it.
  auto rec = std::make_shared<SpzExtensionGeoreferenceNiantic>();
  uint32_t wktLength{};
  if (!readExact(is, extVersion) || !readExact(is, flags) || !readExact(is, reserved) ||
      !readExact(is, rec->crsEpsg) || !readExact(is, rec->origin) ||
      !readExact(is, rec->rotation) || !readExact(is, rec->scale) || !readExact(is, rec->epoch) ||
      !readExact(is, wktLength)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: truncated payload — extension skipped");
    return std::nullopt;
  }
  if (extVersion != kExtVersion) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: unsupported ext_version %u (expected %u) — extension skipped",
           static_cast<unsigned>(extVersion), static_cast<unsigned>(kExtVersion));
    return std::nullopt;
  }
  if (rec->crsEpsg == 0) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: crs_epsg 0 is reserved and invalid in ext_version 1 — extension skipped");
    return std::nullopt;
  }
  // Unknown flag bits are ignored; only the bits this version defines are interpreted.
  const std::streamoff remaining = remainingBytes(is);
  if (remaining < 0 || static_cast<uint64_t>(wktLength) > static_cast<uint64_t>(remaining)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: wkt_length %u exceeds remaining payload bytes — extension skipped",
           static_cast<unsigned>(wktLength));
    return std::nullopt;
  }
  if (wktLength > 0) {
    rec->wkt.resize(wktLength);
    if (!is.read(rec->wkt.data(), static_cast<std::streamsize>(wktLength))) {
      SpzLog("[SPZ WARNING] GeoreferenceNiantic: failed to read WKT string — extension skipped");
      return std::nullopt;
    }
  }
  if (!(flags & kFlagHasEpoch))
    rec->epoch = std::numeric_limits<double>::quiet_NaN();

  const auto& q = rec->rotation;
  const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!std::isfinite(norm) || norm == 0.0) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: rotation quaternion is not normalizable — extension skipped");
    return std::nullopt;
  }
  if (std::abs(norm - 1.0) > kUnitQuaternionTolerance) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: rotation quaternion norm %f is not unit — normalized on load", norm);
    for (auto& c : rec->rotation) c /= norm;
  }
  if (!std::isfinite(rec->scale) || rec->scale <= 0.0) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: scale %f is not finite and positive — extension skipped",
           rec->scale);
    return std::nullopt;
  }
  return std::optional{std::move(rec)};
}

SpzExtensionType SpzExtensionGeoreferenceNiantic::type() {
  return SpzExtensionType::SPZ_NIANTIC_georeference;
}

SpzExtensionBase* SpzExtensionGeoreferenceNiantic::copyAsRawData() const {
  return new SpzExtensionGeoreferenceNiantic(*this);
}

// No PLY representation for this extension.
std::optional<SpzExtensionBasePtr> SpzExtensionGeoreferenceNiantic::tryReadFromPly(
    std::istream&, const std::unordered_set<std::string>&) const {
  return std::nullopt;
}

void SpzExtensionGeoreferenceNiantic::writePlyHeader(std::ostream&) const {}

void SpzExtensionGeoreferenceNiantic::writePlyData(std::ostream&) const {}

}  // namespace spz
