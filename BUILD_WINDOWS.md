# UPBGE Build Guide for Windows

## 📋 Prerequisites

### 1. Visual Studio (Required)
- **Visual Studio 2022** (recommended) or **Visual Studio 2019**
- During installation, select the **"Desktop Development with C++"** workload
- Includes: MSVC, Windows SDK, CMake tools
- Download: https://visualstudio.microsoft.com/downloads/

### 2. CMake
- Version 3.10 or higher
- Download: https://cmake.org/download/
- During installation, check "Add CMake to PATH"

### 3. Python 3.11+
- Already installed ✅
- Verify: `python --version`

### 4. Sphinx (For Documentation)
Sphinx is required to generate JavaScript/TypeScript documentation.

**Option A: Via pip (Recommended - already installed)**
```cmd
python -m pip install -r doc/javascript_api/requirements.txt
```

**Option B: Via Chocolatey**
If you have Chocolatey installed:
```cmd
choco install sphinx
```
More information: https://www.sphinx-doc.org/en/master/usage/installation.html#windows

**Verify installation:**
```cmd
python -m sphinx --version
```

**Note**: Sphinx is already installed via pip on your system. The `sphinx-build` command may not be in PATH, but you can use `python -m sphinx` which works perfectly.

### 5. Disk Space
- **Minimum**: 15-20 GB free
- **Recommended**: 30+ GB (including dependencies and build)

## 🚀 Step by Step

### Step 1: Download Dependencies

In the UPBGE project directory:

**Option A: Using make.bat (Windows CMD)**
```cmd
make.bat update
```

**Option B: Using make (if you have make installed, e.g., via Git Bash/MSYS2)**
```bash
make update
```

This downloads all required pre-compiled libraries (SVN and Git).

**Note**: This can take a while the first time (several GB).

### Step 2: Build UPBGE

**Option A: Release Build (Recommended for use)**
```cmd
make.bat release
```
or
```bash
make release
```

**Option B: Developer Build (Faster, with debug)**
```cmd
make.bat developer
```
or
```bash
make developer
```

**Option C: Lite Build (Smaller, without some features)**
```cmd
make.bat lite
```
or
```bash
make lite
```

### Step 3: Wait for Compilation

- **First compilation**: 1-3 hours (depending on hardware)
- **Recompilations**: 10-30 minutes (only changed files)

### Step 4: Find the Executable

After building, `blender.exe` will be in:

```
..\build_windows_Full_x64_vc17_Release\bin\Release\blender.exe
```

Or similar, depending on configuration.

## 🔧 Build Options

### Specify Visual Studio Version

```cmd
make.bat 2022 release    # Visual Studio 2022
make.bat 2019 release    # Visual Studio 2019
```

### Build with Ninja (faster)

```cmd
make.bat ninja release
```

### Debug Build (for development)

```cmd
make.bat debug
```

### Generate Project Files Only (without building)

```cmd
make.bat nobuild release
```

## ⚠️ Common Issues

### Visual Studio not found

**Solution**: Install Visual Studio 2022 with "Desktop Development with C++" workload

### CMake not found

**Solution**: 
1. Install CMake
2. Add to PATH: `C:\Program Files\CMake\bin`
3. Or use: `set CMAKE=C:\Program Files\CMake\bin\cmake.exe`

### Sphinx not found

**Solution**: 
1. Install via pip: `python -m pip install -r doc/javascript_api/requirements.txt`
2. Or use Chocolatey: `choco install sphinx`
3. The build scripts will automatically use `python -m sphinx` if `sphinx-build` is not in PATH

### Disk space error

**Solution**: Free up space or change build directory:
```cmd
make.bat builddir D:\build_upbge release
```

## 📝 After Building

### Test Blender

```cmd
..\build_windows_Full_x64_vc17_Release\bin\Release\blender.exe
```

### Generate JavaScript Documentation

Now that you have Blender compiled:

**Using make.bat:**
```cmd
make.bat doc_js
```

**Using make:**
```bash
make doc_js
```

Documentation will be in:
```
doc\javascript_api\sphinx-out\index.html
```

## 💡 Tips

1. **First time**: Use `make.bat update` or `make update` to download all dependencies
2. **Rebuild**: Use `make.bat release` or `make release` again (only rebuilds what changed)
3. **Clean build**: Delete the `build_windows_*` folder and compile again
4. **Faster build**: Use `ninja` instead of MSBuild: `make.bat ninja release` or `make ninja release`
5. **Development**: Use `developer` for faster builds with debug
6. **Using make directly**: If you have `make` installed (via Git Bash, MSYS2, or other), you can use `make` instead of `make.bat` for a more Unix-like experience

## 🔗 Useful Links

- Official documentation: https://developer.blender.org/docs/handbook/building_blender/windows/
- Visual Studio: https://visualstudio.microsoft.com/downloads/
- CMake: https://cmake.org/download/
- Sphinx (Documentation): https://www.sphinx-doc.org/en/master/usage/installation.html#windows
