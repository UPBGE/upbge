if "%BUILD_VS_YEAR%"=="2019" set BUILD_VS_LIBDIRPOST=vc15
if "%BUILD_VS_YEAR%"=="2022" set BUILD_VS_LIBDIRPOST=vc15

set BUILD_VS_SVNDIR=win64_%BUILD_VS_LIBDIRPOST%
set BUILD_VS_LIBDIR="%BLENDER_DIR%..\lib\%BUILD_VS_SVNDIR%"

if NOT "%verbose%" == "" (
	echo Library Directory = "%BUILD_VS_LIBDIR%"
)
if NOT EXIST %BUILD_VS_LIBDIR% (
	rem libs not found, but svn is on the system
	if not "%SVN%"=="" (
		echo.
		echo The required external libraries in %BUILD_VS_LIBDIR% are missing
		echo.
		set /p GetLibs= "Would you like to download them? (y/n)"
		if /I "!GetLibs!"=="Y" (
			echo.
			echo Downloading %BUILD_VS_SVNDIR% libraries, please wait.
			echo.
:RETRY			
			"%SVN%" checkout https://svn.blender.org/svnroot/bf-blender/trunk/lib/%BUILD_VS_SVNDIR% %BUILD_VS_LIBDIR%
			if errorlevel 1 (
				set /p LibRetry= "Error during download, retry? y/n"
				if /I "!LibRetry!"=="Y" (
					cd %BUILD_VS_LIBDIR%
					"%SVN%" cleanup 
					cd %BLENDER_DIR%
					goto RETRY
				)
				echo.
				echo Error: Download of external libraries failed. 
				echo This is needed for building, please manually run 'svn cleanup' and 'svn update' in
				echo %BUILD_VS_LIBDIR% , until this is resolved you CANNOT make a successful blender build
				echo.
				exit /b 1
			)
		)
	)
) else (
	if NOT EXIST %PYTHON% (
		if not "%SVN%"=="" (
			echo.
			echo Python not found in external libraries, updating to latest version
			echo.
			"%SVN%" update %BUILD_VS_LIBDIR%
		)
	)
)

if NOT EXIST %BUILD_VS_LIBDIR% (
	echo.
	echo Error: Required libraries not found at "%BUILD_VS_LIBDIR%"
	echo This is needed for building, aborting!
	echo.
	if "%SVN%"=="" (
		echo This is most likely caused by svn.exe not being available.
	)
	exit /b 1
)