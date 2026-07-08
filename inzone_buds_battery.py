from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import tempfile
import threading
import time
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


APP_NAME = "INZONE Buds Battery"
STARTUP_FILE_NAME = "INZONE Buds Battery.cmd"
DEFAULT_LOG_PATH = Path(os.environ.get("APPDATA", "")) / "Sony" / "INZONE Hub" / "ActionLog.log"
BATTERY_ITEMS = {
    "batteryStatusLeft": "left",
    "batteryStatusRight": "right",
    "batteryStatusCase": "case",
}


@dataclass
class BatteryValue:
    value: str = "Unknown"
    local_time: str = ""
    timestamp: int = 0

    @property
    def percent(self) -> Optional[int]:
        try:
            number = int(self.value)
        except (TypeError, ValueError):
            return None
        if 0 <= number <= 100:
            return number
        return None

    @property
    def label(self) -> str:
        if self.percent is not None:
            return f"{self.percent}%"
        if self.value == "Disconnect":
            return "Disconnect"
        return "Unknown"


@dataclass
class BatteryState:
    left: BatteryValue
    right: BatteryValue
    case: BatteryValue
    source: Path
    read_at: float
    error: str = ""

    @property
    def min_percent(self) -> Optional[int]:
        values = [part.percent for part in (self.left, self.right)]
        known = [value for value in values if value is not None]
        return min(known) if known else None

    @property
    def newest_local_time(self) -> str:
        values = [self.left, self.right, self.case]
        newest = max(values, key=lambda item: item.timestamp, default=BatteryValue())
        return newest.local_time

    def short_text(self) -> str:
        return f"L {self.left.label} / R {self.right.label} / Case {self.case.label}"

    def details(self) -> str:
        lines = [
            "INZONE Buds",
            f"Left : {self.left.label}",
            f"Right: {self.right.label}",
            f"Case : {self.case.label}",
        ]
        if self.newest_local_time:
            lines.append(f"Last : {self.newest_local_time}")
        if self.error:
            lines.append(f"Note : {self.error}")
        return "\n".join(lines)


@dataclass(frozen=True)
class LogSignature:
    size: int
    mtime_ns: int


@dataclass
class BatteryCache:
    state: BatteryState
    signature: Optional[LogSignature] = None


def _log_signature(path: Path) -> Optional[LogSignature]:
    try:
        stat = path.stat()
    except OSError:
        return None
    return LogSignature(size=stat.st_size, mtime_ns=stat.st_mtime_ns)


def _read_tail(path: Path, max_bytes: int = 2_000_000) -> str:
    size = path.stat().st_size
    with path.open("rb") as file:
        if size > max_bytes:
            file.seek(size - max_bytes)
            file.readline()
        data = file.read()
    return data.decode("utf-8", errors="replace")


def read_battery_state(path: Path = DEFAULT_LOG_PATH) -> BatteryState:
    state = {
        "left": BatteryValue(),
        "right": BatteryValue(),
        "case": BatteryValue(),
    }
    if not path.exists():
        return BatteryState(
            left=state["left"],
            right=state["right"],
            case=state["case"],
            source=path,
            read_at=time.time(),
            error=f"Log file not found: {path}",
        )

    try:
        text = _read_tail(path)
    except OSError as exc:
        return BatteryState(
            left=state["left"],
            right=state["right"],
            case=state["case"],
            source=path,
            read_at=time.time(),
            error=str(exc),
        )

    for line in text.splitlines():
        if "batteryStatus" not in line:
            continue
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue

        action = entry.get("action") or {}
        key = BATTERY_ITEMS.get(action.get("item"))
        if key is None:
            continue

        timestamp = int(entry.get("timeStamp") or 0)
        if timestamp >= state[key].timestamp:
            state[key] = BatteryValue(
                value=str(action.get("value", "Unknown")),
                local_time=str(entry.get("localTime", "")),
                timestamp=timestamp,
            )

    return BatteryState(
        left=state["left"],
        right=state["right"],
        case=state["case"],
        source=path,
        read_at=time.time(),
    )


def read_battery_state_cached(path: Path, cache: Optional[BatteryCache]) -> BatteryCache:
    signature = _log_signature(path)
    if cache is not None and signature is not None and signature == cache.signature:
        return cache

    state = read_battery_state(path)
    return BatteryCache(state=state, signature=signature)


def print_once(path: Path) -> int:
    state = read_battery_state(path)
    print(state.short_text())
    if state.newest_local_time:
        print(f"Last update: {state.newest_local_time}")
    if state.error:
        print(state.error, file=sys.stderr)
        return 1
    return 0


