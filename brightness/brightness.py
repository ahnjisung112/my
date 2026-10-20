# -*- coding: utf-8 -*-
"""
모니터 밝기 조절기 (Windows)

- 50~100% : 모니터 자체 밝기(하드웨어, DDC/CI 또는 노트북 WMI)를 0~100으로 조절
- 0~50%   : 하드웨어 밝기를 0으로 두고, 화면 전체에 클릭이 통과하는 검은 오버레이를 덮어
            더 어둡게 만듦. 0%에서는 완전한 검정(꺼진 것처럼 보임)
- 프로그램 창 위에서 마우스 휠로 조절 (Shift = 미세, Ctrl = 크게)
- 전역 단축키: Ctrl+Alt+PageUp / PageDown (밝게/어둡게), Ctrl+Alt+Home (100%)

외부 라이브러리 없이 Python 표준 라이브러리(tkinter, ctypes)만 사용합니다.
"""

import ctypes
import queue
import subprocess
import sys
import threading
import tkinter as tk
from ctypes import wintypes

if sys.platform != "win32":
    sys.exit("이 프로그램은 Windows 전용입니다.")

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# 고해상도(배율) 환경에서 좌표가 실제 픽셀과 일치하도록 DPI 인식 설정
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass

# ---------------------------------------------------------------- Win32 상수
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOPMOST = 0x00000008
LWA_ALPHA = 0x2
HWND_TOPMOST = wintypes.HWND(-1)
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x1, 0x2, 0x10
SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN = 76, 77
SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN = 78, 79
WM_HOTKEY, WM_QUIT = 0x0312, 0x0012
MOD_ALT, MOD_CONTROL = 0x1, 0x2
VK_PRIOR, VK_NEXT, VK_HOME = 0x21, 0x22, 0x24
CREATE_NO_WINDOW = 0x08000000

user32.GetWindowLongW.restype = ctypes.c_long
user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
user32.SetWindowLongW.restype = ctypes.c_long
user32.SetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_long]
user32.SetLayeredWindowAttributes.argtypes = [wintypes.HWND, wintypes.COLORREF, wintypes.BYTE, wintypes.DWORD]
user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wintypes.UINT]
user32.GetParent.restype = wintypes.HWND
user32.GetParent.argtypes = [wintypes.HWND]
user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, ctypes.c_void_p, wintypes.LPARAM]

# ---------------------------------------------------------------- 밝기 곡선
STEP = 2  # 휠 한 칸당 변화량(%)


def overlay_transmission(level, split):
    """0..split 구간의 레벨을 오버레이 투과량(0~255)으로 변환.
    사람 눈은 어두운 영역에 민감하므로 감마 곡선을 써서 바닥 근처를 촘촘하게 나눈다.
    level 0 -> 0 (완전 검정), level 1 -> 1/255 (거의 꺼짐)."""
    if level >= split:
        return 255
    if level <= 0:
        return 0
    t = max(0.0, (level - 1) / (split - 1))
    return max(1, min(255, round(1 + 254 * t ** 2.2)))


# ---------------------------------------------------------------- 하드웨어 밝기
class PHYSICAL_MONITOR(ctypes.Structure):
    _fields_ = [("hPhysicalMonitor", wintypes.HANDLE),
                ("szPhysicalMonitorDescription", ctypes.c_wchar * 128)]


MONITORENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                                     ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)


class DdcBackend:
    """외장 모니터: DDC/CI (dxva2.dll)"""

    def __init__(self):
        self.dxva2 = d = ctypes.WinDLL("dxva2")
        d.GetNumberOfPhysicalMonitorsFromHMONITOR.argtypes = [wintypes.HMONITOR, ctypes.POINTER(wintypes.DWORD)]
        d.GetPhysicalMonitorsFromHMONITOR.argtypes = [wintypes.HMONITOR, wintypes.DWORD,
                                                      ctypes.POINTER(PHYSICAL_MONITOR)]
        d.GetMonitorBrightness.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.DWORD)] * 3
        d.SetMonitorBrightness.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        d.DestroyPhysicalMonitor.argtypes = [wintypes.HANDLE]
        self.monitors = []  # (handle, min, max)
        hmons = []

        def cb(hmon, hdc, rect, lparam):
            hmons.append(hmon)
            return True

        proc = MONITORENUMPROC(cb)
        user32.EnumDisplayMonitors(None, None, proc, 0)
        for hmon in hmons:
            n = wintypes.DWORD()
            if not self.dxva2.GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, ctypes.byref(n)) or not n.value:
                continue
            arr = (PHYSICAL_MONITOR * n.value)()
            if not self.dxva2.GetPhysicalMonitorsFromHMONITOR(hmon, n.value, arr):
                continue
            for pm in arr:
                mn, cur, mx = wintypes.DWORD(), wintypes.DWORD(), wintypes.DWORD()
                if self.dxva2.GetMonitorBrightness(pm.hPhysicalMonitor, ctypes.byref(mn),
                                                   ctypes.byref(cur), ctypes.byref(mx)) and mx.value > mn.value:
                    self.monitors.append((pm.hPhysicalMonitor, mn.value, mx.value, cur.value))
                else:
                    self.dxva2.DestroyPhysicalMonitor(pm.hPhysicalMonitor)

    def available(self):
        return bool(self.monitors)

    def get(self):
        h, mn, mx, cur = self.monitors[0]
        return round((cur - mn) * 100 / (mx - mn))

    def set(self, pct):
        for h, mn, mx, _ in self.monitors:
            self.dxva2.SetMonitorBrightness(h, wintypes.DWORD(round(mn + (mx - mn) * pct / 100)))

    def close(self):
        for h, *_ in self.monitors:
            self.dxva2.DestroyPhysicalMonitor(h)
        self.monitors = []


