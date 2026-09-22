"""Tests for SPZ extension round-trip through NGSP v4 and legacy gzip formats."""

import gzip
import io
import math
import os
import struct
import tempfile

import numpy as np
import pytest

import spz


def _make_cloud(num_points=5, seed=0):
    rng = np.random.default_rng(seed)
    cloud = spz.GaussianCloud()
    cloud.sh_degree = 1
    cloud.positions = rng.uniform(-1.0, 1.0, size=num_points * 3).astype(np.float32)
    cloud.scales = rng.uniform(-2.0, 2.0, size=num_points * 3).astype(np.float32)
    cloud.rotations = rng.uniform(-1.0, 1.0, size=num_points * 4).astype(np.float32)
    cloud.alphas = rng.uniform(-1.0, 1.0, size=num_points).astype(np.float32)
    cloud.colors = rng.uniform(0.0, 1.0, size=num_points * 3).astype(np.float32)
    cloud.sh = rng.uniform(-0.5, 0.5, size=num_points * 9).astype(np.float32)
    return cloud


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_safe_orbit_extension_round_trip():
    """SafeOrbit extension survives a save/load round-trip"""
    cloud = _make_cloud()

    ext = spz.SpzExtensionSafeOrbitCameraAdobe()
    ext.safe_orbit_elevation_min = -0.5
    ext.safe_orbit_elevation_max = 1.2
    ext.safe_orbit_radius_min = 0.3
    cloud.extensions = [ext]

    filename = os.path.join(tempfile.gettempdir(), "ext_round_trip.spz")
    assert spz.save_spz(cloud, spz.PackOptions(), filename) is True

    # FlagHasExtensions (0x2) must be set in the header flags byte (offset 14).
    with open(filename, "rb") as f:
        header = f.read(32)
    flags = header[14]
    assert flags & 0x2, f"FlagHasExtensions not set in saved file (flags=0x{flags:02x})"

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert loaded.num_points == cloud.num_points
    assert len(loaded.extensions) == 1

    loaded_ext = loaded.extensions[0]
    assert isinstance(loaded_ext, spz.SpzExtensionSafeOrbitCameraAdobe)
    assert abs(loaded_ext.safe_orbit_elevation_min - ext.safe_orbit_elevation_min) < 1e-5
    assert abs(loaded_ext.safe_orbit_elevation_max - ext.safe_orbit_elevation_max) < 1e-5
    assert abs(loaded_ext.safe_orbit_radius_min - ext.safe_orbit_radius_min) < 1e-5


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_no_extension_flag_when_no_extensions():
    """FlagHasExtensions must NOT be set when the cloud has no extensions."""
    cloud = _make_cloud()
    cloud.extensions = []

    filename = os.path.join(tempfile.gettempdir(), "no_ext.spz")
    assert spz.save_spz(cloud, spz.PackOptions(), filename) is True

    with open(filename, "rb") as f:
        header = f.read(32)
    flags = header[14]
    assert not (flags & 0x2), f"FlagHasExtensions unexpectedly set (flags=0x{flags:02x})"


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_toc_byte_offset_advances_with_extensions():
    """tocByteOffset must be > 32 when extensions are correctly saved, 32 when absent."""
    cloud = _make_cloud()

    # Without extensions
    filename_no_ext = os.path.join(tempfile.gettempdir(), "toc_no_ext.spz")
    cloud.extensions = []
    assert spz.save_spz(cloud, spz.PackOptions(), filename_no_ext) is True
    with open(filename_no_ext, "rb") as f:
        hdr_no_ext = f.read(32)
    tbo_no_ext = struct.unpack_from("<I", hdr_no_ext, 16)[0]
    assert tbo_no_ext == 32, f"Expected tocByteOffset=32 with no extensions, got {tbo_no_ext}"

    # With extension
    ext = spz.SpzExtensionSafeOrbitCameraAdobe()
    cloud.extensions = [ext]
    filename_ext = os.path.join(tempfile.gettempdir(), "toc_with_ext.spz")
    assert spz.save_spz(cloud, spz.PackOptions(), filename_ext) is True
    with open(filename_ext, "rb") as f:
        hdr_ext = f.read(32)
    tbo_ext = struct.unpack_from("<I", hdr_ext, 16)[0]
    assert tbo_ext > 32, f"Expected tocByteOffset>32 with extension, got {tbo_ext}"