def watch_console(path: Path, interval: int) -> int:
    previous = ""
    try:
        while True:
            state = read_battery_state(path)
            text = f"{state.short_text()}  ({state.newest_local_time or 'no timestamp'})"
            if text != previous:
                print(text)
                previous = text
            time.sleep(interval)
    except KeyboardInterrupt:
        return 0


def run_tray(path: Path, interval: int, show_on_start: bool = False) -> int:
    if os.name != "nt":
        print("Tray mode is only supported on Windows.", file=sys.stderr)
        return 1
    _configure_win32_api()
    mutex = ctypes.windll.kernel32.CreateMutexW(None, False, "Local\\INZONEBudsBatteryTray")
    if ctypes.windll.kernel32.GetLastError() == 183:
        ctypes.windll.user32.MessageBoxW(
            None,
            "INZONE Buds Battery is already running.\n\nCheck the notification area or the hidden icons menu (^).",
            APP_NAME,
            0,
        )
        return 0
    return _WindowsTrayApp(path, interval, show_on_start).run()


def startup_file_path() -> Path:
    return (
        Path(os.environ.get("APPDATA", ""))
        / "Microsoft"
        / "Windows"
        / "Start Menu"
        / "Programs"
        / "Startup"
        / STARTUP_FILE_NAME
    )


def startup_command() -> str:
    if getattr(sys, "frozen", False):
        return f'start "" "{Path(sys.executable)}" --tray'
    return f'start "" "{Path(sys.executable)}" "{Path(__file__).resolve()}" --tray'


def is_startup_enabled() -> bool:
    return startup_file_path().exists()


def set_startup_enabled(enabled: bool) -> None:
    path = startup_file_path()
    if enabled:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"@echo off\n{startup_command()}\n", encoding="utf-8")
    elif path.exists():
        path.unlink()


def _configure_win32_api() -> None:
    user32 = ctypes.windll.user32
    shell32 = ctypes.windll.shell32
    kernel32 = ctypes.windll.kernel32

    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.GetLastError.restype = wintypes.DWORD

    user32.CreateWindowExW.restype = wintypes.HWND
    user32.CreateWindowExW.argtypes = [
        wintypes.DWORD,
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.HWND,
        wintypes.HANDLE,
        wintypes.HINSTANCE,
        ctypes.c_void_p,
    ]
    user32.CreatePopupMenu.restype = wintypes.HANDLE
    user32.AppendMenuW.restype = wintypes.BOOL
    user32.AppendMenuW.argtypes = [wintypes.HANDLE, wintypes.UINT, ctypes.c_size_t, wintypes.LPCWSTR]
    user32.InsertMenuItemW.restype = wintypes.BOOL
    user32.InsertMenuItemW.argtypes = [wintypes.HANDLE, wintypes.UINT, wintypes.BOOL, ctypes.c_void_p]
    user32.TrackPopupMenu.restype = ctypes.c_int
    user32.TrackPopupMenu.argtypes = [
        wintypes.HANDLE,
        wintypes.UINT,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.HWND,
        ctypes.c_void_p,
    ]
    user32.DestroyMenu.argtypes = [wintypes.HANDLE]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.LoadImageW.restype = wintypes.HANDLE
    user32.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT, ctypes.c_int, ctypes.c_int, wintypes.UINT]
    user32.LoadIconW.restype = wintypes.HANDLE
    user32.LoadIconW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]
    shell32.Shell_NotifyIconW.restype = wintypes.BOOL