class WmiBackend:
    """노트북 내장 화면: WMI (PowerShell 경유)"""

    def __init__(self):
        self.ok = False
        self.cur = 100
        out = self._ps("(Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness "
                       "-ErrorAction Stop | Select-Object -First 1).CurrentBrightness")
        try:
            self.cur = int(out.strip().splitlines()[0])
            self.ok = True
        except Exception:
            pass

    @staticmethod
    def _ps(cmd):
        try:
            return subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd],
                                  capture_output=True, text=True, timeout=10,
                                  creationflags=CREATE_NO_WINDOW).stdout
        except Exception:
            return ""

    def available(self):
        return self.ok

    def get(self):
        return self.cur

    def set(self, pct):
        self._ps("Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightnessMethods | "
                 f"Invoke-CimMethod -MethodName WmiSetBrightness -Arguments @{{Timeout=1;Brightness={int(pct)}}}")

    def close(self):
        pass


class HardwareWorker(threading.Thread):
    """DDC/WMI 호출은 느리므로(수십~수백 ms) 별도 스레드에서 '마지막 요청값'만 적용한다."""

    def __init__(self, events):
        super().__init__(daemon=True)
        self.events = events
        self.cond = threading.Condition()
        self.target = None
        self.stop = False
        self.backends = []

    def request(self, pct):
        with self.cond:
            self.target = pct
            self.cond.notify()

    def shutdown(self, final_pct=None):
        with self.cond:
            if final_pct is not None:
                self.target = final_pct
            self.stop = True
            self.cond.notify()

    def run(self):
        for cls in (DdcBackend, WmiBackend):
            try:
                b = cls()
                if b.available():
                    self.backends.append(b)
            except Exception:
                pass
        initial = self.backends[0].get() if self.backends else None
        self.events.put(("hw_ready", initial))

        last = initial
        while True:
            with self.cond:
                while self.target is None and not self.stop:
                    self.cond.wait()
                pct, self.target = self.target, None
                stopping = self.stop
            if pct is not None and pct != last:
                for b in self.backends:
                    try:
                        b.set(pct)
                    except Exception:
                        pass
                last = pct
            if stopping:
                break
        for b in self.backends:
            b.close()


class HotkeyThread(threading.Thread):
    HOTKEYS = {1: VK_PRIOR, 2: VK_NEXT, 3: VK_HOME}

    def __init__(self, events):
        super().__init__(daemon=True)
        self.events = events
        self.tid = None

    def run(self):
        self.tid = kernel32.GetCurrentThreadId()
        for hid, vk in self.HOTKEYS.items():
            user32.RegisterHotKey(None, hid, MOD_CONTROL | MOD_ALT, vk)
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            if msg.message == WM_HOTKEY:
                self.events.put(("hotkey", msg.wParam))
        for hid in self.HOTKEYS:
            user32.UnregisterHotKey(None, hid)

    def quit(self):
        if self.tid:
            user32.PostThreadMessageW(self.tid, WM_QUIT, 0, 0)


# ---------------------------------------------------------------- 화면 오버레이
def hwnd_of(widget):
    widget.update_idletasks()
    return user32.GetParent(widget.winfo_id()) or widget.winfo_id()


def virtual_screen():
    g = user32.GetSystemMetrics
    return (g(SM_XVIRTUALSCREEN), g(SM_YVIRTUALSCREEN),
            g(SM_CXVIRTUALSCREEN), g(SM_CYVIRTUALSCREEN))


