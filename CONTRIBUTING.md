# Contributing to Takeoff

Thank you for your interest in contributing to Takeoff!

Takeoff is a lightweight, focused, native Windows application launcher built with Win32, Direct2D, and DirectWrite. Our primary design principles are **speed, responsiveness, minimal resource usage, zero telemetry, and zero heavy dependencies**.

## Design Philosophy

- **Native and Dependency-Free**: Use standard Win32, Direct2D, DirectWrite, and standard C++17 library facilities. Do not introduce heavy third-party runtimes, frameworks, or dependencies.
- **Instant Response**: Keystroke processing and search indexing must feel instantaneous (< 5ms response time).
- **Privacy First**: Zero telemetry, zero tracking, zero background web services. Network access is strictly restricted to checking GitHub Releases when enabled.
- **Reliability and Fallback**: Maintain graceful fallbacks for transparency, high contrast, and varying Windows versions (Windows 10 1809+ and Windows 11).

## Development Setup

### Prerequisites

- Windows 10 version 1809 or newer, or Windows 11
- Visual Studio 2022 with the **Desktop development with C++** workload
- Windows 10 / 11 SDK
- *(Optional)* CMake 3.21 or newer
- *(Optional)* Python 3.9+ with `Pillow` and `tkinter` (for interactive UI smoke testing)

### Building the Project

#### Option A: Visual Studio 2022 (IDE)

1. Open `Takeoff.sln` in Visual Studio.
2. Set configuration to `Release` and platform to `x64`.
3. Choose **Build > Build Solution** (or press `Ctrl+Shift+B`).
4. The executable will be produced at `x64\Release\Takeoff.exe`.

#### Option B: CMake (Command Line)

```powershell
# Configure
cmake -S . -B build -A x64

# Build
cmake --build build --config Release
```

The output executable will be located at `build\Release\Takeoff.exe`.

## Running Tests

### Core Regression Tests

The CMake build configures a fast, dependency-free test suite that validates fuzzy matching, text input navigation, hotkey formatting/migrations, recency ranking, and update version comparisons:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

You can also run the test executable directly:

```powershell
.\build\Release\TakeoffCoreTests.exe
```

### UI Smoke Test

An interactive UI smoke test validates window creation, acrylic composition, caret blinking, text selection, and actions overlay using a standalone fixture window:

```powershell
py tests/ui_smoke.py build/Release/TakeoffUiTests.exe
```

*Note: The test executable `TakeoffUiTests.exe` runs in an isolated mode with separate window classes and mutexes, so it does not interfere with your running launcher.*

## Code Style & Guidelines

- **C++ Standard**: C++17 (`/std:c++17` on MSVC).
- **Compiler Warnings**: Clean build at warning level 4 (`/W4`) with permissive mode disabled (`/permissive-`).
- **Encoding**: Source files are UTF-8 with standard Windows line endings (`CRLF`) or `LF`.
- **Naming**:
  - Types / Classes / Structs: `PascalCase`
  - Functions / Methods: `PascalCase`
  - Constants / Enums: `kCamelCase` or `UPPER_SNAKE_CASE`
  - Member variables: `camelCase` or `m_camelCase`
- **Memory & Resource Management**: Prefer RAII wrappers (e.g., `Microsoft::WRL::ComPtr` for COM objects, unique handles for Win32 primitives). Avoid manual raw pointer memory leaks.

## Pull Request Process

1. **Check Issues**: For significant changes or new features, please check existing issues or open a discussion issue first.
2. **Branch**: Create a feature branch off `main` (`git checkout -b feature/your-feature-name`).
3. **Verify**: Ensure both Visual Studio and CMake builds compile cleanly without warnings, and all `ctest` tests pass.
4. **Submit**: Open a Pull Request with a clear title, description of the change, and notes on how you verified it.
