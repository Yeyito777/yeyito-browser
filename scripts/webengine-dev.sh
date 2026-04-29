#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${script_dir}/.." && pwd)
build_dir="${repo_dir}/build"
webengine_build="${build_dir}/qtwebengine"
install_dir="${build_dir}/install"
venv_dir="${HOME}/.local/share/qutebrowser-venv"
launcher_dir="${HOME}/.local/bin"
launcher_path="${launcher_dir}/qutebrowser"

jobs="${JOBS:-$(nproc)}"
cmd="${1:-build-install}"

core_build_dir="${webengine_build}/src/core/RelWithDebInfo/x86_64"
stage_dir="${build_dir}/.webengine-dev-staging"

# Outer CMake/Ninja marker outputs for custom commands which delegate to the
# inner Chromium Ninja build. QtWebEngine's generated outer build treats these as
# outputs, but the delegated inner Ninja command can leave them absent after
# interrupted/manual builds. Missing markers make every outer Ninja invocation
# rerun the inner Chromium target and relink libQt6WebEngineCore. We create them
# once after the inner build so outer no-op checks stay no-op.
outer_marker_outputs=(
    "${core_build_dir}/obj/tools/v8_context_snapshot/v8_context_snapshot.stamp"
    "${core_build_dir}/convert_dict.stamp"
    "${core_build_dir}/webenginedriver_group"
    "${core_build_dir}/QtWebEngineCore"
)
outer_core_library="${webengine_build}/lib/libQt6WebEngineCore.so.6.10.2"

usage() {
    cat <<EOF
Usage: $0 [command]

Fast QtWebEngine/Chromium developer loop for this qutebrowser checkout.

Commands:
  build          Build only the qutebrowser-relevant QtWebEngine targets.
  install        Quickly copy rebuilt QtWebEngine runtime artifacts into install.
  install-full   Run Qt/CMake component install targets via DESTDIR staging.
  build-install  Build then quick-install QtWebEngine. Default.
  launcher       Rewrite ~/.local/bin/qutebrowser for this checkout/install tree.
  status         Print current paths and cache/build status.
  cache-status   Show whether ccache/sccache is configured/available.
  enable-cache   Reconfigure this build to use ccache/sccache if installed.
                 WARNING: first build after enabling a compiler launcher can be broad.
  help           Show this help.

Environment:
  JOBS=N         Ninja parallelism. Default: nproc.

Typical Chromium/Blink edit loop:
  make webengine-dev

This avoids:
  - rebuilding/installing QtPdf/QtWebEngineQuick unless needed
  - reinstalling the qutebrowser Python package
  - rebuilding PyQt6-WebEngine SIP bindings
EOF
}

require_build_tree() {
    if [[ ! -f "${webengine_build}/build.ninja" ]]; then
        echo "ERROR: ${webengine_build}/build.ninja not found." >&2
        echo "Run 'make install-dirty' once to configure/build the full tree first." >&2
        exit 1
    fi
    if [[ ! -d "${core_build_dir}" ]]; then
        echo "ERROR: Chromium inner build dir not found: ${core_build_dir}" >&2
        echo "Run 'make install-dirty' once to configure/build the full tree first." >&2
        exit 1
    fi
}

write_launcher() {
    echo "[+] Writing dev launcher ${launcher_path}"
    mkdir -p "${launcher_dir}"
    cat >"${launcher_path}" <<EOF
#!/usr/bin/env bash
export LD_LIBRARY_PATH="${install_dir}/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${install_dir}/plugins\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}"
export QTWEBENGINEPROCESS_PATH="${install_dir}/lib/qt6/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="${install_dir}/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="${install_dir}/share/qt6/translations/qtwebengine_locales"
# Dev-mode source checkout wins over the wheel copied into the venv, so Python
# edits in qutebrowser/ are live without rerunning pip install.
export PYTHONPATH="${repo_dir}\${PYTHONPATH:+:\$PYTHONPATH}"
exec ${venv_dir}/bin/python -m qutebrowser "\$@"
EOF
    chmod +x "${launcher_path}"
}

repair_outer_markers() {
    for marker in "${outer_marker_outputs[@]}"; do
        if [[ ! -e "${marker}" ]]; then
            mkdir -p "$(dirname "${marker}")"
            touch "${marker}"
        fi
    done
}