class Overlay:
    """모든 모니터를 덮는 검은 창. 마우스 클릭/키보드는 그대로 아래 창으로 통과한다."""

    def __init__(self, root):
        self.win = tk.Toplevel(root)
        self.win.overrideredirect(True)
        self.win.configure(bg="black")
        self.geom = None
        self.fit()
        self.hwnd = hwnd_of(self.win)
        self._apply_style()
        self.transmission = None
        self.set(255)

    def _apply_style(self):
        # 레이어드(반투명) + 클릭 통과 + 작업표시줄/Alt+Tab 제외 + 포커스 안 뺏음
        ex = user32.GetWindowLongW(self.hwnd, GWL_EXSTYLE)
        user32.SetWindowLongW(self.hwnd, GWL_EXSTYLE,
                              ex | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
                              | WS_EX_NOACTIVATE | WS_EX_TOPMOST)

    def fit(self):
        x, y, w, h = virtual_screen()
        geom = f"{w}x{h}{x:+d}{y:+d}"
        if geom != self.geom:
            self.win.geometry(geom)
            self.geom = geom

    def set(self, transmission):
        if transmission == self.transmission:
            return
        self.transmission = transmission
        if transmission >= 255:
            self.win.withdraw()
            return
        if self.win.state() == "withdrawn":
            self.win.deiconify()
            self._apply_style()
            self.raise_top()
        user32.SetLayeredWindowAttributes(self.hwnd, 0, 255 - transmission, LWA_ALPHA)

    def raise_top(self):
        user32.SetWindowPos(self.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)


# ---------------------------------------------------------------- 메인 창
BG, FG, SUB, ACCENT, TRACK = "#1b1d22", "#f2f3f5", "#8a8f99", "#ffcc4d", "#33363d"


