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

// Fixed section: ext_version(1) + flags(1) + crs_id_length(2) + provenance_wkt_length(4) +
// origin(24) + rotation(32) + scale(8) + epoch(8), followed by crs_id then provenance_wkt.
constexpr uint32_t kFixedPayloadBytes = 80;
}  // namespace

bool SpzExtensionGeoreferenceNiantic::isValidCrsId(const std::string& id) {
  if (id.empty() || id.size() > kMaxCrsIdBytes) return false;
  size_t colons = 0;
  size_t colon = 0;
  for (size_t i = 0; i < id.size(); ++i) {
    // Printable ASCII only: rules out spaces, control bytes and multi-byte sequences.
    const char c = id[i];
    if (c < 0x21 || c > 0x7E) return false;
    if (c == ':') {
      ++colons;
      colon = i;
    }
  }
  return colons == 1 && colon != 0 && colon + 1 != id.size();
}

SpzExtensionGeoreferenceNiantic::SpzExtensionGeoreferenceNiantic()
    : SpzExtensionBase(SpzExtensionType::SPZ_NIANTIC_georeference) {}

uint32_t SpzExtensionGeoreferenceNiantic::payloadBytes() const {
  const uint64_t crsIdBytes = crsId.size() <= kMaxCrsIdBytes ? crsId.size() : 0;
  const uint64_t provenanceWktBytes = provenanceWkt.size() <= kMaxProvenanceWktBytes ? provenanceWkt.size() : 0;
  return static_cast<uint32_t>(kFixedPayloadBytes + crsIdBytes + provenanceWktBytes);
}

void SpzExtensionGeoreferenceNiantic::write(std::ostream& os) const {
  const uint32_t t = static_cast<uint32_t>(extensionType);
  const uint32_t len = payloadBytes();
  const uint8_t extVersion = kExtVersion;
  // Unlike the optional provenance_wkt, an invalid crs_id invalidates the whole record: a transform with no
  // target is meaningless. Oversized values are omitted (16-bit length), the rest written as-is.
  uint16_t crsIdLength = 0;
  if (crsId.size() <= kMaxCrsIdBytes) {
    crsIdLength = static_cast<uint16_t>(crsId.size());
  }
  if (!isValidCrsId(crsId)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: crs_id is not a well-formed AUTHORITY:CODE "
           "identifier of at most %u bytes (got %llu bytes) — readers will reject this record",
           static_cast<unsigned>(kMaxCrsIdBytes),
           static_cast<unsigned long long>(crsId.size()));
  }
  uint32_t provenanceWktLength = static_cast<uint32_t>(provenanceWkt.size());
  if (provenanceWkt.size() > kMaxProvenanceWktBytes) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: provenance_wkt size %llu exceeds %u bytes — omitted on write",
           static_cast<unsigned long long>(provenanceWkt.size()), static_cast<unsigned>(kMaxProvenanceWktBytes));
    provenanceWktLength = 0;
  }
  uint8_t flags = 0;
  if (!std::isnan(epoch)) flags |= kFlagHasEpoch;
  if (provenanceWktLength > 0) flags |= kFlagHasProvenanceWkt;
  SpzLog("[SPZ] Writing extension: GeoreferenceNiantic (crs=%s)",
         isValidCrsId(crsId) ? crsId.c_str() : "<invalid>");
  os.write(reinterpret_cast<const char*>(&t), sizeof(t));
  os.write(reinterpret_cast<const char*>(&len), sizeof(len));
  os.write(reinterpret_cast<const char*>(&extVersion), sizeof(extVersion));
  os.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
  os.write(reinterpret_cast<const char*>(&crsIdLength), sizeof(crsIdLength));
  os.write(reinterpret_cast<const char*>(&provenanceWktLength), sizeof(provenanceWktLength));
  os.write(reinterpret_cast<const char*>(origin.data()), sizeof(origin));
  os.write(reinterpret_cast<const char*>(rotation.data()), sizeof(rotation));
  os.write(reinterpret_cast<const char*>(&scale), sizeof(scale));
  os.write(reinterpret_cast<const char*>(&epoch), sizeof(epoch));
  if (crsIdLength > 0)
    os.write(crsId.data(), static_cast<std::streamsize>(crsIdLength));
  if (provenanceWktLength > 0)
    os.write(provenanceWkt.data(), static_cast<std::streamsize>(provenanceWktLength));
}

std::optional<SpzExtensionBasePtr> SpzExtensionGeoreferenceNiantic::read(std::istream& is) {
  SpzLog("[SPZ] Found extension: GeoreferenceNiantic");
  uint8_t extVersion{};
  uint8_t flags{};
  auto rec = std::make_shared<SpzExtensionGeoreferenceNiantic>();
  uint16_t crsIdLength{};
  uint32_t provenanceWktLength{};
  if (!readExact(is, extVersion) || !readExact(is, flags) || !readExact(is, crsIdLength) ||
      !readExact(is, provenanceWktLength) || !readExact(is, rec->origin) ||
      !readExact(is, rec->rotation) || !readExact(is, rec->scale) || !readExact(is, rec->epoch)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: truncated payload — extension skipped");
    return std::nullopt;
  }
  if (extVersion != kExtVersion) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: unsupported ext_version %u (expected %u) — extension skipped",
           static_cast<unsigned>(extVersion), static_cast<unsigned>(kExtVersion));
    return std::nullopt;
  }
  // Unknown flag bits are ignored; only the bits this version defines are interpreted.
  const std::streamoff remaining = remainingBytes(is);
  const uint64_t declared = static_cast<uint64_t>(crsIdLength) + provenanceWktLength;
  if (remaining < 0 || declared > static_cast<uint64_t>(remaining)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: crs_id_length %u + provenance_wkt_length %u exceed the "
           "remaining payload bytes — extension skipped",
           static_cast<unsigned>(crsIdLength), static_cast<unsigned>(provenanceWktLength));
    return std::nullopt;
  }
  if (crsIdLength > 0) {
    rec->crsId.resize(crsIdLength);
    if (!is.read(rec->crsId.data(), static_cast<std::streamsize>(crsIdLength))) {
      SpzLog("[SPZ WARNING] GeoreferenceNiantic: failed to read crs_id — extension skipped");
      return std::nullopt;
    }
  }
  if (!isValidCrsId(rec->crsId)) {
    SpzLog("[SPZ WARNING] GeoreferenceNiantic: crs_id is empty or not a well-formed "
           "AUTHORITY:CODE identifier — extension skipped");
    return std::nullopt;
  }
  if (provenanceWktLength > 0) {
    rec->provenanceWkt.resize(provenanceWktLength);
    if (!is.read(rec->provenanceWkt.data(), static_cast<std::streamsize>(provenanceWktLength))) {
      SpzLog("[SPZ WARNING] GeoreferenceNiantic: failed to read provenance_wkt — extension skipped");
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
