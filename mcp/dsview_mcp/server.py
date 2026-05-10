"""DSView MCP server.

Exposes DSLogic logic-analyzer capture and analysis as MCP tools backed by
the `dsview_helper` C binary built against libsigrok4DSL.

Tools:
    list_devices()                       -> attached devices
    device_info(index)                   -> active-device info
    capture(...)                         -> run a capture, returns capture id
    list_captures()                      -> recent captures in workdir
    analyze(capture_id, channel)         -> edge / pulse / frequency stats
    read_window(capture_id, channel,
                start_sample, length)    -> bit-level samples (capped)

The helper writes:
    <workdir>/cap-<id>.bin        raw packed bytes (unitsize bytes per slice)
    <workdir>/cap-<id>.json       metadata
"""

from __future__ import annotations

import json
import os
import subprocess
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Configuration via env vars (fall back to repo-relative defaults).
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[2]

HELPER = Path(os.environ.get(
    "DSVIEW_HELPER",
    REPO_ROOT / "mcp" / "build" / "dsview_helper",
))
FIRMWARE_DIR = Path(os.environ.get(
    "DSVIEW_FIRMWARE_DIR",
    REPO_ROOT / "DSView" / "res",
))
DECODERS_DIR = Path(os.environ.get(
    "DSVIEW_DECODERS_DIR",
    REPO_ROOT / "libsigrokdecode4DSL" / "decoders",
))
USER_DATA_DIR = Path(os.environ.get(
    "DSVIEW_USER_DATA_DIR",
    Path.home() / ".dsview",
))
WORKDIR = Path(os.environ.get(
    "DSVIEW_WORKDIR",
    Path.home() / ".dsview" / "captures",
))

WORKDIR.mkdir(parents=True, exist_ok=True)
USER_DATA_DIR.mkdir(parents=True, exist_ok=True)

# Make the bundled demo patterns available to the virtual-demo driver.
# Without this the driver falls back to random sample data, which makes
# Demo Device captures useless for testing protocol decoders.
_DEMO_LINK = USER_DATA_DIR / "demo"
_DEMO_SOURCE = REPO_ROOT / "DSView" / "demo"
if not _DEMO_LINK.exists() and _DEMO_SOURCE.is_dir():
    try:
        _DEMO_LINK.symlink_to(_DEMO_SOURCE)
    except OSError:
        pass

mcp = FastMCP("dsview")

# Hard caps to keep MCP responses small.
MAX_WINDOW_SAMPLES = 4096


# ---------------------------------------------------------------------------
# Helper invocation
# ---------------------------------------------------------------------------

class HelperError(RuntimeError):
    pass


def _run_helper(args: list[str], timeout: int = 60) -> dict[str, Any]:
    """Run dsview_helper with the given args and parse its JSON stdout."""
    if not HELPER.exists():
        raise HelperError(
            f"dsview_helper not found at {HELPER}. "
            "Build it with: cmake -S mcp -B mcp/build && cmake --build mcp/build"
        )

    cmd = [str(HELPER)] + args + [
        "--firmware", str(FIRMWARE_DIR),
        "--user-data", str(USER_DATA_DIR),
        "--decoders-dir", str(DECODERS_DIR),
    ]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired as e:
        raise HelperError(f"helper timed out: {' '.join(cmd)}") from e

    out = proc.stdout.strip().splitlines()
    if not out:
        raise HelperError(
            f"helper produced no output (rc={proc.returncode}); "
            f"stderr: {proc.stderr.strip()[-500:]}"
        )
    try:
        result = json.loads(out[-1])
    except json.JSONDecodeError as e:
        raise HelperError(
            f"helper returned non-JSON: {out[-1][:200]!r}; stderr: "
            f"{proc.stderr.strip()[-500:]}"
        ) from e

    if not result.get("ok", False):
        raise HelperError(result.get("error", "unknown helper error"))
    return result


# ---------------------------------------------------------------------------
# Capture loading & bit unpacking
# ---------------------------------------------------------------------------

