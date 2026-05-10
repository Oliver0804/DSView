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


def _run_helper(args: list[str], timeout: int = 60,
                retries: int = 2) -> dict[str, Any]:
    """Run dsview_helper with the given args and parse its JSON stdout.

    Retries up to `retries` times on USB-exclusion / active-device
    mismatch errors (with a 0.6s settle delay between attempts), since
    those are nearly always transient hotplug-enumeration races on a
    machine with multiple DSLogic units."""
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

    last_msg: str | None = None
    for attempt in range(retries + 1):
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

        if result.get("ok", False):
            return result

        last_msg = result.get("error", "unknown helper error")
        # Only retry transient USB races, not config errors.
        transient = ("active-device mismatch" in last_msg
                     or "USB exclusion" in last_msg
                     or ("open" in last_msg.lower()
                         and "fail" in last_msg.lower()))
        if attempt < retries and transient:
            time.sleep(0.6)
            continue

        msg = last_msg
        # Make common failures actionable for the LLM.
        if "active-device mismatch" in msg or "USB exclusion" in msg:
            msg += ("\n\nHint: another process is holding the requested "
                    "USB device. Try:\n"
                    "  • If the GUI is running, gui_set_active_device("
                    "index=0) to park it on Demo,\n"
                    "  • call list_devices() again — handles change "
                    "between helper spawns,\n"
                    "  • try the other DSLogic index when two are "
                    "attached.")
        elif "open" in msg.lower() and "fail" in msg.lower():
            msg += ("\n\nHint: USB busy. Wait ~2s and retry, or close "
                    "the DSView GUI / kill stale processes.")
        raise HelperError(msg)
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
    """List currently attached DSLogic / DSCope / Demo devices.

    The `index` is what you pass to `capture(index=...)`. Demo Device is
    always present (virtual driver, fixed protocol-pattern data — handy
    as a baseline). Two same-named DSLogic Plus units only differ by
    their `handle` (process-local pointer; values change between calls).

    Example:
        >>> list_devices()
        {"devices": [
            {"index": 0, "name": "Demo Device",  "handle": 4361044064},
            {"index": 1, "name": "DSLogic PLus", "handle": 51187451296},
            {"index": 2, "name": "DSLogic PLus", "handle": 51187451008},
        ]}
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

    Example:
        >>> device_info(index=1)
        {"name": "DSLogic PLus", "samplerate": 1000000, "depth": 1000000,
         "channels": [{"index": 0, "name": "SDA", "enabled": true}, ...]}
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
    name: str | None = None,
    trigger_channel: int | None = None,
    trigger_edge: str | None = None,
    trigger_pos_pct: int | None = None,
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
        name: free-form tag stored alongside the capture. Shows up in
             `list_captures` so you (and the LLM) can find this run
             later by name instead of the hex capture_id.
        trigger_channel: probe to trigger on (0..N-1). Pair with
             `trigger_edge`. Implies Buffer mode (auto-set if not
             specified — DSLogic only fires triggers in Buffer mode).
        trigger_edge: 'rising' | 'falling' | 'either' | 'high' | 'low'.
             Capture starts when this condition is met on
             trigger_channel. Omit / None disables the trigger.
        trigger_pos_pct: 0..100 — fraction of the buffer kept *before*
             the trigger fires (50 = half pre / half post). Default
             leaves the device's current setting.

    Example:
        >>> # Step 1: dry-run to inspect settings
        >>> capture(samplerate=1_000_000, depth=200_000, channels=[0,1])
        {"needs_confirm": true, "preview": {...}, "hint": "..."}
        >>> # Step 2: confirm to actually capture
        >>> capture(samplerate=1_000_000, depth=200_000, channels=[0,1],
        ...         vth=0.9, name="uart-bring-up", confirm=True)
        {"capture_id": "ab12cd34", "samples": 200000, ...}

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
        "name": name,
        "trigger_channel": trigger_channel,
        "trigger_edge": trigger_edge,
        "trigger_pos_pct": trigger_pos_pct,
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
    if trigger_channel is not None and trigger_channel >= 0:
        args += ["--trigger-channel", str(int(trigger_channel))]
    if trigger_edge:
        args += ["--trigger-edge", str(trigger_edge)]
    if trigger_pos_pct is not None:
        args += ["--trigger-pos", str(int(trigger_pos_pct))]

    t0 = time.time()
    _run_helper(args, timeout=timeout_sec + 30)
    elapsed = time.time() - t0

    cap = _load_capture(cap_id)

    # Persist user-supplied tag into the metadata file so list_captures
    # can surface it without re-deriving from log/timestamp guesses.
    if name:
        json_path = WORKDIR / f"cap-{cap_id}.json"
        try:
            meta = json.loads(json_path.read_text())
            meta["name"] = name
            json_path.write_text(json.dumps(meta, indent=2))
        except Exception:
            pass

    return {
        "capture_id": cap_id,
        "samples": cap.samples,
        "samplerate": cap.samplerate,
        "duration_s": round(elapsed, 3),
        "channels": cap.channel_indices,
        "name": name,
    }


@mcp.tool()
def list_captures(limit: int = 20) -> dict[str, Any]:
    """List recent captures in the workdir, newest first.

    Each entry surfaces the optional `name` you passed to capture(),
    so you can find a previous run by tag instead of remembering the
    hex capture_id.

    Example:
        >>> list_captures(limit=3)
        {"captures": [
            {"capture_id": "ab12cd34", "name": "uart-bring-up",
             "samples": 200000, "samplerate": 1000000,
             "channels": [0, 1], "device": "DSLogic PLus", ...},
            ...
        ], "workdir": "/Users/.../.dsview/captures"}
    """
    items = []
    for p in sorted(WORKDIR.glob("cap-*.json"), key=lambda x: x.stat().st_mtime,
                    reverse=True)[:limit]:
        try:
            meta = json.loads(p.read_text())
        except Exception:
            continue
        items.append({
            "capture_id": p.stem.removeprefix("cap-"),
            "name": meta.get("name"),
            "device": meta.get("active_device_name"),
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

    Example:
        >>> analyze(capture_id="ab12cd34", channel=0)
        {"samples": 200000, "samplerate": 1000000, "duration_s": 0.2,
         "duty_cycle": 0.5, "edges": 4000, "rising_edges": 2000,
         "falling_edges": 2000, "estimated_freq_hz": 10000.0,
         "pulse_width_s": {"min": 5e-5, "median": 5e-5, ...}}
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

    Example:
        >>> read_window(capture_id="ab12cd34", channel=0, length=64)
        {"channel": 0, "start_sample": 0, "end_sample": 64,
         "samplerate": 1000000,
         "bits": "0011000011001100110011001100..."}
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

    Example:
        >>> # I2C on SDA=ch0, SCL=ch1
        >>> decode(capture_id="ab12cd34", protocol="1:i2c",
        ...        channel_map={"sda": 0, "scl": 1})
        {"protocol": "1:i2c", ...,
         "streams": {"data-write": "04 02 02 02 06 07",
                     "data-read":  "4F 32 BB 32 BB 32 BB 1F FF"},
         "events": [{"s": 45147, "l": "start", "t": "Start"}, ...]}

        >>> # Got 0 annotations? Try swapping clk/cs/mosi/miso or
        >>> # different cpol/cpha — re-call decode() with new mapping;
        >>> # no need to re-capture.
        >>> decode(capture_id="ab12cd34", protocol="1:spi",
        ...        channel_map={"clk":12, "cs":13, "mosi":15, "miso":14},
        ...        options={"cpol": 1, "cpha": 1})
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


# ---------------------------------------------------------------------------
# GUI bridge: forward calls to the McpServer embedded in DSView.exe
# ---------------------------------------------------------------------------
#
# When DSView is running with the embedded MCP server enabled (Help menu →
# MCP Server, or DSVIEW_MCP_AUTOSTART=1), it listens on 127.0.0.1:7384 and
# exposes JSON-RPC methods that drive the live GUI session — the same place
# the user sees waveforms, decoder annotations, and toolbar buttons.
#
# The standalone helper-based tools above (capture/decode/...) work even
# without the GUI, but operate on a private SigSession; the user can't see
# what the LLM is doing. The `gui_*` family below routes through 7384 so
# every action (a capture, a config change, a button press) is reflected
# in the visible GUI window.
#
# These two paths are *both* useful:
#   - stdio path  → headless / CI / exclusive hardware access
#   - gui_* path  → interactive debugging where the human watches along
#
# The gui_* tools degrade gracefully: if 7384 is closed they return a hint
# explaining how to enable it.

import socket as _socket
import subprocess as _subprocess

_GUI_HOST = "127.0.0.1"
_GUI_PORT = 7384

# Where to find the DSView binary when launching it. Override with the
# DSVIEW_BINARY env var; otherwise we try the dev build location next to
# this checkout, then /Applications/DSView.app on macOS.
_DSVIEW_BINARY_CANDIDATES = [
    os.environ.get("DSVIEW_BINARY", ""),
    str(REPO_ROOT / "build.dir" / "DSView"),
    "/Applications/DSView.app/Contents/MacOS/DSView",
]


def _find_dsview_binary() -> str | None:
    for p in _DSVIEW_BINARY_CANDIDATES:
        if p and Path(p).is_file() and os.access(p, os.X_OK):
            return p
    return None


def _gui_is_listening() -> bool:
    try:
        _socket.create_connection((_GUI_HOST, _GUI_PORT),
                                  timeout=0.5).close()
        return True
    except OSError:
        return False


def _gui_call(method: str, params: dict | None = None,
              timeout: float = 10.0) -> dict[str, Any]:
    """Send one JSON-RPC request to the embedded MCP server in DSView.

    Returns the raw JSON-RPC response object (`{id, jsonrpc, result|error}`).
    Raises ConnectionRefusedError when the GUI server is not listening.
    """
    sock = _socket.create_connection((_GUI_HOST, _GUI_PORT), timeout=timeout)
    sock.settimeout(timeout)
    try:
        req = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params is not None:
            req["params"] = params
        sock.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    finally:
        sock.close()
    return json.loads(buf.decode().strip())


def _gui_invoke(method: str, params: dict | None = None,
                timeout: float = 10.0) -> dict[str, Any]:
    """High-level wrapper used by every gui_* tool. Translates connection
    refused / RPC error into structured tool output the LLM can act on."""
    try:
        rsp = _gui_call(method, params, timeout=timeout)
    except (ConnectionRefusedError, OSError) as e:
        return {
            "ok": False,
            "gui_running": False,
            "hint": (
                "DSView's embedded MCP server is not reachable at "
                f"{_GUI_HOST}:{_GUI_PORT}. Open DSView and either: "
                "1) toggle Help menu → 'MCP Server (port 7384)', or "
                "2) launch with DSVIEW_MCP_AUTOSTART=1."
            ),
            "error": str(e),
        }
    if "error" in rsp:
        return {"ok": False, "gui_running": True, "error": rsp["error"]}
    return {"ok": True, "gui_running": True, **(rsp.get("result") or {})}


@mcp.tool()
def gui_state() -> dict[str, Any]:
    """Snapshot of the live DSView GUI: active device, samplerate, depth,
    collect mode, whether a capture is in flight, MCP listener status."""
    return _gui_invoke("get_state")


@mcp.tool()
def gui_screenshot(path: str | None = None,
                   max_width: int | None = 1280) -> dict[str, Any]:
    """Save a PNG screenshot of the DSView main window. Returns the file
    path so the caller can open it with a vision-capable model.

    Args:
        path: optional output file path; defaults to /tmp/dsview_screenshot_*.png.
        max_width: downscale to this width in pixels (default 1280, set to
            None or 0 to keep full resolution — full-HD PNG can be ~400KB).
    """
    p: dict[str, Any] = {}
    if path:                       p["path"] = path
    if max_width and max_width > 0: p["max_width"] = int(max_width)
    return _gui_invoke("screenshot", p, timeout=15.0)


@mcp.tool()
def gui_run_start() -> dict[str, Any]:
    """Press the toolbar 'Start' button — begin a normal capture in DSView.
    Same as calling SigSession::start_capture(false)."""
    return _gui_invoke("run_start")


@mcp.tool()
def gui_run_stop() -> dict[str, Any]:
    """Press the toolbar 'Stop' button — abort an in-flight capture."""
    return _gui_invoke("run_stop")


@mcp.tool()
def gui_instant_shot(auto_screenshot: bool = False,
                     screenshot_path: str | None = None,
                     wait_seconds: float = 2.0) -> dict[str, Any]:
    """Press the toolbar 'Instant' button — single-shot quick capture.

    Args:
        auto_screenshot: capture a PNG of the GUI after the shot
            settles. Saves the LLM one round-trip when it's about to
            show the user a result anyway.
        screenshot_path: where to write the PNG (default
            /tmp/dsv_after_instant_<ts>.png). Only used when
            auto_screenshot=True.
        wait_seconds: how long to wait for the capture to render
            before grabbing the screenshot.

    Example:
        >>> gui_instant_shot(auto_screenshot=True)
        {"ok": true, "action": "instant_shot",
         "screenshot": {"path": "/tmp/dsv_after_instant_1700.png",
                        "width": 1280, "height": 800, "bytes": 92103}}
    """
    res = _gui_invoke("instant_shot")
    if not auto_screenshot or not res.get("ok"):
        return res
    time.sleep(max(0.1, wait_seconds))
    path = screenshot_path or (f"/tmp/dsv_after_instant_"
                               f"{int(time.time()*1000)}.png")
    shot = _gui_invoke("screenshot",
                       {"path": path, "max_width": 1280}, timeout=15)
    res["screenshot"] = shot
    return res


@mcp.tool()
def gui_set_collect_mode(mode: str) -> dict[str, Any]:
    """Pick the toolbar capture-mode dropdown.

    Args:
        mode: 'single' | 'repeat' | 'loop'.
    """
    return _gui_invoke("set_collect_mode", {"mode": mode})


@mcp.tool()
def gui_set_active_device(index: int) -> dict[str, Any]:
    """Switch the toolbar device dropdown to the given index (from
    list_devices). Note: the underlying call updates the libsigrok driver
    state; current versions may not redraw the GUI's device combo until
    the user clicks once. Use gui_state to verify."""
    return _gui_invoke("set_active_device", {"index": int(index)})


@mcp.tool()
def gui_set_config(
    samplerate: int | None = None,
    depth: int | None = None,
    vth: float | None = None,
    operation_mode: int | None = None,
    buffer_option: int | None = None,
    filter: int | None = None,         # noqa: A002 — mirrors GUI label
    channel_mode: int | None = None,
    rle: bool | None = None,
    ext_clock: bool | None = None,
    falling_edge_clock: bool | None = None,
    max_height: str | None = None,
    pattern_mode: str | None = None,
) -> dict[str, Any]:
    """Drive the 'Options' (設備選項) dialog and the samplerate/depth
    dropdowns in one call. Pass only the fields you want to change; the
    rest stay at the GUI's current values.

    Returns `{applied: {...}, errors: {...}}` per key — some options
    (filter / operation_mode / channel_mode) may be rejected by the
    driver depending on the device's current state, in which case they
    end up in `errors`."""
    p: dict[str, Any] = {}
    for k, v in (
        ("samplerate", samplerate), ("depth", depth), ("vth", vth),
        ("operation_mode", operation_mode), ("buffer_option", buffer_option),
        ("filter", filter), ("channel_mode", channel_mode),
        ("rle", rle), ("ext_clock", ext_clock),
        ("falling_edge_clock", falling_edge_clock),
        ("max_height", max_height),
        ("pattern_mode", pattern_mode),
    ):
        if v is not None:
            p[k] = v
    if not p:
        return {"ok": True, "applied": {}, "hint": "no fields specified"}
    return _gui_invoke("set_config", p)


@mcp.tool()
def gui_load_session(path: str) -> dict[str, Any]:
    """Open a previously-saved `.dsl` session file in the running GUI.
    Counterpart to `gui_save_session` — lets the LLM (or user) keep
    re-analysing a fixed dataset across conversations.

    Args:
        path: absolute path to a `.dsl` file. The GUI's active device
            and channel layout are replaced with whatever was in the
            file; current capture buffer is dropped.

    Example:
        >>> gui_load_session("/tmp/dsv_demo_run.dsl")
        {"ok": true, "path": "/tmp/dsv_demo_run.dsl", "device": "Demo Device"}
    """
    return _gui_invoke("load_session", {"path": path}, timeout=30)


@mcp.tool()
def gui_save_session(path: str | None = None) -> dict[str, Any]:
    """Save the current GUI capture (samples + decoder stack + channel
    layout) as a `.dsl` session file the user can re-open later in
    DSView. Equivalent to clicking File → Save in the GUI but without
    popping a dialog.

    Args:
        path: absolute output path. Auto-named into the user's home dir
            (`<device>-<mode>-<timestamp>.dsl`) when omitted.

    Returns:
        {ok, path, bytes} — `path` is what was actually written, even
        when the caller didn't supply one. Surface this to the user so
        they know where to look.

    Example:
        >>> gui_save_session("/tmp/my_run.dsl")
        {"ok": true, "path": "/tmp/my_run.dsl", "bytes": 248501}
    """
    p: dict[str, Any] = {}
    if path: p["path"] = path
    return _gui_invoke("save_session", p, timeout=60)


@mcp.tool()
def launch_gui(prefer_device: str = "demo",
               wait_seconds: float = 8.0) -> dict[str, Any]:
    """Spawn the DSView GUI app with the embedded MCP server enabled,
    so subsequent `gui_*` calls can drive it. Returns once the server
    is listening on 127.0.0.1:7384, or fails after `wait_seconds`.

    Already-running GUI: returns immediately with {already_running: true}.

    Args:
        prefer_device: name substring of the device to land on at boot
            ('demo' | 'dslogic' | 'dscope' | ''). 'demo' leaves any
            attached USB DSLogic free for parallel stdio access.
        wait_seconds: how long to wait for port 7384 to come up.

    Example:
        >>> launch_gui()                            # default: GUI on Demo
        {"ok": true, "already_running": false, "pid": 12345, "port": 7384}
        >>> launch_gui(prefer_device="dslogic")     # GUI grabs DSLogic
    """
    if _gui_is_listening():
        return {"ok": True, "already_running": True, "port": _GUI_PORT}

    binary = _find_dsview_binary()
    if not binary:
        return {"ok": False, "error": "DSView binary not found",
                "hint": "set DSVIEW_BINARY env or build the GUI "
                        "(`cmake --build build_dsview -j`)"}

    env = os.environ.copy()
    env["DSVIEW_MCP_AUTOSTART"] = "1"
    if prefer_device:
        env["DSVIEW_DEFAULT_DEVICE"] = prefer_device

    proc = _subprocess.Popen(
        [binary],
        env=env,
        stdout=_subprocess.DEVNULL,
        stderr=_subprocess.DEVNULL,
        start_new_session=True,
    )

    deadline = time.time() + wait_seconds
    while time.time() < deadline:
        if _gui_is_listening():
            return {"ok": True, "already_running": False,
                    "pid": proc.pid, "port": _GUI_PORT, "binary": binary}
        time.sleep(0.2)

    return {"ok": False, "error": "GUI did not open port 7384 in time",
            "pid": proc.pid, "binary": binary,
            "hint": "GUI may have failed to start; check for stale "
                    "DSView processes or port conflicts."}


@mcp.tool()
def gui_set_channel(channels: dict[str, bool] | None = None,
                    index: int | None = None,
                    enabled: bool | None = None) -> dict[str, Any]:
    """Enable/disable logic channels.

    Args:
        channels: batch map like {"0": true, "1": false, ...} (keys as str).
        index / enabled: single-channel form, e.g. index=5, enabled=True.
    """
    p: dict[str, Any] = {}
    if channels is not None:
        # normalize int keys → str so the JSON object is well-formed
        p["channels"] = {str(k): bool(v) for k, v in channels.items()}
    if index is not None:
        p["index"] = int(index)
        if enabled is not None:
            p["enabled"] = bool(enabled)
    if not p:
        return {"ok": False, "hint": "pass channels=... or index=...,enabled=..."}
    return _gui_invoke("set_channel", p)


_PROTOCOL_PIN_HINTS: dict[str, dict[str, list[str]]] = {
    # decoder_id (without 0:/1: prefix) -> pin_id -> channel-name keywords.
    # First match wins. Case-insensitive substring match against the user's
    # channel naming in the GUI / capture metadata.
    "i2c":  {"sda": ["sda", "data"], "scl": ["scl", "clk"]},
    "spi":  {"clk": ["clk", "sck", "sclk"],
             "mosi": ["mosi", "do",  "sdi", "tx"],
             "miso": ["miso", "di",  "sdo", "rx"],
             "cs":   ["cs",   "ce",  "ss",  "select"]},
    "uart": {"rxtx": ["rxtx", "tx", "rx", "uart", "ser"]},
    "can":  {"can_rx": ["can", "rx"]},
}


def _suggest_channel_map(protocol: str,
                         channels_meta: list[dict[str, Any]]) -> dict[str, int]:
    """Heuristically map decoder pins → logic channel indices using the
    user's channel names. `protocol` may be 'i2c' / '0:i2c' / '1:i2c'.
    Returns a partial mapping (only pins whose name confidently matched);
    the caller should reject if a required pin is missing."""
    key = protocol.split(":")[-1].lower()
    hints = _PROTOCOL_PIN_HINTS.get(key, {})
    if not hints:
        return {}
    enabled = [c for c in channels_meta if c.get("enabled")]

    mapped: dict[str, int] = {}
    used: set[int] = set()
    for pin, kw_list in hints.items():
        for c in enabled:
            if c["index"] in used:
                continue
            cname = (c.get("name") or "").lower()
            if any(kw in cname for kw in kw_list):
                mapped[pin] = c["index"]
                used.add(c["index"])
                break
    return mapped


@mcp.tool()
def suggest_channel_map(capture_id: str, protocol: str) -> dict[str, Any]:
    """Heuristically infer a `channel_map` for `decode()` from the user's
    channel names. Useful when the user has already labeled channels in
    DSView (e.g. ch0='SDA', ch1='SCL') — saves the LLM from re-asking.

    Args:
        capture_id: a capture saved by capture(...).
        protocol:   decoder id (e.g. "1:i2c", "0:spi", "uart", "can-fd").

    Example:
        >>> suggest_channel_map(capture_id="ab12cd34", protocol="1:spi")
        {"channel_map": {"clk": 1, "mosi": 14, "miso": 15, "cs": 13},
         "matched": 4, "missing_required": []}
    """
    cap = _load_capture(capture_id)
    chs = [{"index": c["index"], "enabled": c.get("enabled", False),
            "name": c.get("name", "")} for c in cap.meta.get("channels", [])]
    cmap = _suggest_channel_map(protocol, chs)

    # Required pins from the DSL decoder list (best-effort — the helper
    # already exposes that, but to keep this Pythonic we mirror the
    # known minima here):
    required = {
        "i2c":  ["sda", "scl"],
        "spi":  ["clk"],            # cs/mosi/miso optional
        "uart": ["rxtx"],
        "can":  ["can_rx"],
    }.get(protocol.split(":")[-1].lower(), [])
    missing = [p for p in required if p not in cmap]

    return {
        "channel_map": cmap,
        "matched": len(cmap),
        "missing_required": missing,
        "hint": ("All required pins matched — pass channel_map straight "
                 "to decode().") if not missing else
                ("Channel names didn't cover required pin(s) " +
                 ", ".join(missing) +
                 ". Either rename the channels in DSView (Options → "
                 "rename) or pass channel_map explicitly to decode()."),
    }


@mcp.tool()
def auto_capture_and_decode(
    protocol: str,
    samplerate: int = 1_000_000,
    depth: int = 200_000,
    index: int = -1,
    vth: float | None = 0.9,
    options: dict[str, Any] | None = None,
    name: str | None = None,
) -> dict[str, Any]:
    """One-shot capture + decode. Captures with all-channels-enabled,
    auto-derives channel_map from device-info channel names, then runs
    the protocol decoder in summary mode.

    Use this when the user just says "show me the I²C traffic" —
    you don't have to chain capture → list_devices → decode by hand.

    If channel-name auto-mapping fails, the response includes a
    `suggested_channel_map` with whatever could be inferred plus the
    raw `channels` list, so the caller can recover with one explicit
    decode() call.

    Args:
        protocol:   decoder id (e.g. "1:i2c", "0:spi", "uart").
        samplerate: capture rate.
        depth:      capture depth (samples).
        index:      device index from list_devices (-1 = last attached).
        vth:        voltage threshold (default 0.9 for 1.8V logic).
        options:    decoder options (baudrate / cpol / cpha / etc.).
        name:       free-form tag for the capture.

    Example:
        >>> auto_capture_and_decode(protocol="1:i2c",
        ...                         samplerate=25_000_000, depth=131_072,
        ...                         index=0, vth=None,
        ...                         name="demo-baseline")
        {"capture_id": "...", "protocol": "1:i2c",
         "channel_map": {"sda": 0, "scl": 1},
         "streams": {"data-write": "04 02 02 02 06 07", ...},
         "events":  [{"s": 45147, "l": "start", "t": "Start"}, ...]}
    """
    info = device_info(index=index)
    chs = info.get("channels", [])
    if not chs:
        return {"ok": False, "error": "no enabled channels on this device",
                "hint": "Run list_devices then device_info to inspect."}

    enabled_idx = [c["index"] for c in chs]
    cap = capture(
        samplerate=samplerate, depth=depth, channels=enabled_idx,
        index=index, vth=vth, name=name, confirm=True,
    )
    cap_id = cap.get("capture_id")
    if not cap_id:
        return {"ok": False, "error": "capture failed", "capture_response": cap}

    cmap = _suggest_channel_map(protocol, chs)
    if not cmap:
        return {
            "ok": False,
            "capture_id": cap_id,
            "error": "could not auto-map any decoder pin",
            "channels": [{"index": c["index"], "name": c.get("name")}
                          for c in chs],
            "hint": ("Rename channels in DSView (e.g. SDA / SCL / CLK / "
                     "MOSI / MISO / RXTX) or call decode() directly with "
                     "an explicit channel_map."),
        }

    res = decode(
        capture_id=cap_id, protocol=protocol,
        channel_map=cmap, options=options,
    )
    res["capture_id"]  = cap_id
    res["channel_map"] = cmap
    res["captured_channels"] = [
        {"index": c["index"], "name": c.get("name")} for c in chs
    ]
    return res


_WORKFLOWS = [
    {
        "name": "demo-baseline",
        "purpose": "Sanity-check the whole MCP/decoder stack against "
                   "fixed Demo Device data (deterministic — handy for "
                   "regression / proving the wiring works).",
        "steps": [
            "list_devices()                       # confirm Demo at index 0",
            "capture(index=0, samplerate=25_000_000, depth=131_072,"
            "        channels=list(range(16)), name='demo-baseline',"
            "        confirm=True)",
            "decode(capture_id=..., protocol='1:i2c',"
            "       channel_map={'sda': 0, 'scl': 1})",
            "# expect data-write '04 02 02 02 06 07' / "
            "data-read '4F 32 BB 32 BB 32 BB 1F FF' (fixed pattern)",
        ],
    },
    {
        "name": "real-uart",
        "purpose": "Capture a UART line on a 1.8 V system and decode.",
        "steps": [
            "list_devices()",
            "capture(index=1, samplerate=2_000_000, depth=200_000,"
            "        channels=[0], vth=0.9, name='uart-tx',"
            "        confirm=True)",
            "analyze(capture_id=..., channel=0)"
            "  # estimated_freq_hz / pulse_width to back out baudrate",
            "decode(capture_id=..., protocol='1:uart',"
            "       channel_map={'rxtx': 0},"
            "       options={'baudrate': 115200, 'num_data_bits': 8})",
        ],
    },
    {
        "name": "spi-iterate-mapping",
        "purpose": "When SPI returns 0 annotations, try alternative "
                   "channel mappings without re-capturing.",
        "steps": [
            "capture(..., channels=[0,1,2,3], name='spi-probe', confirm=True)",
            "# Try the obvious mapping first",
            "decode(capture_id=..., protocol='1:spi',"
            "       channel_map={'clk':0,'mosi':1,'miso':2,'cs':3},"
            "       options={'cpol':0,'cpha':0,'cs_polarity':'active-low'})",
            "# Got 0 byte? Swap MOSI/MISO and tweak cpol/cpha:",
            "for clk_idx in (0,1,2,3):",
            "    for mosi, miso in [(1,2),(2,1)]:",
            "        decode(... channel_map={'clk':clk_idx,'mosi':mosi,...})",
            "# read_window(capture_id=..., channel=clk_idx) shows which "
            "  pin actually carries the clock.",
        ],
    },
    {
        "name": "interactive-with-gui",
        "purpose": "Spin up the DSView GUI, drive it from MCP, save the "
                   "capture as a .dsl the user can re-open.",
        "steps": [
            "launch_gui(prefer_device='demo')",
            "# GUI on Demo so the USB DSLogic stays free for stdio",
            "gui_set_config(samplerate=25_000_000, depth=131_072,"
            "               pattern_mode='protocol')",
            "gui_set_channel(channels={str(i): True for i in range(16)})",
            "gui_set_collect_mode('single')",
            "gui_instant_shot()",
            "gui_screenshot(path='/tmp/dsv_run.png', max_width=1280)",
            "# now show the user the screenshot + persist data:",
            "gui_save_session(path='/tmp/dsv_run.dsl')",
            "# returned {ok, path, bytes} — surface the path to the user",
        ],
    },
    {
        "name": "two-agents-different-devices",
        "purpose": "Run GUI on Demo while stdio drives a real DSLogic in "
                   "parallel — typical Claude Code session.",
        "steps": [
            "# 1) Start GUI on Demo so it doesn't grab the USB device",
            "launch_gui(prefer_device='demo')",
            "# 2) From stdio side, capture the real Plus",
            "list_devices()",
            "capture(index=1, samplerate=5_000_000, depth=300_000,"
            "        channels=[0,1,2,3], name='hw-spi-burst',"
            "        confirm=True)",
            "# Verify metadata.active_device_name == 'DSLogic PLus'",
            "# (capture aborts hard if libsigrok fell back to another "
            " device due to USB exclusion).",
        ],
    },
    {
        "name": "recover-from-busy",
        "purpose": "What to do when capture errors with 'active-device "
                   "mismatch' or 'USB busy'.",
        "steps": [
            "# 1) Check who's holding it — gui_state shows the GUI's "
            " active device:",
            "gui_state()  # via stdio bridge if GUI is up",
            "# 2) Park the GUI on Demo to release the USB device:",
            "gui_set_active_device(index=0)",
            "# 3) Retry the original stdio capture:",
            "capture(index=1, ..., confirm=True)",
        ],
    },
]


@mcp.tool()
def workflows() -> dict[str, Any]:
    """Built-in 'recipe book' — tested step-by-step examples covering
    common DSView MCP workflows. Use this when the user's request is
    fuzzy ('show me UART traffic', 'save the capture', 'two devices at
    once') and you want a known-good template instead of reasoning
    from individual tool docstrings.

    Each recipe has `name`, `purpose`, and a list of pseudo-Python
    `steps` that already account for the gotchas (dry-run + confirm
    flow, channel-map iteration, GUI-vs-stdio device exclusion, etc.).

    Example:
        >>> workflows()
        {"recipes": [{"name": "demo-baseline", ...},
                     {"name": "real-uart", ...},
                     {"name": "spi-iterate-mapping", ...},
                     {"name": "interactive-with-gui", ...},
                     {"name": "two-agents-different-devices", ...},
                     {"name": "recover-from-busy", ...}]}
    """
    return {"recipes": _WORKFLOWS}


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
