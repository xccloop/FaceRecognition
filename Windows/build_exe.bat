@echo off
REM ============================================================
REM  FaceRecognition Windows 后台 — 打包脚本
REM  产出：dist/FaceRecognition.exe（单文件）
REM  环境：conda activate facerec
REM ============================================================

setlocal

echo [1/4] 激活 conda 环境...
call conda activate facerec || exit /b 1

echo [2/4] 安装/更新 PyInstaller...
pip install pyinstaller>=6.0 -q

echo [3/4] 构建前端...
cd frontend
call npm run build
cd ..

echo [4/4] 打包 EXE...
pyinstaller --clean --noconfirm ^
    --name FaceRecognition ^
    --onefile ^
    --console ^
    --add-data "frontend/dist;frontend/dist" ^
    --add-data "photos;photos" ^
    --hidden-import sqlalchemy ^
    --hidden-import uvicorn ^
    --hidden-import fastapi ^
    --hidden-import aiofiles ^
    --hidden-import python_multipart ^
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
    main.py

if %errorlevel% == 0 (
    echo.
    echo ============================================
    echo   打包成功！
    echo   输出: dist/FaceRecognition.exe
    echo ============================================
) else (
    echo.
    echo 打包失败，请检查错误信息。
    exit /b 1
)

endlocal
