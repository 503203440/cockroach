# AGENTS.md - Cockroach Desktop Prank Project Guide for AI Agents

Welcome, AI Coding Agent! This document provides technical architecture details, build processes, state machine specifications, graphics pipeline guidelines, and coding practices for working on the **Desktop Photorealistic Cockroach** codebase.

---

## 🪳 Project Overview

This project is a Windows-native, single-executable desktop prank program built in C++ and GDI+. It renders a photorealistic cockroach with procedural biomechanical gait animations, forward-kinematic whip antennae, and realistic crawling/sniffing/fleeing AI on a topmost layered transparent window.

### Key Metrics & Stack
- **Language**: C++11 (MSVC Compiler)
- **Graphics API**: Win32 API + GDI+ (Gdiplus)
- **Distribution Format**: Single portable EXE (~274 KB), static resource embedding (RCDATA)
- **Target OS**: Windows 10 / 11 (Per-Monitor V2 DPI Aware)

---

## 📁 Repository Structure

```
cockroach/
├── main.cpp          # Core application code: Win32 entry, GDI+ rendering, state machine, kinematics
├── resource.rc       # Win32 resource script (embeds cockroach.png into EXE as IDR_ROACH_PNG)
├── resource.h        # Resource header definitions (IDR_ROACH_PNG = 101)
├── cockroach.png      # 32-bit ARGB photorealistic cockroach torso image
├── cockroach.ini      # Idle timeout configuration file (idle_seconds = 30)
├── build.bat         # MSVC batch build script
├── .gitignore        # Git ignore file
├── README.md         # User-facing documentation
└── AGENTS.md         # Guidelines and architecture docs for AI Agents (this file)
```

---

## 🛠️ Build & Process Management

### 1. Build Environment
- Toolchain: MSVC x64 (`cl.exe`, `rc.exe`, `vcvars64.bat`)
- Build Script: Run `build.bat` from project root directory or via terminal:
  ```cmd
  cmd /c build.bat
  ```

### 2. CRITICAL: Process Lock Handling (LNK1104 Error Prevention)
Because `cockroach.exe` is a background desktop app (running quietly in system tray), **the binary file may be locked by Windows while running**.
If `build.bat` fails with `fatal error LNK1104: cannot open file cockroach.exe`, you **MUST** terminate the running instance before rebuilding:
```cmd
taskkill /f /im cockroach.exe
```
Then rerun `build.bat`.

### 3. Compiler & Linker Flags
```cmd
rc /nologo resource.rc
cl /nologo /O1 /MT /EHsc /GL /DNDEBUG /utf-8 main.cpp resource.res /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib /OUT:cockroach.exe
```

---

## 🏗️ Architecture & Subsystems

### 1. Windowing & Layered Graphics Pipeline
- **Transparent Overlay Window**:
  - Class name: `RoachWindow`
  - Style: `WS_POPUP`
  - Extended Style: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`
- **Render Engine**:
  - Double-buffered 32-bit PARGB DIB Section (`g_hDib`, `g_memDC`, `g_bmp`, `g_gfx`).
  - Frame updates driven by Win32 timer (`TIMER_MS = 20` ~ 50 FPS) using high-resolution performance counter (`QueryPerformanceCounter`).
  - Screen blit performed via `UpdateLayeredWindow(g_hwnd, NULL, &pt, &sz, g_memDC, &src, 0, &blend, ULW_ALPHA)`.

### 2. State Machine & AI Behavior
- **Primary States (`State`)**:
  - `HIDDEN`: Idle timer monitoring system input (`GetLastInputInfo`). When idle time exceeds `g_idleMs`, calls `Spawn()`.
  - `CRAWL`: Active on-screen state. Automatically detects mouse/keyboard input to trigger `StartFlee()`. Sub-states:
    - `SUB_RUN`: Leg tripod gait active (`g_legAnimT` increments). Steering toward target angle.
    - `SUB_PAUSE`: Stationary pause (`g_legAnimT` frozen). Antennae sniffing amplitude multiplied (`ampMult = 2.5f`). Cycles through 3 pause durations: micro-pause (35%), sniffing pause (50%), deep rest (15%).
  - `FLEE`: High-speed sprint (600–850 px/s) toward nearest monitor edge, hiding upon exit.

### 3. Kinematics Engines

#### Biomechanical Leg Gait (`DrawLeg`)
- 6 legs (Front, Middle, Rear) rendered as 3-segment linkages (Femur, Tibia, Tarsus) with 4 sharp Tibia spines.
- Non-linear kinetic shaping `AsymSwing(t)` creates fast forward recovery swing and smooth stance push.
- Phase lag applied across joint angles (`kneeFlex`, `clawFlex`).

#### Forward Kinematics Whip Antennae (`DrawSingleAntenna`)
- Rendered via tangential angle integration along 5 control nodes ($s \in [0, 1]$).
- Formula:
  $$\text{tangAngle}(s) = \text{restAngle} + \theta_{\text{base}} \cdot s + \theta_{\text{whip}} \cdot s^{1.4} + \text{dir} \cdot 0.15 \cdot s^2$$
- Guarantees **anchor at base socket ($s=0$, offset 0)** and **maximum swing amplitude at outermost tip ($s=1.0$)**.

### 4. Edge Steering & Multi-Monitor Support
- Layer 1: 180px soft early warning steering toward screen center.
- Layer 2: 40px hard boundary clamp per active monitor (`MonitorFromPoint`).
- Layer 3: Offscreen emergency return dash (350 px/s).

---

## 🤖 Guidelines for AI Agents Working on This Codebase

When making modifications or adding new features:

1. **Preserve Single-File Simplicity**:
   - Keep `main.cpp` self-contained unless explicit modularization is requested.
   - Do NOT introduce external third-party C++ libraries (e.g. Boost, OpenCV). Stick strictly to Win32 API and GDI+.

2. **Resource Management Rules**:
   - Always ensure proper cleanup of GDI+ objects (`Graphics`, `Bitmap`, `Pen`, `Brush`, `GraphicsPath`).
   - Clean up Win32 handles (`HDC`, `HBITMAP`, `HANDLE`) to prevent memory leaks during long-running desktop sessions.

3. **Performance & Binary Size Optimization**:
   - Keep binary footprint minimal (~270 KB).
   - Ensure `UpdateLayeredWindow` updates remain smooth at 50 FPS with low CPU consumption.

4. **Testing Checklist Before Declaring Completion**:
   - [ ] Run `taskkill /f /im cockroach.exe` if running.
   - [ ] Execute `build.bat` and verify **0 warnings, 0 errors**.
   - [ ] Verify tray icon and single-instance mutex behavior.
