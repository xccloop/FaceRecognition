"""
PyInstaller hook for pywebview (imported as 'webview').

pywebview 在 Windows 上使用 Edge Chromium (WebView2)，通过 ctypes/COM 调用。
此 hook 确保所有子模块和平台后端被正确收集到 --onefile EXE 中。
"""
from PyInstaller.utils.hooks import collect_submodules, collect_data_files, collect_dynamic_libs

# 收集 webview 所有子模块（包括 platforms/*, js/* 等）
hiddenimports = collect_submodules('webview')

# 收集 webview 的数据文件（如 js/api.js, js/css/* 等）
datas = collect_data_files('webview', subdir=None, includes=['**/*.js', '**/*.css', '**/*.html'])

# 收集 webview 的动态库（如 .pyd, .dll）
binaries = collect_dynamic_libs('webview')

# 显式确保所有平台后端都作为 hidden import
hiddenimports += [
    'webview.platforms.edgechromium',
    'webview.platforms.winforms',
    'webview.platforms.cef',
    'webview.guilib',
    'webview.http',
    'webview.js',
    'webview.menu',
    'webview.util',
    'webview.window',
]