# ---------------------------------------------------------------------------
# SPZ_NIANTIC_georeference (0x4E530001)
# ---------------------------------------------------------------------------
#
# File layout when the georeference extension is the only extension:
# fixed 32-byte header, then the record at offset 32:
#   [u32 type @32][u32 byteLength @36][payload @40]
# Payload offsets within the file: ext_version @40, flags @41, crs_id_length @42,
# provenance_crs_length @44, origin @48, rotation @72, scale @104, epoch @112,
# crs_id @120, provenance_crs @120+crs_id_length.
_GEOREF_TYPE = 0x4E530001
_GEOREF_RECORD_OFFSET = 32
_GEOREF_PAYLOAD_OFFSET = 40
_GEOREF_CRS_ID_LENGTH_OFFSET = 42
_GEOREF_PROVENANCE_CRS_LENGTH_OFFSET = 44
_GEOREF_FIXED_PAYLOAD_BYTES = 80
_GEOREF_CRS_ID_OFFSET = _GEOREF_PAYLOAD_OFFSET + _GEOREF_FIXED_PAYLOAD_BYTES
# Guard against silent layout drift.
assert _GEOREF_CRS_ID_LENGTH_OFFSET + 2 == _GEOREF_PROVENANCE_CRS_LENGTH_OFFSET
assert _GEOREF_PAYLOAD_OFFSET + _GEOREF_FIXED_PAYLOAD_BYTES == _GEOREF_CRS_ID_OFFSET
_GEOREF_MAX_CRS_ID_BYTES = 256
_GEOREF_FLAGS_OFFSET = _GEOREF_PAYLOAD_OFFSET + 1
_PROJJSON_SAMPLE = '{"type":"GeographicCRS","name":"WGS 84"}'

_PROVENANCE_CRS_SAMPLE = (
    'COMPOUNDCRS["WGS 84 + EGM2008 height",'
    'GEOGCRS["WGS 84"],VERTCRS["EGM2008 height"]]'
)


def _make_georef_ext(
    provenance_crs="", epoch=float("nan"), scale=0.9736, crs_id="EPSG:4978", encoding=None
):
    ext = spz.SpzExtensionGeoreferenceNiantic()
    ext.crs_id = crs_id
    ext.origin = [4194304.5, -555555.25, 4713930.125]
    ext.rotation = [0.5, 0.5, 0.5, 0.5]
    ext.scale = scale
    ext.epoch = epoch
    ext.provenance_crs = provenance_crs
    if encoding is not None:
        ext.provenance_crs_encoding = encoding
    return ext


def _save_to_tmp(cloud, name):
    filename = os.path.join(tempfile.gettempdir(), name)
    assert spz.save_spz(cloud, spz.PackOptions(), filename) is True
    return filename


def _assert_gaussians_equal(a, b):
    assert a.num_points == b.num_points
    for attr in ("positions", "scales", "rotations", "alphas", "colors", "sh"):
        assert np.array_equal(np.asarray(getattr(a, attr)), np.asarray(getattr(b, attr))), (
            f"Gaussian attribute {attr} differs"
        )


def _patch_file(filename, offset, data):
    with open(filename, "r+b") as f:
        f.seek(offset)
        f.write(data)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
