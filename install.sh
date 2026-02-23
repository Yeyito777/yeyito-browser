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
last_commit_file="${build_dir}/.last_build_commit"
last_bindings_commit_file="${build_dir}/.last_bindings_commit"

# Parse flags
dirty=false
for arg in "$@"; do
    case $arg in
        --dirty) dirty=true ;;
    esac
done

# ============================================================
# Phase 1: Check submodules
# ============================================================
echo "[+] Checking submodules..."
if [[ ! -d "${repo_dir}/qtwebengine/src/3rdparty/chromium" ]] || [[ ! -d "${repo_dir}/pyqt6-webengine/sip" ]]; then
    echo "[+] Initializing submodules (QtWebEngine download may take several hours)..."
    git -C "${repo_dir}" submodule update --init --recursive
fi

# ============================================================
# Phase 2: Build QtWebEngine
# ============================================================
mkdir -p "${build_dir}"

current_commit=$(git -C "${repo_dir}/qtwebengine" rev-parse HEAD 2>/dev/null || echo "unknown")
last_commit=$(cat "${last_commit_file}" 2>/dev/null || echo "none")

if [[ "${current_commit}" == "${last_commit}" ]] && [[ "${dirty}" == false ]]; then
    echo "[+] QtWebEngine build is up to date (commit ${current_commit:0:10})"
    echo "    Use --dirty to force rebuild with uncommitted changes"
else
    echo "[+] Building QtWebEngine..."
    if [[ "${dirty}" == true ]]; then
        echo "    --dirty flag set, building with current working tree"
    elif [[ "${last_commit}" == "none" ]]; then
        echo "    First build (commit ${current_commit:0:10})"
    else
        echo "    New commit: ${last_commit:0:10} -> ${current_commit:0:10}"
    fi

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
    # Unlink old shared libraries and renderer binary before installing so that
    # a running qutebrowser keeps the old inodes alive via mmap. ninja install
    # then creates fresh files with new inodes — no in-place overwrites.
    find "${install_dir}/lib" -name "libQt6WebEngine*.so*" -type f -delete 2>/dev/null || true
    rm -f "${install_dir}/lib/qt6/QtWebEngineProcess" 2>/dev/null || true
    ninja -C "${webengine_build}" install

    # Record the commit we just built
    echo "${current_commit}" > "${last_commit_file}"
fi

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
# Phase 4: Build custom PyQt6-WebEngine bindings
# ============================================================
# The stock PyQt6-WebEngine (from pip/system) doesn't know about our custom
# C++ API additions (e.g., ElementShaderEnabled enum). We build from source
# against our custom Qt headers so the SIP bindings include our enum values.

bindings_current_commit=$(git -C "${repo_dir}/pyqt6-webengine" rev-parse HEAD 2>/dev/null || echo "unknown")
bindings_last_commit=$(cat "${last_bindings_commit_file}" 2>/dev/null || echo "none")

if [[ "${bindings_current_commit}" == "${bindings_last_commit}" ]] && [[ "${dirty}" == false ]]; then
    echo "[+] PyQt6-WebEngine bindings are up to date (commit ${bindings_current_commit:0:10})"