build_webengine() {
    require_build_tree
    echo "[+] Fast-building QtWebEngineCore through inner Chromium Ninja"
    echo "    inner build dir: ${core_build_dir}"
    echo "    jobs: ${jobs}"

    # Always ask the inner Chromium Ninja directly, because the outer CMake/Ninja
    # graph only depends on marker files and will not notice Chromium source edits
    # once those markers exist. The inner graph has the real C++ dependencies.
    ninja -C "${core_build_dir}" -j"${jobs}" QtWebEngineCore
    repair_outer_markers

    # Relink/copy the outer Qt library only when the inner stamp changed.
    ninja -C "${webengine_build}" -j"${jobs}" \
        lib/libQt6WebEngineCore.so.6.10.2 WebEngineWidgets
}

install_regular_file_if_changed() {
    local source="$1"
    local dest="$2"
    mkdir -p "$(dirname "${dest}")"
    if [[ -f "${dest}" ]] && cmp -s "${source}" "${dest}"; then
        return 0
    fi
    local tmp
    tmp="${dest}.tmp.$$"
    cp -a "${source}" "${tmp}"
    mv -f "${tmp}" "${dest}"
}

install_qt_library_family() {
    local stem="$1"
    local source_versioned="${webengine_build}/lib/${stem}.so.6.10.2"
    if [[ ! -f "${source_versioned}" ]]; then
        return 0
    fi
    install_regular_file_if_changed "${source_versioned}" \
        "${install_dir}/lib/${stem}.so.6.10.2"
    ln -sfn "${stem}.so.6.10.2" "${install_dir}/lib/${stem}.so.6"
    ln -sfn "${stem}.so.6" "${install_dir}/lib/${stem}.so"
}

quick_install_webengine() {
    require_build_tree
    if [[ ! -d "${install_dir}" ]]; then
        echo "ERROR: install tree missing: ${install_dir}" >&2
        echo "Run 'make install-dirty' once before using the quick dev installer." >&2
        exit 1
    fi
    if [[ ! -f "${outer_core_library}" ]]; then
        echo "ERROR: built QtWebEngineCore library missing: ${outer_core_library}" >&2
        exit 1
    fi

    echo "[+] Quick-installing rebuilt QtWebEngine runtime artifacts"
    mkdir -p "${install_dir}/lib" "${install_dir}/lib/qt6"
    install_qt_library_family libQt6WebEngineCore
    install_qt_library_family libQt6WebEngineWidgets
    if [[ -f "${webengine_build}/lib/qt6/QtWebEngineProcess" ]]; then
        install_regular_file_if_changed \
            "${webengine_build}/lib/qt6/QtWebEngineProcess" \
            "${install_dir}/lib/qt6/QtWebEngineProcess"
    fi
    write_launcher
    echo "[+] Quick QtWebEngine dev install complete"
}

partial_install_webengine() {
    require_build_tree
    if [[ ! -d "${install_dir}" ]]; then
        echo "ERROR: install tree missing: ${install_dir}" >&2
        echo "Run 'make install-dirty' once before using the partial dev installer." >&2
        exit 1
    fi

    echo "[+] Partial-installing qutebrowser-relevant QtWebEngine artifacts"
    echo "    install dir: ${install_dir}"
    rm -rf "${stage_dir}"
    mkdir -p "${stage_dir}"

    DESTDIR="${stage_dir}" ninja -C "${webengine_build}" -j"${jobs}" "${install_targets[@]}"

    staged_install="${stage_dir}${install_dir}"
    if [[ ! -d "${staged_install}" ]]; then
        echo "ERROR: staged install did not produce ${staged_install}" >&2
        exit 1
    fi

    # Merge only the staged component files into the existing complete install
    # tree. Do not --delete: the partial component install intentionally omits
    # QtPdf/QtWebEngineQuick/etc. that the full install already provided.
    rsync -a "${staged_install}/" "${install_dir}/"
    rm -rf "${stage_dir}"
    write_launcher
    echo "[+] Fast QtWebEngine dev install complete"
}