class _WindowsTrayApp:
    WM_DESTROY = 0x0002
    WM_COMMAND = 0x0111
    WM_TIMER = 0x0113
    WM_APP_TRAY = 0x8000 + 1
    WM_LBUTTONUP = 0x0202
    WM_RBUTTONUP = 0x0205

    NIM_ADD = 0x00000000
    NIM_MODIFY = 0x00000001
    NIM_DELETE = 0x00000002
    NIF_MESSAGE = 0x00000001
    NIF_ICON = 0x00000002
    NIF_TIP = 0x00000004

    TPM_RIGHTBUTTON = 0x0002
    TPM_NONOTIFY = 0x0080
    TPM_RETURNCMD = 0x0100

    MF_STRING = 0x0000
    MF_SEPARATOR = 0x0800
    MF_CHECKED = 0x0008

    ID_SHOW = 1001
    ID_REFRESH = 1002
    ID_STARTUP = 1003
    ID_EXIT = 1004

    def __init__(self, path: Path, interval: int, show_on_start: bool = False) -> None:
        self.path = path
        self.interval_ms = max(3, interval) * 1000
        self.hwnd = None
        self.hicon = None
        self._wnd_proc = None
        self.show_on_start = show_on_start
        self.cache = read_battery_state_cached(path, None)
        self.state = self.cache.state
        self.last_display_text = ""
        self.class_name = "INZONEBudsBatteryTrayWindow"

    def run(self) -> int:
        self._create_window()
        self._update_icon(add=True)
        ctypes.windll.user32.SetTimer(self.hwnd, 1, self.interval_ms, None)
        if self.show_on_start:
            self._show_details()

        msg = wintypes.MSG()
        while ctypes.windll.user32.GetMessageW(ctypes.byref(msg), None, 0, 0) != 0:
            ctypes.windll.user32.TranslateMessage(ctypes.byref(msg))
            ctypes.windll.user32.DispatchMessageW(ctypes.byref(msg))
        return 0

    def _create_window(self) -> None:
        hinst = ctypes.windll.kernel32.GetModuleHandleW(None)
        wnd_proc_type = ctypes.WINFUNCTYPE(
            wintypes.LPARAM, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
        )
        self._wnd_proc = wnd_proc_type(self._window_proc)

        class WNDCLASS(ctypes.Structure):
            _fields_ = [
                ("style", wintypes.UINT),
                ("lpfnWndProc", wnd_proc_type),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wintypes.HINSTANCE),
                ("hIcon", wintypes.HANDLE),
                ("hCursor", wintypes.HANDLE),
                ("hbrBackground", wintypes.HANDLE),
                ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR),
            ]

        wndclass = WNDCLASS()
        wndclass.lpfnWndProc = self._wnd_proc
        wndclass.hInstance = hinst
        wndclass.lpszClassName = self.class_name
        ctypes.windll.user32.RegisterClassW(ctypes.byref(wndclass))

        self.hwnd = ctypes.windll.user32.CreateWindowExW(
            0, self.class_name, APP_NAME, 0, 0, 0, 0, 0, None, None, hinst, None
        )
        if not self.hwnd:
            raise ctypes.WinError()

    def _window_proc(self, hwnd, msg, wparam, lparam):
        if msg == self.WM_TIMER:
            self._refresh()
            return 0
        if msg == self.WM_COMMAND:
            command = wparam & 0xFFFF
            if command == self.ID_SHOW:
                self._show_details()
            elif command == self.ID_REFRESH:
                self._refresh(show_details=True)
            elif command == self.ID_STARTUP:
                self._toggle_startup()
            elif command == self.ID_EXIT:
                ctypes.windll.user32.DestroyWindow(self.hwnd)
            return 0
        if msg == self.WM_APP_TRAY:
            if lparam == self.WM_LBUTTONUP:
                self._show_details()
            elif lparam == self.WM_RBUTTONUP:
                self._show_menu()
            return 0
        if msg == self.WM_DESTROY:
            ctypes.windll.shell32.Shell_NotifyIconW(self.NIM_DELETE, ctypes.byref(self._notify_data()))
            ctypes.windll.user32.KillTimer(hwnd, 1)
            if self.hicon:
                ctypes.windll.user32.DestroyIcon(self.hicon)
            ctypes.windll.user32.PostQuitMessage(0)
            return 0
        return ctypes.windll.user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _refresh(self, show_details: bool = False) -> None:
        previous_text = self.state.short_text()
        self.cache = read_battery_state_cached(self.path, self.cache)
        self.state = self.cache.state
        if self.state.short_text() != previous_text:
            self._update_icon(add=False)
        if show_details:
            self._show_details()

    def _show_details(self) -> None:
        ctypes.windll.user32.MessageBoxW(self.hwnd, self.state.details(), APP_NAME, 0)

    def _toggle_startup(self) -> None:
        enabled = not is_startup_enabled()
        try:
            set_startup_enabled(enabled)
            status = "ON" if enabled else "OFF"
            ctypes.windll.user32.MessageBoxW(self.hwnd, f"スタートアップを{status}にしました。", APP_NAME, 0)
        except OSError as exc:
            ctypes.windll.user32.MessageBoxW(self.hwnd, f"スタートアップ設定に失敗しました。\n\n{exc}", APP_NAME, 0)

    def _show_menu(self) -> None:
        point = wintypes.POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(point))
        threading.Thread(target=self._show_tk_menu, args=(point.x, point.y), daemon=True).start()

    def _show_tk_menu(self, x: int, y: int) -> None:
        try:
            import tkinter as tk
        except Exception as exc:
            ctypes.windll.user32.MessageBoxW(self.hwnd, f"メニュー表示に失敗しました。\n\n{exc}", APP_NAME, 0)
            return

        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        menu = tk.Menu(root, tearoff=False)
        menu.add_command(label="バッテリーを表示", command=lambda: self._post_command(self.ID_SHOW, root))
        menu.add_command(label="表示を更新", command=lambda: self._post_command(self.ID_REFRESH, root))
        startup_label = "スタートアップ起動: ON" if is_startup_enabled() else "スタートアップ起動: OFF"
        menu.add_command(label=startup_label, command=lambda: self._post_command(self.ID_STARTUP, root))
        menu.add_separator()
        menu.add_command(label="終了", command=lambda: self._post_command(self.ID_EXIT, root))
        try:
            menu.tk_popup(x, y)
        finally:
            menu.grab_release()
            root.after(10, root.destroy)
            root.mainloop()

    def _post_command(self, command: int, root) -> None:
        try:
            root.destroy()
        except Exception:
            pass
        ctypes.windll.user32.PostMessageW(self.hwnd, self.WM_COMMAND, command, 0)

    def _update_icon(self, add: bool) -> None:
        display_text = self.state.short_text()
        if not add and display_text == self.last_display_text:
            return
        old_icon = self.hicon
        self.hicon = self._make_icon()
        command = self.NIM_ADD if add else self.NIM_MODIFY
        ctypes.windll.shell32.Shell_NotifyIconW(command, ctypes.byref(self._notify_data()))
        self.last_display_text = display_text
        if old_icon:
            ctypes.windll.user32.DestroyIcon(old_icon)

    def _notify_data(self):
        class NOTIFYICONDATA(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD),
                ("hWnd", wintypes.HWND),
                ("uID", wintypes.UINT),
                ("uFlags", wintypes.UINT),
                ("uCallbackMessage", wintypes.UINT),
                ("hIcon", wintypes.HANDLE),
                ("szTip", wintypes.WCHAR * 128),
            ]

        data = NOTIFYICONDATA()
        data.cbSize = ctypes.sizeof(NOTIFYICONDATA)
        data.hWnd = self.hwnd
        data.uID = 1
        data.uFlags = self.NIF_MESSAGE | self.NIF_ICON | self.NIF_TIP
        data.uCallbackMessage = self.WM_APP_TRAY
        data.hIcon = self.hicon
        data.szTip = f"{APP_NAME}: {self.state.short_text()}"[:127]
        return data

    def _make_icon(self):
        icon_path = Path(tempfile.gettempdir()) / "inzone_buds_battery.ico"
        percent = self.state.min_percent
        text = "--" if percent is None else str(percent)
        color = _battery_color(percent)

        try:
            from PIL import Image, ImageDraw, ImageFont

            image = Image.new("RGBA", (64, 64), color)
            draw = ImageDraw.Draw(image)
            font = _load_font(28 if len(text) <= 2 else 23)
            bbox = draw.textbbox((0, 0), text, font=font)
            x = (64 - (bbox[2] - bbox[0])) // 2
            y = (64 - (bbox[3] - bbox[1])) // 2 - 2
            draw.rounded_rectangle((2, 2, 62, 62), radius=12, outline=(255, 255, 255, 230), width=3)
            draw.text((x, y), text, font=font, fill=(255, 255, 255, 255))
            image.save(icon_path)
            return ctypes.windll.user32.LoadImageW(None, str(icon_path), 1, 0, 0, 0x0010)
        except Exception:
            return ctypes.windll.user32.LoadIconW(None, ctypes.c_void_p(32512))


