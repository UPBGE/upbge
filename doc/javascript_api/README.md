# UPBGE JavaScript/TypeScript API Documentation

This directory contains the documentation for the JavaScript/TypeScript API in UPBGE.

## Prerequisites

### Python Version

**Python 3.11 or higher is required** (UPBGE requires Python 3.11+).

- **Minimum**: Python 3.11
- **Recommended**: Python 3.11 or 3.12
- **Maximum**: Python 3.13 (3.14+ not supported yet)

You can check your Python version:
```bash
python --version
# or
python3 --version
```

### Installing Python

#### Windows:
1. Download Python 3.11+ from [python.org](https://www.python.org/downloads/)
2. During installation, check "Add Python to PATH"
3. Verify installation: `python --version`

#### Linux/macOS:
Most distributions come with Python 3.11+, but if needed:
```bash
# Ubuntu/Debian
sudo apt-get install python3.11 python3.11-venv python3.11-dev

# macOS (using Homebrew)
brew install python@3.11
```

### Node.js Version

**Node.js is required for TypeScript language server support in the editor.**

- **Recommended**: Node.js >= 20.19.x
- **Minimum**: Node.js 18.x (may have limited support)

You can check your Node.js version:
```bash
node --version
# or
nodejs --version
```

### Installing Node.js

#### Windows:
1. Download Node.js 20.19+ from [nodejs.org](https://nodejs.org/)
2. Use the LTS (Long Term Support) version
3. During installation, ensure "Add to PATH" is checked
4. Verify installation: `node --version` and `npm --version`

#### Linux/macOS:
```bash
# Ubuntu/Debian (using NodeSource repository)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs

# macOS (using Homebrew)
brew install node@20

# Verify installation
node --version
npm --version
```

### TypeScript Installation

**TypeScript must be installed globally** for the editor's autocomplete feature to work.

After installing Node.js, install TypeScript globally:
```bash
npm install -g typescript
```

Verify TypeScript installation:
```bash
tsc --version
```

**Note**: The `typescript-language-server` is automatically installed via `npx` when needed, but having TypeScript installed globally ensures better editor support.

## Structure

- **`rst/`** - reStructuredText source files for the documentation
- **`examples/`** - Example JavaScript and TypeScript scripts
- **`static/`** - Static files (CSS, JavaScript, images) for the documentation
- **`templates/`** - HTML templates for Sphinx
- **`conf.py`** - Sphinx configuration file
- **`requirements.txt`** - Python dependencies for building documentation
- **`sphinx_doc_gen_blender.py`** - Script that runs inside Blender to generate documentation

## Building the Documentation

### Step 1: Install Prerequisites

1. Install Python 3.11+ (see Prerequisites above)

2. Install Node.js >= 20.19.x (see Prerequisites above)

3. Install TypeScript globally:
   ```bash
   npm install -g typescript
   ```

4. Install Sphinx and Python dependencies:
   ```bash
   python -m pip install -r doc/javascript_api/requirements.txt
   ```

   **Note**: If `pip` command doesn't work, always use `python -m pip` instead.

### Step 2: Generate and Build Documentation

The documentation is generated automatically from the Python API documentation, similar to how `make doc_py` works for Python.

#### On Linux/macOS:
```bash
make doc_js
```

#### On Windows:
```cmd
make.bat doc_js
```

#### On Windows (Git Bash):
```bash
cmd //c make.bat doc_js
```

This will:
1. Run Blender to generate JavaScript API RST files from Python API
2. Build HTML documentation using Sphinx
3. Output will be in `doc/javascript_api/sphinx-out/index.html`

## How It Works

The JavaScript/TypeScript API documentation is automatically generated from the Python API documentation,
ensuring synchronization between the two APIs.

### Build Process

1. **Generate RST from Python API**: Blender runs `sphinx_doc_gen_blender.py` which converts Python RST files to JavaScript equivalents
2. **Build HTML**: Sphinx compiles the RST files to HTML in `sphinx-out/`

The process is identical to `make doc_py` for Python documentation, but generates JavaScript/TypeScript formatted documentation instead.

## Documentation Structure

- **`index.rst`** - Main entry point
- **`info_javascript_overview.rst`** - Overview of JavaScript/TypeScript in UPBGE
- **`info_javascript_quickstart.rst`** - Quick start guide
- **`info_javascript_python_differences.rst`** - Differences between Python and JavaScript APIs
- **`bge.javascript.rst`** - JavaScript API reference (generated from `bge.logic.rst`)

## Adding New Documentation

1. Create or edit RST files in `rst/`
2. Add references to new files in `index.rst` using `.. toctree::`
3. Rebuild the documentation using `make doc_js` (or `make.bat doc_js` on Windows)

## Notes

- The JavaScript API documentation follows the same structure as the Python API documentation
- Example scripts are in `examples/` and can be referenced in the documentation
- The documentation uses the Read the Docs theme (sphinx_rtd_theme)
- Generated files in `rst/` are automatically created from Python API - manual edits may be overwritten

## Troubleshooting

### Python Version Issues

If you get errors about Python version:
- Ensure you have Python 3.11 or higher
- Check with: `python --version` or `python3 --version`
- UPBGE requires Python 3.11+ for building

### pip Command Not Found

If `pip` command doesn't work:
- **Use `python -m pip` instead**: `python -m pip install -r doc/javascript_api/requirements.txt`
- This works even if `pip` is not in your PATH
- On Windows, Python Scripts may not be in PATH by default

### Sphinx Not Found

If `sphinx-build` is not found:
- Install dependencies: `python -m pip install -r doc/javascript_api/requirements.txt`
- The build scripts will automatically use `python -m sphinx` if `sphinx-build` is not in PATH

### Blender Not Found

If Blender is not found during generation:
- Set `BLENDER_BIN` environment variable to Blender executable path
- Or ensure `blender` is in your PATH
- On Windows, the script will look for `blender.exe` in common locations

### Node.js Not Found

If `node` or `npm` commands are not found:
- Ensure Node.js is installed (see Prerequisites above)
- Verify installation: `node --version` and `npm --version`
- On Windows, restart your terminal after installation
- On Linux/macOS, ensure Node.js is in your PATH
- The editor's TypeScript autocomplete requires Node.js to be available

### TypeScript Not Installed

If TypeScript autocomplete doesn't work in the editor:
- Install TypeScript globally: `npm install -g typescript`
- Verify installation: `tsc --version`
- Ensure `npx` is available (comes with Node.js)
- The `typescript-language-server` will be installed automatically via `npx` when needed