cache_status() {
    echo "[+] Compiler cache availability"
    if command -v sccache >/dev/null 2>&1; then
        echo "    sccache: $(command -v sccache)"
        sccache --version || true
    else
        echo "    sccache: not installed"
    fi
    if command -v ccache >/dev/null 2>&1; then
        echo "    ccache:  $(command -v ccache)"
        ccache --version | head -2 || true
        CCACHE_BASEDIR="${repo_dir}" ccache -s | head -25 || true
    else
        echo "    ccache:  not installed"
    fi

    if [[ -f "${webengine_build}/CMakeCache.txt" ]]; then
        echo "[+] CMake launcher configuration"
        grep -E '^(CMAKE_C_COMPILER_LAUNCHER|CMAKE_CXX_COMPILER_LAUNCHER|QT_USE_CCACHE):' \
            "${webengine_build}/CMakeCache.txt" || true
    fi

    if [[ -f "${core_build_dir}/args.gn" ]]; then
        echo "[+] Chromium GN wrapper"
        grep -E '^cc_wrapper' "${core_build_dir}/args.gn" || true
    fi
}

enable_cache() {
    wrapper=""
    if command -v sccache >/dev/null 2>&1; then
        wrapper="$(command -v sccache)"
    elif command -v ccache >/dev/null 2>&1; then
        wrapper="$(command -v ccache)"
    fi

    if [[ -z "${wrapper}" ]]; then
        cat >&2 <<EOF
ERROR: neither sccache nor ccache is installed.

On this Arch machine, install one with e.g.:
  yay -S ccache
or:
  sudo pacman -S ccache

Then run:
  make webengine-cache-enable

Note: enabling a compiler launcher changes compile command lines, so the first
build after enabling it can still be large. Subsequent rebuilds are where the
cache pays off.
EOF
        exit 1
    fi

    echo "[+] Reconfiguring QtWebEngine to use compiler launcher: ${wrapper}"
    echo "    WARNING: first build after this can be broad; later builds should cache."
    cmake -S "${repo_dir}/qtwebengine" -B "${webengine_build}" -GNinja \
        -DCMAKE_INSTALL_PREFIX="${install_dir}" \
        -DCMAKE_PREFIX_PATH="/usr/lib/cmake/Qt6" \
        -DCMAKE_C_COMPILER_LAUNCHER="${wrapper}" \
        -DCMAKE_CXX_COMPILER_LAUNCHER="${wrapper}" \
        -DQT_FEATURE_webengine_system_ffmpeg=ON \
        -DQT_FEATURE_webengine_system_icu=ON \
        -DQT_FEATURE_webengine_system_libevent=ON \
        -DQT_FEATURE_webengine_system_re2=ON \
        -DQT_FEATURE_webengine_proprietary_codecs=ON

    if [[ "${wrapper}" == *ccache ]]; then
        echo "[+] Recommended ccache settings for this checkout:"
        echo "    export CCACHE_BASEDIR=${repo_dir}"
        echo "    ccache -M 50G"
    fi
}

status() {
    echo "repo_dir=${repo_dir}"
    echo "webengine_build=${webengine_build}"
    echo "core_build_dir=${core_build_dir}"
    echo "install_dir=${install_dir}"
    echo "venv_dir=${venv_dir}"
    echo "launcher_path=${launcher_path}"
    echo "jobs=${jobs}"
    echo
    if [[ -f "${webengine_build}/build.ninja" ]]; then
        echo "[+] Outer Ninja: configured"
    else
        echo "[-] Outer Ninja: missing"
    fi
    if [[ -d "${core_build_dir}" ]]; then
        echo "[+] Inner Chromium Ninja: present"
    else
        echo "[-] Inner Chromium Ninja: missing"
    fi
    if [[ -d "${install_dir}" ]]; then
        echo "[+] Install tree: present"
    else
        echo "[-] Install tree: missing"
    fi
    echo
    cache_status
}

case "${cmd}" in
    build)
        build_webengine
        ;;
    install)
        quick_install_webengine
        ;;
    install-full)
        partial_install_webengine
        ;;
    build-install|dev)
        build_webengine
        quick_install_webengine
        ;;
    launcher)
        write_launcher
        ;;
    status)
        status
        ;;
    cache-status)
        cache_status
        ;;
    enable-cache)
        enable_cache
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        echo "ERROR: unknown command: ${cmd}" >&2
        usage >&2
        exit 2
        ;;
esac