class App:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("밝기 조절")
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)
        self.root.configure(bg=BG)

        self.events = queue.Queue()
        self.level = 100
        self.split = 50          # 하드웨어 밝기 없으면 100으로 바뀌어 전 구간 오버레이
        self.hw_original = None
        self.hw_ready = False
        self.touched = False
        self.hover = False

        self.overlay = Overlay(self.root)
        self._build_ui()

        self.hw = HardwareWorker(self.events)
        self.hw.start()
        self.hotkeys = HotkeyThread(self.events)
        self.hotkeys.start()

        self.root.update_idletasks()
        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        w, h = self.root.winfo_reqwidth(), self.root.winfo_reqheight()
        self.root.geometry(f"+{sw - w - 40}+{sh - h - 90}")
        self.root_hwnd = hwnd_of(self.root)

        self.apply()
        self.root.after(50, self._poll)
        self.root.after(1000, self._keep_on_top)

    # ---------- UI
    def _build_ui(self):
        r = self.root
        frame = tk.Frame(r, bg=BG, padx=16, pady=12, highlightthickness=1, highlightbackground="#2c2f36")
        frame.pack(fill="both", expand=True)

        top = tk.Frame(frame, bg=BG)
        top.pack(fill="x")
        self.title_lbl = tk.Label(top, text="☀ 밝기", bg=BG, fg=SUB, font=("Malgun Gothic", 10))
        self.title_lbl.pack(side="left")
        close = tk.Label(top, text="✕", bg=BG, fg=SUB, font=("Malgun Gothic", 11), cursor="hand2")
        close.pack(side="right")
        close.bind("<Button-1>", lambda e: self.quit())

        self.value_lbl = tk.Label(frame, text="100%", bg=BG, fg=FG, font=("Malgun Gothic", 30, "bold"))
        self.value_lbl.pack(pady=(2, 4))

        self.bar = tk.Canvas(frame, width=240, height=14, bg=BG, highlightthickness=0, cursor="hand2")
        self.bar.pack()
        self.bar.bind("<Button-1>", self._bar_click)
        self.bar.bind("<B1-Motion>", self._bar_click)

        self.mode_lbl = tk.Label(frame, text="", bg=BG, fg=SUB, font=("Malgun Gothic", 9))
        self.mode_lbl.pack(pady=(6, 0))
        tk.Label(frame, text="휠: 조절 · Shift 미세 · Ctrl 크게 · 우클릭 메뉴",
                 bg=BG, fg="#5d626c", font=("Malgun Gothic", 8)).pack()

        self.menu = tk.Menu(r, tearoff=0)
        for v in (100, 75, 50, 25, 10, 0):
            self.menu.add_command(label=f"{v}%", command=lambda v=v: self.set_level(v))
        self.menu.add_separator()
        self.menu.add_command(label="종료", command=self.quit)

        r.bind_all("<MouseWheel>", self._wheel)
        r.bind_all("<Button-3>", lambda e: self.menu.tk_popup(e.x_root, e.y_root))
        r.bind_all("<Up>", lambda e: self.step(STEP))
        r.bind_all("<Down>", lambda e: self.step(-STEP))
        r.bind_all("<Escape>", lambda e: self.quit())

        # 창 드래그 이동
        for w in (frame, top, self.title_lbl, self.value_lbl, self.mode_lbl):
            w.bind("<ButtonPress-1>", self._drag_start)
            w.bind("<B1-Motion>", self._drag_move)
        r.bind("<Enter>", lambda e: self._set_hover(True))
        r.bind("<Leave>", lambda e: self._set_hover(False))

    def _drag_start(self, e):
        self._dx, self._dy = e.x_root - self.root.winfo_x(), e.y_root - self.root.winfo_y()

    def _drag_move(self, e):
        self.root.geometry(f"+{e.x_root - self._dx}+{e.y_root - self._dy}")

    def _set_hover(self, on):
        self.hover = on
        self._update_window_alpha()

    def _bar_click(self, e):
        w = int(self.bar["width"])
        self.set_level(round(max(0, min(1, e.x / w)) * 100))

    def _wheel(self, e):
        notches = e.delta / 120
        mult = 0.5 if e.state & 0x1 else (5 if e.state & 0x4 else 1)   # Shift / Ctrl
        self.step(notches * STEP * mult)

    # ---------- 밝기 로직
    def step(self, delta):
        self.set_level(self.level + delta)

    def set_level(self, value):
        self.touched = True
        self.level = max(0.0, min(100.0, round(value * 2) / 2))
        self.apply()

    def hw_percent(self):
        if self.split >= 100:
            return None
        return round(max(0, self.level - self.split) * 100 / (100 - self.split))

    def apply(self):
        self.overlay.set(overlay_transmission(self.level, self.split))
        hw = self.hw_percent()
        if hw is not None and self.hw_ready:
            self.hw.request(hw)
        self._redraw()

    def _redraw(self):
        lv = self.level
        self.value_lbl.config(text=(f"{lv:g}%" if lv < 10 else f"{round(lv)}%"))
        c, w, h = self.bar, int(self.bar["width"]), int(self.bar["height"])
        c.delete("all")
        c.create_rectangle(0, 4, w, h - 4, fill=TRACK, width=0)
        if self.split < 100:
            sx = w * self.split / 100
            c.create_line(sx, 0, sx, h, fill="#555a63")
        c.create_rectangle(0, 4, w * lv / 100, h - 4, fill=ACCENT, width=0)
        if lv <= 0:
            mode = "완전 소등 (화면 꺼짐 상태)"
        elif lv < self.split:
            mode = "모니터 최저 밝기 + 화면 어둡게" if self.split < 100 else "화면 어둡게 (오버레이)"
        else:
            mode = f"모니터 밝기 {self.hw_percent()}%"
        if not self.hw_ready:
            mode += " · 모니터 확인 중…"
        self.mode_lbl.config(text=mode)
        self._update_window_alpha()

    def _update_window_alpha(self):
        # 화면이 어두울 때 조절 창만 눈부시지 않도록 같이 흐리게 (마우스를 올리면 선명)
        if self.hover:
            a = 1.0
        else:
            a = 0.35 + 0.65 * min(1.0, self.level / max(1, self.split))
        self.root.attributes("-alpha", a)

    # ---------- 이벤트 루프
    def _poll(self):
        try:
            while True:
                kind, val = self.events.get_nowait()
                if kind == "hw_ready":
                    self.hw_ready = True
                    self.hw_original = val
                    if val is None:
                        self.split = 100       # 하드웨어 제어 불가 -> 전 구간 오버레이
                    elif not self.touched:
                        self.level = self.split + val * (100 - self.split) / 100
                    self.apply()
                elif kind == "hotkey":
                    if val == 1:
                        self.step(STEP)
                    elif val == 2:
                        self.step(-STEP)
                    elif val == 3:
                        self.set_level(100)
        except queue.Empty:
            pass
        self.root.after(50, self._poll)

    def _keep_on_top(self):
        # 작업표시줄 등 다른 최상위 창이 덮어도 오버레이 -> 조절 창 순서로 다시 위로
        self.overlay.fit()
        if self.overlay.transmission < 255:
            self.overlay.raise_top()
        user32.SetWindowPos(self.root_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        self.root.after(1000, self._keep_on_top)

    def quit(self):
        # 오버레이 구간(하드웨어 0%)에서 종료하면 원래 모니터 밝기로 되돌림
        final = None
        if self.hw_original is not None and self.level < self.split:
            final = self.hw_original
        self.hw.shutdown(final)
        self.hotkeys.quit()
        self.overlay.set(255)
        self.root.destroy()
        self.hw.join(timeout=5)

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    App().run()