@dataclass
class Capture:
    cap_id: str
    meta: dict[str, Any]
    bin_path: Path

    @property
    def samplerate(self) -> int:
        return int(self.meta["samplerate"])

    @property
    def unitsize(self) -> int:
        return int(self.meta["unitsize"]) or 1

    @property
    def samples(self) -> int:
        return int(self.meta.get("samples_written", 0))

    @property
    def channel_indices(self) -> list[int]:
        return [c["index"] for c in self.meta["channels"] if c["enabled"]]

    @property
    def layout(self) -> str:
        return str(self.meta.get("layout", "cross_byte"))

    @property
    def enabled_channel_indices(self) -> list[int]:
        return [c["index"] for c in self.meta["channels"] if c["enabled"]]

    def _load_raw_bytes(self) -> np.ndarray:
        return np.fromfile(self.bin_path, dtype=np.uint8)

    def channel_bits(self, ch_index: int) -> np.ndarray:
        """Return a uint8 array of 0/1 for the given channel."""
        if self.layout == "dsl_atomic":
            return self._channel_bits_atomic(ch_index)
        return self._channel_bits_cross(ch_index)

    def _channel_bits_atomic(self, ch_index: int) -> np.ndarray:
        """DSLogic atomic-block layout: per (atomic_samples, en_ch_num)
        block of `atomic_bytes_per_ch * en_ch` bytes, channels stored as
        consecutive 8-byte LSB-first packed runs of 64 samples each."""
        en = self.enabled_channel_indices
        if ch_index not in en:
            raise ValueError(f"channel {ch_index} not enabled in this capture")
        ch_slot = en.index(ch_index)
        en_count = len(en)
        atom_samples = int(self.meta.get("atomic_samples", 64))
        atom_bytes = int(self.meta.get("atomic_bytes_per_channel", 8))
        block_bytes = atom_bytes * en_count

        raw = self._load_raw_bytes()
        n_blocks = raw.size // block_bytes
        if n_blocks == 0:
            return np.zeros(0, dtype=np.uint8)
        blocks = raw[: n_blocks * block_bytes].reshape(
            n_blocks, en_count, atom_bytes
        )
        bits = np.unpackbits(blocks[:, ch_slot, :], axis=1, bitorder="little")
        flat = bits.reshape(-1)
        return flat[: self.samples] if self.samples else flat

    def _channel_bits_cross(self, ch_index: int) -> np.ndarray:
        unit = self.unitsize or 1
        raw = self._load_raw_bytes()
        n = (raw.size // unit) * unit
        slices = raw[:n].reshape(-1, unit)
        byte = ch_index // 8
        bit = ch_index % 8
        if byte >= unit:
            raise ValueError(
                f"channel {ch_index} out of range (unitsize={unit})"
            )
        return ((slices[:, byte] >> bit) & 1).astype(np.uint8)


def _load_capture(cap_id: str) -> Capture:
    json_path = WORKDIR / f"cap-{cap_id}.json"
    bin_path = WORKDIR / f"cap-{cap_id}.bin"
    if not json_path.exists() or not bin_path.exists():
        raise HelperError(f"capture {cap_id!r} not found in {WORKDIR}")
    meta = json.loads(json_path.read_text())
    return Capture(cap_id=cap_id, meta=meta, bin_path=bin_path)


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def list_devices() -> dict[str, Any]:
    """List currently attached DSLogic / Demo devices.

    Returns:
        {"devices": [{"index": int, "name": str, "handle": int}, ...]}
    """
    res = _run_helper(["list-devices"], timeout=15)
    return {"devices": res.get("devices", [])}


@mcp.tool()
def device_info(index: int = -1, include_disabled: bool = False) -> dict[str, Any]:
    """Get info about a device (channels, current samplerate, depth).

    Args:
        index: device index from list_devices (-1 = last attached).
        include_disabled: include disabled channels in the listing
            (default False — most devices expose 16 channels but only a
            handful are enabled, dropping the rest saves ~70% of tokens).
    """
    res = _run_helper(["device-info", "--index", str(index)], timeout=15)
    res.pop("ok", None)
    if not include_disabled:
        res["channels"] = [c for c in res.get("channels", [])
                           if c.get("enabled")]
    # type=10000 is SR_CHANNEL_LOGIC for every channel here; drop the noise.
    for c in res.get("channels", []):
        c.pop("type", None)
    return res


_OPERATION_MODES = {"buffer": 0, "stream": 1, "internal_test": 2,
                    "external_test": 3, "loopback_test": 4}
_BUFFER_OPTIONS  = {"stop": 0, "upload": 1}
_FILTERS         = {"none": 0, "1t": 1}


def _resolve_enum(name: str, value, table: dict[str, int]) -> int | None:
    """Accept either an int or a known string label; return int or None."""
    if value is None:
        return None
    if isinstance(value, int):
        return value
    key = str(value).strip().lower()
    if key in table:
        return table[key]
    raise ValueError(f"{name}: unknown value {value!r}; "
                     f"expected int or one of {list(table)}")


@mcp.tool()
def capture(
    samplerate: int = 1_000_000,
    depth: int = 100_000,
    channels: list[int] | None = None,
    index: int = -1,
    timeout_sec: int = 30,
    vth: float | None = 0.9,
    operation_mode: str | int | None = None,
    buffer_option: str | int | None = None,
    filter: str | int | None = None,
    channel_mode: int | None = None,
    rle: bool | None = None,
    ext_clock: bool | None = None,
    falling_edge_clock: bool | None = None,
    max_height: str | None = None,
    confirm: bool = False,
) -> dict[str, Any]:
    """Run a single logic capture on the selected device. **Two-step**.

    Calling with `confirm=False` (default) **does NOT touch the hardware** —
    it returns a `preview` describing every setting that would be applied.
    The caller (or the human) must inspect the preview, then call again with
    `confirm=True` (and the same other args) to actually trigger acquisition.

    Args:
        samplerate: samples per second (e.g. 1_000_000 = 1 MS/s).
        depth: total samples to capture.
        channels: list of channel indices to enable (None = keep current).
        index: device index from list_devices (-1 = last attached).
        timeout_sec: hard timeout for the capture.

      GUI-equivalent device options (None = leave at driver default):
        vth: voltage threshold in volts. **Defaults to 0.9 (suitable for
             1.8V logic systems)**. For 3.3V/5V systems pass a higher value
             (e.g. 1.4) or pass None to use the driver default ~1.0V.
        operation_mode: "buffer" | "stream" | "internal_test" |
             "external_test" | "loopback_test", or the raw int.
             Stream mode samples until depth or timeout; Buffer mode
             collects to on-board RAM then uploads.
        buffer_option: "stop" or "upload" — what to do when the user aborts
             a Buffer-mode capture.
        filter: "none" or "1t" (1-sample-clock glitch filter).
        channel_mode: 0..3 selecting (16ch@20MHz, 12ch@25MHz, 6ch@50MHz,
             3ch@100MHz) for DSLogic Plus. Match the layout in the GUI's
             "通道" panel.
        rle: enable hardware RLE compression (Buffer mode only).
        ext_clock: True = sample on an externally-supplied clock pin.
        falling_edge_clock: True = sample on falling clock edge instead of
             rising.
        max_height: "1X", "2X", "5X" — DSO display option, rarely needed
             for logic captures.
        confirm: must be True to actually run capture; False (default)
             returns a settings preview only.

    Returns (confirm=False):
        {"preview": {…}, "needs_confirm": True, "hint": "..."}

    Returns (confirm=True):
        {"capture_id": str, "samples": int, "samplerate": int,
         "duration_s": float, "channels": [..], "settings": {…}}
    """
    op_mode_i  = _resolve_enum("operation_mode", operation_mode,
                               _OPERATION_MODES)
    buf_opt_i  = _resolve_enum("buffer_option", buffer_option,
                               _BUFFER_OPTIONS)
    filter_i   = _resolve_enum("filter", filter, _FILTERS)

    settings = {
        "samplerate": samplerate,
        "depth": depth,
        "channels": channels,
        "index": index,
        "timeout_sec": timeout_sec,
        "vth": vth,
        "operation_mode": operation_mode,
        "buffer_option": buffer_option,
        "filter": filter,
        "channel_mode": channel_mode,
        "rle": rle,
        "ext_clock": ext_clock,
        "falling_edge_clock": falling_edge_clock,
        "max_height": max_height,
    }

    if not confirm:
        return {
            "needs_confirm": True,
            "preview": settings,
            "hint": ("This is a dry-run preview — no hardware was touched. "
                     "Review the settings above, then call capture(...) "
                     "again with confirm=True to run the actual acquisition."),
        }

    cap_id = uuid.uuid4().hex[:8]
    prefix = WORKDIR / f"cap-{cap_id}"

    args = [
        "capture",
        "--index", str(index),
        "--output", str(prefix),
        "--samplerate", str(samplerate),
        "--depth", str(depth),
        "--timeout", str(timeout_sec),
    ]
    if channels:
        args += ["--channels", ",".join(str(c) for c in channels)]
    if vth is not None:
        args += ["--vth", str(vth)]
    if op_mode_i is not None:
        args += ["--operation-mode", str(op_mode_i)]
    if buf_opt_i is not None:
        args += ["--buffer-option", str(buf_opt_i)]
    if filter_i is not None:
        args += ["--filter", str(filter_i)]
    if channel_mode is not None:
        args += ["--channel-mode", str(int(channel_mode))]
    if rle is not None:
        args += ["--rle", "1" if rle else "0"]
    if ext_clock is not None:
        args += ["--ext-clock", "1" if ext_clock else "0"]
    if falling_edge_clock is not None:
        args += ["--falling-edge", "1" if falling_edge_clock else "0"]
    if max_height:
        args += ["--max-height", max_height]

    t0 = time.time()
    _run_helper(args, timeout=timeout_sec + 30)
    elapsed = time.time() - t0

    cap = _load_capture(cap_id)
    return {
        "capture_id": cap_id,
        "samples": cap.samples,
        "samplerate": cap.samplerate,
        "duration_s": round(elapsed, 3),
        "channels": cap.channel_indices,
    }


@mcp.tool()
def list_captures(limit: int = 20) -> dict[str, Any]:
    """List recent captures in the workdir, newest first."""
    items = []
    for p in sorted(WORKDIR.glob("cap-*.json"), key=lambda x: x.stat().st_mtime,
                    reverse=True)[:limit]:
        try:
            meta = json.loads(p.read_text())
        except Exception:
            continue
        items.append({
            "capture_id": p.stem.removeprefix("cap-"),
            "samples": meta.get("samples_written"),
            "samplerate": meta.get("samplerate"),
            "channels": [c["index"] for c in meta.get("channels", [])
                         if c.get("enabled")],
            "mtime": p.stat().st_mtime,
        })
    return {"captures": items, "workdir": str(WORKDIR)}


@mcp.tool()
def analyze(capture_id: str, channel: int) -> dict[str, Any]:
    """Compute edge / pulse / frequency stats on a channel.

    Args:
        capture_id: id from `capture` or `list_captures`.
        channel: channel index (0..N-1).
    """
    cap = _load_capture(capture_id)
    bits = cap.channel_bits(channel)
    n = bits.size
    if n == 0:
        return {"error": "empty capture"}

    # Edges where the bit changes.
    diff = np.diff(bits.astype(np.int8))
    rising = np.flatnonzero(diff == 1)
    falling = np.flatnonzero(diff == -1)
    edges = np.sort(np.concatenate([rising, falling]))

    sr = cap.samplerate
    duration_s = n / sr if sr else 0.0

    high_count = int(bits.sum())
    duty = (high_count / n) if n else 0.0

    # Pulse widths between consecutive edges.
    pulse_widths_samples = np.diff(edges) if edges.size > 1 else np.array([])
    pulse_widths_s = pulse_widths_samples / sr if sr else pulse_widths_samples

    def _stats(arr: np.ndarray) -> dict[str, Any] | None:
        if arr.size == 0:
            return None
        return {
            "count": int(arr.size),
            "min": float(arr.min()),
            "max": float(arr.max()),
            "mean": float(arr.mean()),
            "median": float(np.median(arr)),
        }

    # Frequency estimate from rising-edge spacing.
    freq_hz = None
    if rising.size >= 2 and sr:
        period_samples = float(np.median(np.diff(rising)))
        if period_samples > 0:
            freq_hz = sr / period_samples

    return {
        "capture_id": capture_id,
        "channel": channel,
        "samples": int(n),
        "samplerate": sr,
        "duration_s": duration_s,
        "duty_cycle": duty,
        "high_samples": high_count,
        "low_samples": int(n - high_count),
        "edges": int(edges.size),
        "rising_edges": int(rising.size),
        "falling_edges": int(falling.size),
        "estimated_freq_hz": freq_hz,
        "pulse_width_s": _stats(pulse_widths_s),
    }


@mcp.tool()
def read_window(
    capture_id: str,
    channel: int,
    start_sample: int = 0,
    length: int = 128,
) -> dict[str, Any]:
    """Read raw bits for a small window of a channel.

    Args:
        capture_id: id from `capture`.
        channel: channel index.
        start_sample: first sample index (0-based).
        length: number of samples (default 128, capped at 4096). Keep small
            — this prints one '0'/'1' character per sample, so length=4096
            costs ~1k tokens. Use `analyze` for whole-channel stats and only
            reach for `read_window` when you need to eyeball a transition.

    Returns:
        {"bits": str of '0'/'1', "start_sample": int, "end_sample": int,
         "samplerate": int}
    """
    if length <= 0:
        return {"error": "length must be > 0"}
    length = min(length, MAX_WINDOW_SAMPLES)

    cap = _load_capture(capture_id)
    bits = cap.channel_bits(channel)
    end = min(start_sample + length, bits.size)
    if start_sample >= bits.size:
        return {"error": "start_sample beyond capture end",
                "samples": int(bits.size)}

    window = bits[start_sample:end]
    return {
        "channel": channel,
        "start_sample": start_sample,
        "end_sample": int(end),
        "samplerate": cap.samplerate,
        "bits": "".join(str(b) for b in window.tolist()),
    }


@mcp.tool()
def list_decoders(
    filter_substring: str | None = None,
    detail: bool = False,
) -> dict[str, Any]:
    """List available protocol decoders (i2c, spi, uart, ...).

    There are 150+ bundled decoders, so the unfiltered + detailed listing is
    enormous (~20k tokens). The default returns just `{id, name}` pairs and
    refuses to enumerate everything when no filter is given — supply a
    `filter_substring` first.

    Args:
        filter_substring: case-insensitive substring on id/name. **Required
            unless detail=True is also explicitly set.**
        detail: include channel ids and option ids per decoder. Defaults to
            False to save context; flip on once you've narrowed down to a
            decoder you actually want to call.

    Returns:
        compact (default):
            {"decoders": [{"id": str, "name": str}, ...], "count": int}
        detail=True:
            {"decoders": [
                {"id", "name", "longname",
                 "channels": [{"id", "name", "required"}, ...],
                 "options":  [{"id", "desc"}, ...]}, ...]}

    The DSL fork uses ids like "0:uart" / "1:uart" (two flavours of the
    same protocol). The `id` field is what you pass to `decode(protocol=...)`.
    """
    if not filter_substring and not detail:
        return {
            "error": "Pass a filter_substring (e.g. 'i2c', 'spi', 'uart') "
                     "or detail=True. Listing all 150+ decoders compactly "
                     "is allowed via filter_substring='', but unfiltered + "
                     "detail is intentionally blocked to save context.",
        }
    res = _run_helper(["list-decoders"], timeout=30)
    decoders = res.get("decoders", [])
    if filter_substring:
        s = filter_substring.lower()
        decoders = [
            d for d in decoders
            if s in d.get("id", "").lower()
            or s in d.get("name", "").lower()
            or s in d.get("longname", "").lower()
        ]
    if not detail:
        decoders = [{"id": d["id"], "name": d.get("name", d["id"])}
                    for d in decoders]
    return {"decoders": decoders, "count": len(decoders)}


_BIT_LEVEL_LABELS = {
    "miso-bits", "mosi-bits", "data-bits", "bit",
    "address-read", "address-write",  # i2c also emits these as bit-level pairs
    "stuff-bit",                       # CAN / CAN-FD bit-stuffing markers
    "warnings",  # noisy and rarely actionable for high-level summary
}


def _summarize_annotations(anns: list[dict[str, Any]]) -> dict[str, Any]:
    """Collapse a fat annotation list into a token-cheap summary.

    Groups same-label data annotations into hex byte streams (for SPI/I2C/UART
    style protocols) and keeps non-bit-level events as compact triplets so the
    LLM can still see Start / Stop / framing.
    """
    streams: dict[str, list[str]] = {}
    events: list[dict[str, Any]] = []
    for a in anns:
        label = a.get("label", "ann")
        if label in _BIT_LEVEL_LABELS:
            continue
        hex_v = a.get("hex")
        if hex_v:
            streams.setdefault(label, []).append(hex_v)
            continue
        # Compact event form: [start_sample, label, text_or_empty]
        text = a.get("text") or []
        events.append({
            "s": a["start"],
            "l": label,
            "t": text[0] if text else "",
        })
    out: dict[str, Any] = {}
    if streams:
        out["streams"] = {k: " ".join(v) for k, v in streams.items()}
    if events:
        out["events"] = events
    return out


@mcp.tool()
def decode(
    capture_id: str,
    protocol: str,
    channel_map: dict[str, int],
    options: dict[str, Any] | None = None,
    start_sample: int = 0,
    end_sample: int = 0,
    limit: int = 5000,
    output: str = "summary",
) -> dict[str, Any]:
    """Run a protocol decoder over a captured logic trace.

    The decoder may emit thousands of fine-grained bit-level annotations
    (e.g. SPI emits 8 miso-bit + 8 mosi-bit + miso-data + mosi-data per
    transferred byte — 18 entries each), which blows up the LLM context.
    The default `output="summary"` collapses those into one hex byte stream
    per data lane plus a short list of framing events.

    Args:
        capture_id: id from `capture` or `list_captures`.
        protocol:   decoder id from `list_decoders`, e.g. "0:i2c", "0:uart".
        channel_map: decoder channel id -> logic channel index, e.g.
                     {"scl": 0, "sda": 1}. **You can re-call decode() with
                     a different mapping if the result looks wrong — no need
                     to re-capture.**
        options:    decoder options keyed by id, e.g.
                     {"baudrate": 9600, "num_data_bits": 8}.
        start_sample / end_sample: inclusive/exclusive sample window
                     (0 = whole capture).
        limit:      max raw annotations to read from helper (default 5000).
        output:     "summary" (default, ~100x cheaper) or "raw" (every
                     annotation including bit-level entries).

    Returns (output="summary"):
        {"protocol": str, "samplerate": int, "window_samples": int,
         "raw_count": int, "truncated": bool,
         "streams": {"<label>": "AB CD EF ..."},   # hex byte streams
         "events":  [{"s": sample, "l": label, "t": text}, ...]}

    Returns (output="raw"):
        {"protocol": str, "samplerate": int, "window_samples": int,
         "annotations": [{"start", "end", "class", "label", "text", "hex"?}],
         "count": int, "truncated": bool}
    """
    cap = _load_capture(capture_id)
    bin_path = cap.bin_path
    prefix = str(bin_path)[:-4]  # strip ".bin"

    args = [
        "decode",
        "--input", prefix,
        "--protocol", protocol,
        "--map", ",".join(f"{k}={v}" for k, v in channel_map.items()),
        "--limit", str(limit),
    ]
    if options:
        args += ["--options", ",".join(f"{k}={v}" for k, v in options.items())]
    if start_sample:
        args += ["--start", str(start_sample)]
    if end_sample:
        args += ["--end", str(end_sample)]

    res = _run_helper(args, timeout=120)
    res.pop("ok", None)

    if output == "raw":
        return res

    anns = res.get("annotations", [])
    summary = _summarize_annotations(anns)
    return {
        "protocol":       res.get("protocol", protocol),
        "samplerate":     res.get("samplerate"),
        "window_samples": res.get("window_samples"),
        "raw_count":      res.get("count", len(anns)),
        "truncated":      res.get("truncated", False),
        **summary,
    }


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
