#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${ROOT_DIR}/build"
DIST_DIR="${ROOT_DIR}/web/dist"
EMSDK_DIR=${EMSDK_DIR-}
EM_CACHE_DIR=${EM_CACHE:-${ROOT_DIR}/.emcache}
EM_CONFIG_FILE=${EM_CONFIG:-${ROOT_DIR}/.emscripten}

if [ -n "${EMSDK_DIR}" ] && [ -f "${EMSDK_DIR}/emsdk_env.sh" ]; then
    set +u
    # shellcheck disable=SC1090
    source "${EMSDK_DIR}/emsdk_env.sh" >/dev/null
    set -u
fi

mkdir -p "${BUILD_DIR}" "${DIST_DIR}"
mkdir -p "${EM_CACHE_DIR}"
find "${DIST_DIR}" -maxdepth 1 -type f -delete

export EM_CACHE="${EM_CACHE_DIR}"
export EM_CONFIG="${EM_CONFIG_FILE}"

if [ ! -f "${EM_CONFIG}" ]; then
    emcc --generate-config "${EM_CONFIG}" >/dev/null
fi

EMCMAKE=${EMCMAKE:-emcmake}

GENERATOR_SPECIFIED=0
for arg in "$@"; do
    case "${arg}" in
        -G|-G*)
            GENERATOR_SPECIFIED=1
            ;;
    esac
done

if [ "${GENERATOR_SPECIFIED}" -eq 0 ]; then
    if command -v ninja >/dev/null 2>&1; then
        set -- -G Ninja "$@"
    else
        set -- -G "Unix Makefiles" "$@"
    fi
fi

"${EMCMAKE}" cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "$@"
cmake --build "${BUILD_DIR}"
