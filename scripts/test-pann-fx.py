"""Verify the packaged PANN rack ABI, DSP math, telemetry, state, and GDI+ editor."""

import ctypes as c
from ctypes import wintypes as w
import json
import math
from pathlib import Path
import struct
import sys

u32 = c.c_uint32
i32 = c.c_int32
sz = c.c_size_t
handle = c.c_void_p
floats = c.POINTER(c.c_float)
doubles = c.POINTER(c.c_double)
bytes_ptr = c.POINTER(c.c_uint8)

EditFn = c.CFUNCTYPE(None, handle, i32, i32, c.c_double)
TranslateFn = c.CFUNCTYPE(handle, handle, c.c_char_p)


class Rack(c.Structure):
    _fields_ = [
        ("size", u32),
        ("abi", u32),
        ("code", c.c_char_p),
        ("fuid", c.c_char_p),
        ("version", c.c_char_p),
        ("create", c.CFUNCTYPE(handle)),
        ("destroy", c.CFUNCTYPE(None, handle)),
        ("prepare", c.CFUNCTYPE(c.c_int, handle, u32, u32, u32, c.POINTER(u32))),
        ("process", c.CFUNCTYPE(None, handle, floats, floats, u32, c.c_double)),
        ("get", c.CFUNCTYPE(c.c_float, handle, u32)),
        ("set", c.CFUNCTYPE(c.c_int, handle, u32, c.c_float)),
        ("save", c.CFUNCTYPE(u32, handle, bytes_ptr, u32)),
        ("restore", c.CFUNCTYPE(c.c_int, handle, bytes_ptr, u32)),
        ("visual_active", c.CFUNCTYPE(None, handle, c.c_int)),
        ("visual", c.CFUNCTYPE(u32, handle, floats, u32)),
    ]


class Editor(c.Structure):
    _fields_ = [
        ("size", u32),
        ("version", u32),
        ("width", i32),
        ("height", i32),
        ("create", c.CFUNCTYPE(handle, handle, doubles, sz, i32, i32, c.c_float, EditFn, handle)),
        ("destroy", c.CFUNCTYPE(None, handle)),
        ("resize", c.CFUNCTYPE(None, handle, i32, i32, c.c_float)),
        ("parameter", c.CFUNCTYPE(None, handle, sz, c.c_double)),
        ("visual", c.CFUNCTYPE(None, handle, floats, sz)),
        ("translate", c.CFUNCTYPE(None, handle, TranslateFn, handle)),
    ]


class MSG(c.Structure):
    _fields_ = [
        ("hwnd", w.HWND),
        ("message", w.UINT),
        ("wParam", w.WPARAM),
        ("lParam", w.LPARAM),
        ("time", w.DWORD),
        ("pt", w.POINT),
    ]


def pump_messages():
    user32 = c.windll.user32
    msg = MSG()
    while user32.PeekMessageW(c.byref(msg), None, 0, 0, 1):
        user32.TranslateMessage(c.byref(msg))
        user32.DispatchMessageW(c.byref(msg))


