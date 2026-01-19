set SOURCEDIR=%BLENDER_DIR%/doc/javascript_api/rst
set BUILDDIR=%BLENDER_DIR%/doc/javascript_api/sphinx-out
if "%BF_LANG%" == "" set BF_LANG=en
set SPHINXOPTS=-j auto -D language=%BF_LANG%

call "%~dp0\find_sphinx.cmd"

if EXIST "%SPHINX_BIN%" (
    goto detect_sphinx_done
)

echo unable to locate sphinx-build, run "set sphinx_BIN=full_path_to_sphinx-build.exe"
exit /b 1

:detect_sphinx_done

call "%~dp0\find_blender.cmd"

if EXIST "%BLENDER_BIN%" (
    goto detect_blender_done
)

echo unable to locate blender, run "set BLENDER_BIN=full_path_to_blender.exe"
exit /b 1

:detect_blender_done

echo Generating JavaScript/TypeScript API documentation from Python API...

%BLENDER_BIN% ^
	--background --factory-startup ^
	--python %BLENDER_DIR%/doc/javascript_api/sphinx_doc_gen_blender.py -- ^
	--output=%BLENDER_DIR%/doc/javascript_api/rst --force

if %ERRORLEVEL% NEQ 0 (
    echo Failed to generate JavaScript API documentation
    exit /b 1
)

echo Building HTML documentation...

if "%USE_PYTHON_SPHINX%" == "1" (
    python -m sphinx -b html %SPHINXOPTS% %O% %SOURCEDIR% %BUILDDIR%
) else (
    "%SPHINX_BIN%" -b html %SPHINXOPTS% %O% %SOURCEDIR% %BUILDDIR%
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Documentation built successfully!
    echo Open: %BUILDDIR%\index.html
) else (
    echo.
    echo Build failed. Check errors above.
    exit /b 1
)

:EOF
