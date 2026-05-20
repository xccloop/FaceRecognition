@echo off
REM ============================================================
REM  FaceRecognition Windows 后台 — 打包脚本
REM  产出：dist/FaceRecognition.exe（单文件原生桌面应用）
REM  环境：conda env facerec (python=3.10)
REM
REM  注意：此脚本必须在 cmd.exe 中运行（不能在 Git Bash 中）
REM  如果 conda activate 失败，请使用备选方案：
REM    直接指定 Python 路径（见 build_exe_direct.bat）
REM ============================================================

setlocal enabledelayedexpansion

REM --- 查找 facerec 环境的 Python ---
set "FACEREC_PYTHON="

REM 方式1：尝试 conda activate
call conda activate facerec 2>nul
if %errorlevel% equ 0 (
    for /f "tokens=*" %%i in ('where python') do set "FACEREC_PYTHON=%%i"
    goto :found_python
)

REM 方式2：直接找 anaconda/miniconda 路径
for %%d in (D:\ANACONDA D:\Miniconda3 C:\Users\%USERNAME%\miniconda3 C:\Users\%USERNAME%\anaconda3) do (
    if exist "%%d\envs\facerec\python.exe" (
        set "FACEREC_PYTHON=%%d\envs\facerec\python.exe"
        goto :found_python
    )
)

REM 方式3：从 PATH 中找 conda，再推导 envs 路径
for /f "tokens=*" %%i in ('where conda 2^>nul') do (
    for %%p in ("%%~dpi..") do (
        if exist "%%~fp\envs\facerec\python.exe" (
            set "FACEREC_PYTHON=%%~fp\envs\facerec\python.exe"
            goto :found_python
        )
    )
)

echo [ERROR] 找不到 facerec 环境的 Python！
echo 请确认 conda 环境 facerec 已创建：conda create -n facerec python=3.10
pause
exit /b 1

:found_python
echo [INFO] 使用 Python: %FACEREC_PYTHON%

echo [1/4] 安装/更新依赖...
"%FACEREC_PYTHON%" -m pip install -r requirements.txt -q
if %errorlevel% neq 0 (
    echo [ERROR] pip install 失败
    pause
    exit /b 1
)

echo [2/4] 验证关键依赖...
"%FACEREC_PYTHON%" -c "import webview; print('  webview OK (path: %s)' %% webview.__file__.split('site-packages')[1])" || (
    echo [ERROR] webview 模块未安装！请执行: pip install pywebview
    pause
    exit /b 1
)
"%FACEREC_PYTHON%" -c "import fastapi; print('  fastapi OK')" || (
    echo [ERROR] fastapi 模块未安装！
    pause
    exit /b 1
)
"%FACEREC_PYTHON%" -c "import clr; print('  pythonnet OK')" || (
    echo [ERROR] pythonnet 未安装！请执行: pip install pythonnet
    pause
    exit /b 1
)

echo [3/4] 构建前端...
cd frontend
call npm run build
if %errorlevel% neq 0 (
    echo [ERROR] 前端构建失败
    cd ..
    pause
    exit /b 1
)
cd ..

echo [4/4] 打包 EXE（原生桌面窗口）...
echo   注意：如果在此步骤看到 'WARNING: Hidden import ... not found'
echo   说明有依赖缺失，打包产物可能不完整！

"%FACEREC_PYTHON%" -m PyInstaller --clean --noconfirm ^
    --name FaceRecognition ^
    --onefile ^
    --windowed ^
    --additional-hooks-dir=. ^
    --add-data "frontend/dist;frontend/dist" ^
    --add-data "photos;photos" ^
    --hidden-import sqlalchemy ^
    --hidden-import uvicorn ^
    --hidden-import fastapi ^
    --hidden-import aiofiles ^
    --hidden-import python_multipart ^
    --hidden-import webview ^
    --hidden-import clr ^
    --hidden-import webview.platforms.winforms ^
    --hidden-import uvicorn.logging ^
    --hidden-import uvicorn.loops ^
    --hidden-import uvicorn.loops.auto ^
    --hidden-import uvicorn.protocols ^
    --hidden-import uvicorn.protocols.http ^
    --hidden-import uvicorn.protocols.http.auto ^
    --hidden-import uvicorn.protocols.websockets ^
    --hidden-import uvicorn.protocols.websockets.auto ^
    --hidden-import uvicorn.lifespan ^
    --hidden-import uvicorn.lifespan.on ^
    --collect-all starlette ^
    --collect-all fastapi ^
    --collect-all webview ^
    main.py

if %errorlevel% neq 0 (
    echo.
    echo ============================================
    echo   打包失败！
    echo ============================================
    pause
    exit /b 1
)

REM 验证产物大小（正常应在 15-40 MB 之间）
for %%A in (dist\FaceRecognition.exe) do set EXE_SIZE=%%~zA
set /a EXE_SIZE_MB=!EXE_SIZE! / 1048576
echo.
echo ============================================
echo   打包完成！
echo   输出: dist\FaceRecognition.exe
echo   大小: !EXE_SIZE_MB! MB
if !EXE_SIZE_MB! lss 12 (
    echo   [WARNING] EXE 小于 12 MB，可能缺少依赖！
) else (
    echo   [OK] 大小正常，依赖完整。
)
echo ============================================

endlocal