def _load_font(size: int):
    from PIL import ImageFont

    for font_name in ("segoeuib.ttf", "arialbd.ttf", "arial.ttf"):
        font_path = Path(os.environ.get("WINDIR", "C:\\Windows")) / "Fonts" / font_name
        if font_path.exists():
            return ImageFont.truetype(str(font_path), size)
    return ImageFont.load_default()


def _battery_color(percent: Optional[int]) -> tuple[int, int, int, int]:
    if percent is None:
        return (82, 91, 102, 255)
    if percent <= 20:
        return (206, 67, 67, 255)
    if percent <= 50:
        return (214, 142, 39, 255)
    return (43, 147, 96, 255)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Show INZONE Buds battery percentage from INZONE Hub logs.")
    parser.add_argument("--path", type=Path, default=DEFAULT_LOG_PATH, help="Path to INZONE Hub ActionLog.log")
    parser.add_argument("--interval", type=int, default=15, help="Refresh interval in seconds")
    parser.add_argument("--once", action="store_true", help="Print current battery status once")
    parser.add_argument("--watch", action="store_true", help="Print updates in the console")
    parser.add_argument("--tray", action="store_true", help="Run as a Windows notification-area icon")
    parser.add_argument("--show-on-start", action="store_true", help="Show a startup confirmation dialog in tray mode")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.once:
        return print_once(args.path)
    if args.watch:
        return watch_console(args.path, args.interval)
    return run_tray(args.path, args.interval, args.show_on_start)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        error_path = Path(__file__).with_name("inzone_buds_battery_error.log")
        error_path.write_text(f"{type(exc).__name__}: {exc}\n", encoding="utf-8")
        if os.name == "nt":
            ctypes.windll.user32.MessageBoxW(None, f"{type(exc).__name__}: {exc}", APP_NAME, 0)
        raise