@pytest.mark.parametrize(
    "provenance_crs", ["", _PROVENANCE_CRS_SAMPLE], ids=["no_provenance_crs", "with_provenance_crs"]
)
@pytest.mark.parametrize("epoch", [float("nan"), 2025.5], ids=["no_epoch", "with_epoch"])
def test_georeference_round_trip(provenance_crs, epoch):
    """Georeference extension fields survive a save/load round-trip exactly."""
    cloud = _make_cloud()
    ext = _make_georef_ext(provenance_crs=provenance_crs, epoch=epoch)
    cloud.extensions = [ext]

    filename = _save_to_tmp(cloud, "georef_round_trip.spz")
    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert loaded.num_points == cloud.num_points
    assert len(loaded.extensions) == 1

    loaded_ext = loaded.extensions[0]
    assert isinstance(loaded_ext, spz.SpzExtensionGeoreferenceNiantic)
    assert loaded_ext.crs_id == ext.crs_id
    assert loaded_ext.origin == ext.origin
    assert loaded_ext.rotation == ext.rotation
    assert loaded_ext.scale == ext.scale
    assert loaded_ext.provenance_crs == ext.provenance_crs
    assert loaded_ext.provenance_crs_encoding == ext.provenance_crs_encoding
    if math.isnan(epoch):
        assert math.isnan(loaded_ext.epoch)
    else:
        assert loaded_ext.epoch == epoch


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_coexists_with_safe_orbit():
    """A file carrying both georeference and safe orbit round-trips both intact."""
    cloud = _make_cloud()
    orbit = spz.SpzExtensionSafeOrbitCameraAdobe()
    orbit.safe_orbit_elevation_min = -0.5
    orbit.safe_orbit_elevation_max = 1.2
    orbit.safe_orbit_radius_min = 0.3
    georef = _make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE, epoch=2025.5)
    cloud.extensions = [orbit, georef]

    filename = _save_to_tmp(cloud, "georef_coexist.spz")
    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 2

    loaded_orbit = next(
        e for e in loaded.extensions if isinstance(e, spz.SpzExtensionSafeOrbitCameraAdobe)
    )
    loaded_georef = next(
        e for e in loaded.extensions if isinstance(e, spz.SpzExtensionGeoreferenceNiantic)
    )
    assert abs(loaded_orbit.safe_orbit_elevation_min - orbit.safe_orbit_elevation_min) < 1e-5
    assert abs(loaded_orbit.safe_orbit_elevation_max - orbit.safe_orbit_elevation_max) < 1e-5
    assert abs(loaded_orbit.safe_orbit_radius_min - orbit.safe_orbit_radius_min) < 1e-5
    assert loaded_georef.crs_id == georef.crs_id
    assert loaded_georef.origin == georef.origin
    assert loaded_georef.rotation == georef.rotation
    assert loaded_georef.scale == georef.scale
    assert loaded_georef.epoch == georef.epoch
    assert loaded_georef.provenance_crs == georef.provenance_crs


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_skip_path_preserves_core_data():
    """A reader that does not implement the extension still loads identical core data.

    Simulated by rewriting the record's type ID to an unregistered vendor, which
    exercises the same unknown-type skip path a non-implementing reader would take.
    """
    cloud = _make_cloud()
    baseline_file = _save_to_tmp(cloud, "georef_skip_baseline.spz")
    baseline = spz.load_spz(baseline_file, spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE)]
    filename = _save_to_tmp(cloud, "georef_skip.spz")
    _patch_file(filename, _GEOREF_RECORD_OFFSET, struct.pack("<I", 0x7A7A0001))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_truncated_fixed_section_skipped():
    """A record shorter than the fixed section is skipped and core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_trunc_baseline.spz"), spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE)]
    filename = _save_to_tmp(cloud, "georef_trunc.spz")
    with open(filename, "rb") as f:
        toc_offset = struct.unpack_from("<I", f.read(32), 16)[0]

    # Rewrite the extension zone in place: a georeference record truncated to a
    # 10-byte payload, followed by an unknown-type record padding the remaining
    # zone bytes so the overall file layout is untouched.
    truncated_len = 10
    filler_offset = _GEOREF_PAYLOAD_OFFSET + truncated_len
    filler_payload_len = toc_offset - filler_offset - 8
    assert filler_payload_len >= 0, "provenance_crs sample too short to carve a filler record"
    _patch_file(filename, _GEOREF_RECORD_OFFSET + 4, struct.pack("<I", truncated_len))
    _patch_file(filename, filler_offset, struct.pack("<II", 0x7A7A0002, filler_payload_len))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_provenance_crs_length_overrun_skipped():
    """A provenance_crs_length exceeding the payload is rejected and core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_provenancecrs_overrun_baseline.spz"), spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE)]
    filename = _save_to_tmp(cloud, "georef_provenancecrs_overrun.spz")
    _patch_file(filename, _GEOREF_PROVENANCE_CRS_LENGTH_OFFSET, struct.pack("<I", 0xFFFF0000))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_crs_id_length_overrun_skipped():
    """A crs_id_length exceeding the payload is rejected and core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(
        _save_to_tmp(cloud, "georef_crsid_overrun_baseline.spz"), spz.UnpackOptions()
    )

    cloud.extensions = [_make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE)]
    filename = _save_to_tmp(cloud, "georef_crsid_overrun.spz")
    _patch_file(filename, _GEOREF_CRS_ID_LENGTH_OFFSET, struct.pack("<H", 0xFFFF))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_crs_id_at_size_cap_round_trips():
    """A crs_id of exactly the 256-byte cap is accepted; one byte more is not."""
    cloud = _make_cloud()
    at_cap = "EPSG:" + "9" * (_GEOREF_MAX_CRS_ID_BYTES - len("EPSG:"))
    assert len(at_cap) == _GEOREF_MAX_CRS_ID_BYTES

    cloud.extensions = [_make_georef_ext(crs_id=at_cap)]
    loaded = spz.load_spz(_save_to_tmp(cloud, "georef_crsid_cap.spz"), spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    assert loaded.extensions[0].crs_id == at_cap

    cloud.extensions = [_make_georef_ext(crs_id=at_cap + "9")]
    loaded = spz.load_spz(_save_to_tmp(cloud, "georef_crsid_over_cap.spz"), spz.UnpackOptions())
    assert len(loaded.extensions) == 0


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_unsupported_version_skipped():
    """A payload with ext_version=2 is skipped and core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_v2_baseline.spz"), spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext()]
    filename = _save_to_tmp(cloud, "georef_v2.spz")
    _patch_file(filename, _GEOREF_PAYLOAD_OFFSET, struct.pack("<B", 2))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