else
    echo "[+] Building custom PyQt6-WebEngine bindings..."

    # Ensure SIP build tools are installed
    "${venv_python}" -m pip install --upgrade 'PyQt-builder>=1.19,<2' 'sip>=6.13.1,<7' >/dev/null 2>&1

    # Determine the venv site-packages path dynamically
    venv_site_packages=$("${venv_python}" -c "import sysconfig; print(sysconfig.get_path('purelib'))")

    # Find the SYSTEM PyQt6 directory (not the venv's — use site.getsitepackages()
    # which only returns system paths, avoiding the venv shadowing problem)
    sys_pyqt6_dir=$("${venv_python}" -c "
import site, os
for d in site.getsitepackages():
    p = os.path.join(d, 'PyQt6')
    if os.path.isdir(os.path.join(p, 'bindings')):
        print(p)
        break
")

    if [[ -z "${sys_pyqt6_dir}" ]]; then
        echo "ERROR: Could not find system PyQt6 bindings. Install python-pyqt6." >&2
        exit 1
    fi

    # Set up venv PyQt6 directory so sip-install can find base bindings
    # (QtCore, QtGui, etc.) and install our custom modules
    mkdir -p "${venv_site_packages}/PyQt6"
    if [[ "${sys_pyqt6_dir}/__init__.py" != "${venv_site_packages}/PyQt6/__init__.py" ]]; then
        cp -f "${sys_pyqt6_dir}/__init__.py" "${venv_site_packages}/PyQt6/__init__.py"
    fi
    # Copy system bindings directory (sip-install needs to write into it)
    if [[ "${sys_pyqt6_dir}/bindings" != "${venv_site_packages}/PyQt6/bindings" ]]; then
        rm -rf "${venv_site_packages}/PyQt6/bindings"
        cp -r "${sys_pyqt6_dir}/bindings" "${venv_site_packages}/PyQt6/bindings"
    fi

    # Build and install from our submodule source
    # QMAKEPATH tells qmake to search our custom install dir for mkspecs/modules/*.pri
    # (the system qmake doesn't know about our custom WebEngine modules otherwise)
    #
    # The .pri module specs use $$QT_MODULE_LIB_BASE and the .prl files use
    # $$[QT_INSTALL_LIBS], both of which resolve to /usr/lib (system path).
    # Since we don't have the system qt6-webengine package, we patch these
    # to use absolute paths to our custom install before sip-install runs.
    webengine_mkspecs="${install_dir}/lib/qt6/mkspecs/modules"
    for pri in "${webengine_mkspecs}"/qt_lib_webengine*.pri "${webengine_mkspecs}"/qt_lib_pdf*.pri; do
        [[ -f "$pri" ]] || continue
        sed -i \
            -e "s|\$\$QT_MODULE_LIB_BASE|${install_dir}/lib|g" \
            -e "s|\$\$QT_MODULE_INCLUDE_BASE|${install_dir}/include/qt6|g" \
            "$pri"
    done
    # .prl files list transitive link deps using $$[QT_INSTALL_LIBS] which resolves
    # to /usr/lib. Only redirect WebEngine-specific libraries to our install;
    # system libs (Qt6Quick, Qt6Gui, etc.) must stay at $$[QT_INSTALL_LIBS].
    for prl in "${install_dir}"/lib/libQt6WebEngine*.prl; do
        [[ -f "$prl" ]] || continue
        sed -i \
            -e "s|\$\$\[QT_INSTALL_LIBS\]/libQt6WebEngineCore\.so|${install_dir}/lib/libQt6WebEngineCore.so|g" \
            -e "s|\$\$\[QT_INSTALL_LIBS\]/libQt6WebEngineWidgets\.so|${install_dir}/lib/libQt6WebEngineWidgets.so|g" \
            -e "s|\$\$\[QT_INSTALL_LIBS\]/libQt6WebEngineQuick\.so|${install_dir}/lib/libQt6WebEngineQuick.so|g" \
            "$prl"
    done

    (
        cd "${repo_dir}/pyqt6-webengine"
        QMAKEPATH="${install_dir}/lib/qt6" "${venv_dir}/bin/sip-install" \
            --target-dir "${venv_site_packages}" \
            --qmake "$(command -v qmake6 || command -v qmake)" \
            --qmake-setting "INCLUDEPATH += ${install_dir}/include/qt6 ${install_dir}/include/qt6/QtWebEngineCore ${install_dir}/include/qt6/QtWebEngineWidgets" \
            --qmake-setting "LIBS += -L${install_dir}/lib" \
            --disable QtWebEngineQuick \
            --jobs "$(nproc)"
    )

    echo "${bindings_current_commit}" > "${last_bindings_commit_file}"
    echo "[+] Custom PyQt6-WebEngine bindings installed"
fi

# ============================================================
# Phase 5: Create launcher with LD_LIBRARY_PATH
# ============================================================
echo "[+] Writing launcher ${launcher_path}"
mkdir -p "${launcher_dir}"
cat >"${launcher_path}" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${install_dir}/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${install_dir}/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
export QTWEBENGINEPROCESS_PATH="${install_dir}/lib/qt6/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="${install_dir}/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="${install_dir}/share/qt6/translations/qtwebengine_locales"
exec ${venv_dir}/bin/python -m qutebrowser "\$@"
EOF
chmod +x "${launcher_path}"

echo
echo "Install complete. Launch with: ${launcher_path}"