def main(dll_path: str):
    library = c.CDLL(str(Path(dll_path).resolve()))

    # 1. Rack ABI & Definition JSON
    library.audcore_fx_get_rack.argtypes = [u32]
    library.audcore_fx_get_rack.restype = c.POINTER(Rack)
    assert not library.audcore_fx_get_rack(1)
    rack_ptr = library.audcore_fx_get_rack(2)
    assert rack_ptr
    rack = rack_ptr.contents
    assert rack.code == b"PANN"
    assert rack.fuid == b"A1B2C3D4E5F647898A9B0C1D2E3F4A5B"
    dll_name = Path(dll_path).name
    if dll_name.startswith("pann-v") and dll_name.endswith(".fx.audcore"):
        expected_ver = dll_name[len("pann-v") : -len(".fx.audcore")].encode("utf-8")
        assert rack.version == expected_ver, f"Expected version {expected_ver}, got {rack.version}"
    assert rack.size == c.sizeof(Rack)

    library.audcore_fx_get_definition.argtypes = [u32, c.POINTER(u32)]
    library.audcore_fx_get_definition.restype = handle
    def_size = u32()
    def_ptr = library.audcore_fx_get_definition(1, c.byref(def_size))
    assert def_ptr and def_size.value > 0
    definition = json.loads(c.string_at(def_ptr, def_size.value))
    assert definition["code"] == "PANN"
    assert definition["fuid"] == rack.fuid.decode()
    assert definition["vendor"] == "Nytera"
    assert definition["label"]["zh-CN"] == "声场旋转"
    assert len(definition["parameters"]) == 1
    param0 = definition["parameters"][0]
    assert param0["id"] == "angle"
    assert param0["minimum"] == -180 and param0["maximum"] == 180
    assert param0["step"] == 1
    assert param0["default"] == 0 and param0["component"] == "slider"

    # 2. Editor ABI presence & flags
    library.audcore_fx_get_editor.argtypes = [u32]
    library.audcore_fx_get_editor.restype = c.POINTER(Editor)
    assert not library.audcore_fx_get_editor(2)
    editor_ptr = library.audcore_fx_get_editor(1)
    assert editor_ptr, "audcore_fx_get_editor(1) returned NULL"
    editor = editor_ptr.contents
    assert editor.size == c.sizeof(Editor)
    assert (editor.width, editor.height) == (420, 480)

    library.audcore_fx_editor_flags.argtypes = [u32]
    library.audcore_fx_editor_flags.restype = u32
    assert library.audcore_fx_editor_flags(1) == 1
    assert library.audcore_fx_editor_flags(2) == 0

    # 3. Rack instances, prepare validation, DSP rotation, smoothing, mono, state, telemetry
    inst0 = rack.create()
    inst1 = rack.create()
    assert inst0 and inst1
    try:
        latency = u32(99)
        assert not rack.prepare(inst0, 0, 128, 2, c.byref(latency))
        assert not rack.prepare(inst0, 48000, 128, 3, c.byref(latency))
        assert rack.prepare(inst0, 48000, 128, 2, c.byref(latency)) == 1
        assert latency.value == 0

        # Default 0 deg (normalized 0.5): identity transform
        assert abs(rack.get(inst0, 0) - 0.5) < 1e-6
        buf = (c.c_float * 8)(0.6, -0.3, -0.4, 0.8, 0.1, 0.2, -0.7, 0.5)
        expected_orig = list(buf)
        rack.process(inst0, buf, None, 4, 120.0)
        for a, b in zip(buf, expected_orig):
            assert abs(a - b) < 1e-6

        # Invalid set calls rejected
        assert not rack.set(inst0, 1, 0.5)
        assert not rack.set(inst0, 0, -0.01)
        assert not rack.set(inst0, 0, 1.01)
        assert not rack.set(inst0, 0, float("nan"))
        assert abs(rack.get(inst0, 0) - 0.5) < 1e-6

        # 5 ms smoothing on set(0.75 -> +90 deg) at 48 kHz (240 samples)
        # Under z = L + iR: outL = L*cos(w) - R*sin(w), outR = R*cos(w) + L*sin(w)
        assert rack.set(inst0, 0, 0.75) == 1
        smooth_buf = (c.c_float * (300 * 2))(*([0.6, 0.2] * 300))
        rack.process(inst0, smooth_buf, None, 300, 120.0)
        # First sample must be mid-transition (not jumped to +90 deg outL=-0.2, outR=0.6)
        assert -0.19 < smooth_buf[0] < 0.599 and smooth_buf[1] > 0.201
        # Sample 120 (halfway through 240-sample 5ms ramp -> +45 deg): outL = (0.6-0.2)/sqrt(2), outR = (0.2+0.6)/sqrt(2)
        assert abs(smooth_buf[119 * 2] - (0.4 / math.sqrt(2.0))) < 1e-3
        assert abs(smooth_buf[119 * 2 + 1] - (0.8 / math.sqrt(2.0))) < 1e-3
        # Sample 299 (after 240-sample 5ms ramp) must be settled at +90 deg: outL = -R = -0.2, outR = L = 0.6
        assert abs(smooth_buf[299 * 2] - (-0.2)) < 1e-5
        assert abs(smooth_buf[299 * 2 + 1] - 0.6) < 1e-5

        # Mono transport (channels == 1): reads stereo[2*i] as mono L=R and rotates across stereo output
        # Under z = L + iR at +45 deg (0.625): mono L=R=1/sqrt(2) -> outL = 0.0, outR = 1.0
        assert rack.set(inst1, 0, 0.625) == 1  # +45 deg
        assert rack.prepare(inst1, 48000, 64, 1, c.byref(latency)) == 1
        inv_sqrt2 = 1.0 / math.sqrt(2.0)
        mono_buf = (c.c_float * 4)(inv_sqrt2, -999.0, inv_sqrt2, 999.0)
        rack.process(inst1, mono_buf, None, 2, 120.0)
        assert abs(mono_buf[0] - 0.0) < 1e-5
        assert abs(mono_buf[1] - 1.0) < 1e-5

        # Visual telemetry gating & sample stream
        vis_out = (c.c_float * 64)()
        assert rack.visual(inst0, vis_out, 64) == 0  # visual_active is 0
        rack.visual_active(inst0, 1)
        vis_in = (c.c_float * 8)(0.6, 0.2, 0.6, 0.2, 0.6, 0.2, 0.6, 0.2)
        rack.process(inst0, vis_in, None, 4, 120.0)
        n_floats = rack.visual(inst0, vis_out, 64)
        assert n_floats == 8
        # At +90 deg under z = L + iR, outL = -0.2, outR = 0.6; visual emits interleaved (outR, outL)
        assert abs(vis_out[0] - 0.6) < 1e-5 and abs(vis_out[1] - (-0.2)) < 1e-5
        rack.visual_active(inst0, 0)
        assert rack.visual(inst0, vis_out, 64) == 0

        # State save & restore (v2 6-byte state with collapsed & extendedRange flags + legacy v1 5-byte state)
        req_bytes = rack.save(inst0, None, 0)
        assert req_bytes == 6
        state_buf = (c.c_uint8 * req_bytes)()
        assert rack.save(inst0, state_buf, req_bytes) == 6
        assert state_buf[0] == 2 and state_buf[5] == 0
        assert rack.restore(inst1, state_buf, req_bytes) == 1
        assert abs(rack.get(inst1, 0) - 0.75) < 1e-6

        # Legacy v1 5-byte state still restores cleanly
        legacy_v1 = (c.c_uint8 * 5).from_buffer_copy(b"\x01" + struct.pack("<f", 0.625))
        assert rack.restore(inst1, legacy_v1, 5) == 1
        assert abs(rack.get(inst1, 0) - 0.625) < 1e-6

        bad_ver = (c.c_uint8 * 6)(3, 0, 0, 0, 0, 0)
        assert not rack.restore(inst1, bad_ver, 6)
        bad_flags = (c.c_uint8 * 6).from_buffer_copy(b"\x02" + struct.pack("<f", 0.5) + b"\x04")
        assert not rack.restore(inst1, bad_flags, 6)
        bad_float = (c.c_uint8 * 6).from_buffer_copy(b"\x02" + struct.pack("<f", 1.5) + b"\x00")
        assert not rack.restore(inst1, bad_float, 6)
        assert rack.restore(inst1, None, 0) == 1
        assert abs(rack.get(inst1, 0) - 0.5) < 1e-6
    finally:
        rack.destroy(inst0)
        rack.destroy(inst1)

    # 4. Offscreen Win32 Host HWND + Custom GDI+ Editor lifecycle + Live Rack->Editor scope bridge
    live_inst = rack.create()
    assert live_inst
    latency = u32(0)
    assert rack.prepare(live_inst, 48000, 256, 2, c.byref(latency)) == 1
    assert rack.set(live_inst, 0, 0.625) == 1  # +45 deg

    user32 = c.windll.user32
    user32.CreateWindowExW.argtypes = [
        w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
        c.c_int, c.c_int, c.c_int, c.c_int,
        w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID,
    ]
    user32.CreateWindowExW.restype = w.HWND
    user32.DestroyWindow.argtypes = [w.HWND]
    user32.UpdateWindow.argtypes = [w.HWND]

    parent_hwnd = user32.CreateWindowExW(
        0, "STATIC", "PannHostTest", 0x80000000,  # WS_POPUP (hidden)
        0, 0, 420, 480, None, None, None, None
    )
    assert parent_hwnd, "Failed to create offscreen parent HWND"
    try:
        edits = []

        @EditFn
        def on_edit(ctx, kind, index, value):
            edits.append((kind, index, value))

        translated_str = c.create_string_buffer(b"Pann")

        @TranslateFn
        def on_translate(ctx, key):
            return c.addressof(translated_str)

        init_vals = (c.c_double * 1)(0.5)
        ed = editor.create(parent_hwnd, init_vals, 1, 420, 480, 1.0, on_edit, None)
        assert ed, "editor.create returned NULL"
        try:
            editor.translate(ed, on_translate, None)

            # Stream live audio through rack.process() and trigger WM_TIMER on the editor child window
            live_audio = (c.c_float * 512)()
            for i in range(256):
                live_audio[2 * i] = math.sin(i * 0.1) * 0.7
                live_audio[2 * i + 1] = math.cos(i * 0.1) * 0.7
            rack.process(live_inst, live_audio, None, 256, 120.0)

            # Feed 1024 points (2048 floats) including NaN to verify stride downsampling & scatter NaN resilience
            vis_batch = (c.c_float * 2048)()
            for i in range(1024):
                angle = i * 0.05
                vis_batch[2 * i] = math.cos(angle) * 0.8
                vis_batch[2 * i + 1] = math.sin(angle) * 0.8
            vis_batch[10] = float("nan")
            editor.visual(ed, vis_batch, 2048)

            editor.resize(ed, 525, 600, 1.25)
            user32.UpdateWindow(parent_hwnd)
            pump_messages()

            # Simulate mouse drag & double-click on the editor child window
            user32.GetWindow.argtypes = [w.HWND, w.UINT]
            user32.GetWindow.restype = w.HWND
            user32.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
            user32.GetClientRect.restype = w.BOOL
            user32.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
            user32.SendMessageW.restype = w.LPARAM
            user32.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
            user32.SetWindowTextW.restype = w.BOOL
            user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
            user32.GetWindowTextW.restype = c.c_int
            child_hwnd = user32.GetWindow(parent_hwnd, 5)  # GW_CHILD
            assert child_hwnd, "Editor child HWND not found"
            edit_hwnd = user32.GetWindow(child_hwnd, 5)  # GW_CHILD (the EDIT textbox)
            assert edit_hwnd, "Integer input EDIT textbox HWND not found"

            # Fire WM_TIMER (0x0113, timer id 1) to pull sampled points from live_inst and fade scatter points
            user32.SendMessageW(child_hwnd, 0x0113, 1, 0)
            user32.SendMessageW(child_hwnd, 0x0113, 1, 0)
            user32.UpdateWindow(child_hwnd)
            pump_messages()

            # Test collapse button resizes window height down and expand button restores it
            rect_before = w.RECT()
            user32.GetClientRect(child_hwnd, c.byref(rect_before))
            assert rect_before.bottom == 600, f"Expected initial height 600, got {rect_before.bottom}"

            lparam_toggle = (38 << 16) | (465 & 0xFFFF)
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_toggle)  # WM_LBUTTONDOWN (collapse)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_toggle)  # WM_LBUTTONUP
            rect_collapsed = w.RECT()
            user32.GetClientRect(child_hwnd, c.byref(rect_collapsed))
            assert rect_collapsed.bottom == 195, f"Expected collapsed height 195, got {rect_collapsed.bottom}"
            user32.UpdateWindow(child_hwnd)
            pump_messages()

            # Click the collapse toggle button again to expand back to 600
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_toggle)  # WM_LBUTTONDOWN (expand)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_toggle)  # WM_LBUTTONUP
            rect_expanded = w.RECT()
            user32.GetClientRect(child_hwnd, c.byref(rect_expanded))
            assert rect_expanded.bottom == 600, f"Expected restored height 600, got {rect_expanded.bottom}"
            user32.UpdateWindow(child_hwnd)
            pump_messages()

            # Default slider mode is [-45 deg, +45 deg]: dragging to far right end caps at +45 deg (0.625)
            lparam_drag_start = (535 << 16) | (262 & 0xFFFF)
            lparam_drag_far_right = (535 << 16) | (500 & 0xFFFF)
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_drag_start)      # WM_LBUTTONDOWN
            user32.SendMessageW(child_hwnd, 0x0200, 1, lparam_drag_far_right)  # WM_MOUSEMOVE
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_drag_far_right)  # WM_LBUTTONUP
            drag_vals = [val for k, idx, val in edits if k == 0 and idx == 0]
            assert drag_vals and abs(drag_vals[-1] - 0.625) < 1e-6, (
                f"Expected +45 deg (0.625) at far right in default mode, got {drag_vals}"
            )

            # Click the extended range toggle button ("扩展 ±180°": logical x~125, y~395 -> pixel x=156, y=494)
            lparam_range_btn = (494 << 16) | (156 & 0xFFFF)
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_range_btn)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_range_btn)
            edits.clear()
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_drag_start)
            user32.SendMessageW(child_hwnd, 0x0200, 1, lparam_drag_far_right)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_drag_far_right)
            ext_drag_vals = [val for k, idx, val in edits if k == 0 and idx == 0]
            assert ext_drag_vals and abs(ext_drag_vals[-1] - 1.0) < 1e-6, (
                f"Expected +180 deg (1.0) at far right in extended mode, got {ext_drag_vals}"
            )
            for val in ext_drag_vals:
                deg = -180.0 + val * 360.0
                assert abs(deg - round(deg)) < 1e-9, f"Slider value {deg} not quantized to 1 deg"

            # Verify state persistence of collapsed & extendedRange flags when saved from live_inst
            # Collapse scope again so both collapsed (bit 0) and extendedRange (bit 1) are True (flags == 0x03)
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_toggle)  # WM_LBUTTONDOWN (collapse)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_toggle)  # WM_LBUTTONUP
            live_state = (c.c_uint8 * 6)()
            assert rack.save(live_inst, live_state, 6) == 6
            assert live_state[5] == 0x03, f"Expected saved state flags 0x03 (collapsed + extended), got {live_state[5]:#x}"

            # Expand back before finishing UI tests
            user32.SendMessageW(child_hwnd, 0x0201, 1, lparam_toggle)  # WM_LBUTTONDOWN (expand)
            user32.SendMessageW(child_hwnd, 0x0202, 0, lparam_toggle)  # WM_LBUTTONUP

            # Double-click on slider resets to 0.5 (0 deg) and updates textbox to "0"
            edits.clear()
            user32.SendMessageW(child_hwnd, 0x0203, 0, lparam_drag_start)  # WM_LBUTTONDBLCLK
            assert any(k == 0 and idx == 0 and abs(val - 0.5) < 1e-6 for k, idx, val in edits), f"Reset failed: {edits}"
            text_buf = c.create_unicode_buffer(32)
            user32.GetWindowTextW(edit_hwnd, text_buf, 32)
            assert text_buf.value == "0", f"Expected textbox '0' after reset, got '{text_buf.value}'"

            # Precise integer input via textbox + Enter key (VK_RETURN = 0x0D)
            edits.clear()
            user32.SetWindowTextW(edit_hwnd, "-37")
            user32.SendMessageW(edit_hwnd, 0x0100, 0x0D, 0)  # WM_KEYDOWN VK_RETURN
            expected_norm = (-37 + 180) / 360.0
            assert any(k == 0 and idx == 0 and abs(val - expected_norm) < 1e-9 for k, idx, val in edits), (
                f"Textbox input -37 failed: {edits}"
            )

            # Out-of-range integer input in textbox clamps to [-180, 180]
            edits.clear()
            user32.SetWindowTextW(edit_hwnd, "250")
            user32.SendMessageW(edit_hwnd, 0x0100, 0x0D, 0)  # WM_KEYDOWN VK_RETURN
            assert any(k == 0 and idx == 0 and abs(val - 1.0) < 1e-9 for k, idx, val in edits), (
                f"Textbox clamp 250 -> 180 failed: {edits}"
            )
            user32.GetWindowTextW(edit_hwnd, text_buf, 32)
            assert text_buf.value == "180", f"Expected clamped textbox '180', got '{text_buf.value}'"

            user32.UpdateWindow(child_hwnd)
            pump_messages()
        finally:
            editor.destroy(ed)
    finally:
        user32.DestroyWindow(parent_hwnd)
        rack.destroy(live_inst)

    print("PANN rack ABI, DSP rotation, 5ms smoothing, mono, telemetry, state, and GDI+ editor verified")


if __name__ == "__main__":
    main(sys.argv[1])