@pytest.mark.parametrize(
    "crs_id",
    ["", "4978", "EPSG:", ":4978", "EPSG:4978:1", "EPSG 4978", "EPSG:497 8"],
    ids=["empty", "no_authority", "no_code", "empty_authority", "two_colons", "space", "inner_space"],
)
def test_georeference_malformed_crs_id_skipped(crs_id):
    """A crs_id that is not a well-formed AUTHORITY:CODE is rejected; core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_crsid_baseline.spz"), spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext(crs_id=crs_id)]
    filename = _save_to_tmp(cloud, "georef_crsid.spz")

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
@pytest.mark.parametrize(
    "crs_id", ["EPSG:4978", "IAU_2015:30100", "IGNF:LAMB93"], ids=["earth", "moon", "non_numeric"]
)
def test_georeference_crs_id_authorities_round_trip(crs_id):
    """Any authority round-trips verbatim — the format is not tied to the EPSG registry."""
    cloud = _make_cloud()
    cloud.extensions = [_make_georef_ext(crs_id=crs_id)]

    filename = _save_to_tmp(cloud, "georef_crsid_authority.spz")
    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    assert loaded.extensions[0].crs_id == crs_id


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_oversized_crs_id_omitted_on_write():
    """A crs_id beyond the 256-byte write cap is omitted, so readers reject the record."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_bigcrsid_baseline.spz"), spz.UnpackOptions())

    cloud.extensions = [_make_georef_ext(crs_id="EPSG:" + "9" * 256)]
    filename = _save_to_tmp(cloud, "georef_bigcrsid.spz")

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
@pytest.mark.parametrize(
    "scale", [0.0, -1.0, float("nan"), float("inf")], ids=["zero", "negative", "nan", "inf"]
)
def test_georeference_invalid_scale_skipped(scale):
    """A non-finite or non-positive scale is rejected on load; core data still loads."""
    cloud = _make_cloud()
    baseline = spz.load_spz(_save_to_tmp(cloud, "georef_badscale_baseline.spz"), spz.UnpackOptions())

    ext = _make_georef_ext(scale=scale)
    cloud.extensions = [ext]
    filename = _save_to_tmp(cloud, "georef_badscale.spz")

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 0
    _assert_gaussians_equal(loaded, baseline)


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_oversized_provenance_crs_omitted_on_write():
    """A provenance_crs beyond the 1 MiB write cap is omitted (not truncated); the rest round-trips."""
    cloud = _make_cloud()
    ext = _make_georef_ext(epoch=2025.5)
    ext.provenance_crs = "A" * ((1 << 20) + 1)
    cloud.extensions = [ext]

    filename = _save_to_tmp(cloud, "georef_oversized_provenance_crs.spz")
    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    e = loaded.extensions[0]
    assert e.provenance_crs == ""
    assert e.crs_id == ext.crs_id
    assert e.origin == ext.origin
    assert e.rotation == ext.rotation
    assert e.scale == ext.scale
    assert e.epoch == ext.epoch


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
@pytest.mark.parametrize(
    "encoding,payload",
    [
        (spz.ProvenanceCrsEncoding.WKT2, _PROVENANCE_CRS_SAMPLE),
        (spz.ProvenanceCrsEncoding.PROJJSON, _PROJJSON_SAMPLE),
    ],
    ids=["wkt2", "projjson"],
)
def test_georeference_provenance_crs_encoding_round_trips(encoding, payload):
    """provenance_crs is never of unknown syntax: its encoding round-trips with it."""
    cloud = _make_cloud()
    cloud.extensions = [_make_georef_ext(provenance_crs=payload, encoding=encoding)]

    loaded = spz.load_spz(_save_to_tmp(cloud, "georef_prov_encoding.spz"), spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    assert loaded.extensions[0].provenance_crs == payload
    assert loaded.extensions[0].provenance_crs_encoding == encoding


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_unknown_provenance_encoding_drops_string_keeps_transform():
    """An unassigned encoding drops provenance_crs but must not invalidate the transform."""
    cloud = _make_cloud()
    ext = _make_georef_ext(provenance_crs=_PROVENANCE_CRS_SAMPLE)
    cloud.extensions = [ext]
    filename = _save_to_tmp(cloud, "georef_prov_unknown_encoding.spz")

    # Set encoding bits 2-3 to 3 (unassigned), keeping has_provenance_crs set.
    with open(filename, "rb") as f:
        f.seek(_GEOREF_FLAGS_OFFSET)
        flags = f.read(1)[0]
    _patch_file(filename, _GEOREF_FLAGS_OFFSET, struct.pack("<B", flags | 0x0C))

    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    e = loaded.extensions[0]
    assert e.provenance_crs == ""
    assert e.crs_id == ext.crs_id
    assert e.origin == ext.origin
    assert e.scale == ext.scale


@pytest.mark.skipif(not spz.has_extension_support(), reason="built without extension support")
def test_georeference_earth_radius_precision():
    """Origins at Earth-radius magnitude keep exact double precision."""
    cloud = _make_cloud()
    ext = _make_georef_ext()
    ext.origin = [6378137.123456789, -6356752.987654321, 1234567.891011121]
    cloud.extensions = [ext]

    filename = _save_to_tmp(cloud, "georef_precision.spz")
    loaded = spz.load_spz(filename, spz.UnpackOptions())
    assert len(loaded.extensions) == 1
    assert loaded.extensions[0].origin == ext.origin
