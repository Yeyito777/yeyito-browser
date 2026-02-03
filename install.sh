#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir="${repo_dir}/build"
webengine_build="${build_dir}/qtwebengine"
install_dir="${build_dir}/install"
venv_dir="${HOME}/.local/share/qutebrowser-venv"
venv_python="${venv_dir}/bin/python"
launcher_dir="${HOME}/.local/bin"
launcher_path="${launcher_dir}/qutebrowser"

# ============================================================
# Phase 1: Check submodule
# ============================================================
echo "[+] Checking QtWebEngine submodule..."
if [[ ! -d "${repo_dir}/qtwebengine/src/3rdparty/chromium" ]]; then
    echo "[+] Initializing QtWebEngine submodule (this may take several hours)..."
    git -C "${repo_dir}" submodule update --init --recursive
fi

# ============================================================
# Phase 2: Build QtWebEngine
# ============================================================
echo "[+] Building QtWebEngine..."
mkdir -p "${build_dir}"

if [[ ! -f "${webengine_build}/build.ninja" ]]; then
    echo "[+] Running CMake configure (first time)..."
    cmake -S "${repo_dir}/qtwebengine" -B "${webengine_build}" -GNinja \
        -DCMAKE_INSTALL_PREFIX="${install_dir}" \
        -DCMAKE_PREFIX_PATH="/usr/lib/cmake/Qt6" \
        -DQT_FEATURE_webengine_system_ffmpeg=ON \
        -DQT_FEATURE_webengine_system_icu=ON \
        -DQT_FEATURE_webengine_system_libevent=ON \
        -DQT_FEATURE_webengine_system_re2=ON \
        -DQT_FEATURE_webengine_proprietary_codecs=ON
fi

echo "[+] Running Ninja build (first build takes 2-6 hours)..."
ninja -C "${webengine_build}" -j$(nproc)

echo "[+] Installing to ${install_dir}..."
ninja -C "${webengine_build}" install

# ============================================================
# Phase 3: Python virtualenv
# ============================================================
echo "[+] Creating virtualenv at ${venv_dir}"
python -m venv --system-site-packages "${venv_dir}" >/dev/null

echo "[+] Upgrading pip/wheel"
"${venv_python}" -m pip install --upgrade pip wheel >/dev/null

echo "[+] Installing qutebrowser from ${repo_dir}"
"${venv_python}" -m pip install --upgrade "${repo_dir}" >/dev/null

# ============================================================
# Phase 4: Create launcher with LD_LIBRARY_PATH
# ============================================================
echo "[+] Writing launcher ${launcher_path}"
mkdir -p "${launcher_dir}"
cat >"${launcher_path}" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${install_dir}/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${install_dir}/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
exec ${venv_dir}/bin/python -m qutebrowser "\$@"
EOF
chmod +x "${launcher_path}"

echo
echo "Install complete. Launch with: ${launcher_path}"
