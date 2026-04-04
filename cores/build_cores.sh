#!/usr/bin/env bash
# build_cores.sh – Download and compile upstream libretro cores for EmuC/PS5
#
# Usage:
#   ./build_cores.sh [nes|snes|gb|gba|md|all]
#
# Each core is cloned from GitHub and built as a standard libretro .so.
# The .so cannot be used on PS5 directly (no dynamic linker in our homebrew
# environment), so the script also explains the two paths forward:
#   1. Wrap the core logic using nes_core.c as a template → flat .bin
#   2. Use a full RetroArch build which handles dynamic core loading via
#      sceKernelLoadStartModule on PS5.
#
# Supported cores:
#   nes  – Nestopia UE   https://github.com/libretro/nestopia
#   snes – Snes9x        https://github.com/libretro/snes9x
#   gb   – Gambatte      https://github.com/libretro/gambatte-libretro
#   gba  – mGBA          https://github.com/libretro/mgba
#   md   – Genesis+GX    https://github.com/libretro/Genesis-Plus-GX
#
# Requires: git, gcc, make, objcopy  (apt install build-essential git)
# ---------------------------------------------------------------------------

set -euo pipefail

# ── Colours ────────────────────────────────────────────────────────────────
GREEN="\033[0;32m"
YELLOW="\033[1;33m"
RED="\033[0;31m"
RESET="\033[0m"

ok()   { echo -e "${GREEN}[OK]${RESET}    $*"; }
warn() { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
err()  { echo -e "${RED}[ERR]${RESET}   $*"; }
hdr()  { echo -e "\n${GREEN}=== $* ===${RESET}"; }

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/upstream"    # cloned repos land here
OUT_DIR="${SCRIPT_DIR}"             # .bin outputs go into cores/

mkdir -p "${SRC_DIR}"

# ── Dependency check ───────────────────────────────────────────────────────
check_deps() {
    local missing=()
    for cmd in git gcc make objcopy; do
        command -v "${cmd}" &>/dev/null || missing+=("${cmd}")
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        warn "Missing tools: ${missing[*]}"
        if command -v apt-get &>/dev/null; then
            echo "  Installing build dependencies via apt..."
            sudo apt-get install -y build-essential git
        else
            err "Please install: ${missing[*]}"
            exit 1
        fi
    fi
}

# ── Helper: clone or skip ──────────────────────────────────────────────────
# clone_repo <url> <dest-dir>
clone_repo() {
    local url="$1"
    local dest="$2"

    if [[ -d "${dest}/.git" ]]; then
        ok "Already cloned: $(basename "${dest}") – skipping git clone."
        return 0
    fi

    echo "  Cloning $(basename "${dest}") ..."
    git clone --depth=1 "${url}" "${dest}"
    ok "Cloned $(basename "${dest}")"
}

# ── Helper: explain PS5 .so limitation ────────────────────────────────────
explain_so_limitation() {
    local core_name="$1"
    local so_path="$2"

    echo ""
    warn "Standard libretro .so is NOT directly usable on PS5."
    echo "  Built: ${so_path}"
    echo ""
    echo "  The PS5 homebrew environment (our payload) does not ship a"
    echo "  dynamic linker, so dlopen() / ld.so cannot resolve the .so's"
    echo "  imports at runtime."
    echo ""
    echo "  Two paths to run ${core_name} on PS5:"
    echo ""
    echo "  PATH A – Flat binary core (recommended for this project):"
    echo "    Use cores/nes_core.c as a template:"
    echo "      1. Copy nes_core.c → ${core_name}_core.c"
    echo "      2. Replace the NES emulator calls with ${core_name} engine calls."
    echo "      3. Build with: make -C \"${SCRIPT_DIR}\" ${core_name}"
    echo "    The result is a flat .bin with a core_header at byte 0 that the"
    echo "    ES frontend maps into executable memory (no dynamic linker needed)."
    echo ""
    echo "  PATH B – RetroArch (handles core loading natively on PS5):"
    echo "    Build RetroArch for PS5 (see build_retroarch.sh when available)."
    echo "    RetroArch uses sceKernelLoadStartModule() to load cores as PRX"
    echo "    modules, which *is* supported by the PS5 kernel."
    echo ""
}

# ── Helper: attempt static flat-binary link ───────────────────────────────
# try_static_link <core_name> <build_dir> <so_glob>
# Returns 0 if a .bin was produced, 1 otherwise.
try_static_link() {
    local core_name="$1"
    local build_dir="$2"
    local so_glob="$3"
    local out="${OUT_DIR}/${core_name}_core_upstream.bin"

    # Find the built .so
    local so_path
    so_path="$(find "${build_dir}" -maxdepth 3 -name "${so_glob}" | head -n1 || true)"
    if [[ -z "${so_path}" ]]; then
        warn "No .so found matching ${so_glob} – skipping static link attempt."
        return 1
    fi

    ok "Found shared library: ${so_path}"

    # We cannot trivially convert an arbitrary .so to a flat PS5 binary
    # because it has relocations and external libc dependencies.
    # Report what was built and explain the limitation instead.
    explain_so_limitation "${core_name}" "${so_path}"
    return 1
}

# ── Core build functions ───────────────────────────────────────────────────

build_nes() {
    hdr "NES – Nestopia UE"
    local repo="${SRC_DIR}/nestopia"
    clone_repo "https://github.com/libretro/nestopia" "${repo}"

    echo "  Building Nestopia UE libretro core..."
    if make -C "${repo}/libretro" -f Makefile platform=unix -j"$(nproc)" 2>&1 | tail -5; then
        try_static_link "nes" "${repo}/libretro" "nestopia_libretro.so" || true
        ok "Nestopia UE build complete (see notes above)."
        BUILD_RESULTS+=("nes: built upstream .so – see PATH A/B notes above")
    else
        err "Nestopia UE build failed."
        BUILD_RESULTS+=("nes: BUILD FAILED")
        return 1
    fi
}

build_snes() {
    hdr "SNES – Snes9x"
    local repo="${SRC_DIR}/snes9x"
    clone_repo "https://github.com/libretro/snes9x" "${repo}"

    echo "  Building Snes9x libretro core..."
    if make -C "${repo}/libretro" -f Makefile -j"$(nproc)" 2>&1 | tail -5; then
        try_static_link "snes" "${repo}/libretro" "snes9x_libretro.so" || true
        ok "Snes9x build complete (see notes above)."
        BUILD_RESULTS+=("snes: built upstream .so – see PATH A/B notes above")
    else
        err "Snes9x build failed."
        BUILD_RESULTS+=("snes: BUILD FAILED")
        return 1
    fi
}

build_gb() {
    hdr "Game Boy/GBC – Gambatte"
    local repo="${SRC_DIR}/gambatte-libretro"
    clone_repo "https://github.com/libretro/gambatte-libretro" "${repo}"

    echo "  Building Gambatte libretro core..."
    if make -C "${repo}" -f Makefile -j"$(nproc)" 2>&1 | tail -5; then
        try_static_link "gb" "${repo}" "gambatte_libretro.so" || true
        ok "Gambatte build complete (see notes above)."
        BUILD_RESULTS+=("gb: built upstream .so – see PATH A/B notes above")
    else
        err "Gambatte build failed."
        BUILD_RESULTS+=("gb: BUILD FAILED")
        return 1
    fi
}

build_gba() {
    hdr "Game Boy Advance – mGBA"
    local repo="${SRC_DIR}/mgba"
    clone_repo "https://github.com/libretro/mgba" "${repo}"

    echo "  Building mGBA libretro core..."
    # mGBA uses CMake; fall back to the libretro Makefile wrapper if present.
    if [[ -f "${repo}/Makefile" ]]; then
        if make -C "${repo}" -f Makefile -j"$(nproc)" 2>&1 | tail -5; then
            try_static_link "gba" "${repo}" "mgba_libretro.so" || true
            ok "mGBA build complete (see notes above)."
            BUILD_RESULTS+=("gba: built upstream .so – see PATH A/B notes above")
            return 0
        fi
    fi
    # CMake path
    if command -v cmake &>/dev/null; then
        mkdir -p "${repo}/build_libretro"
        if cmake -S "${repo}" -B "${repo}/build_libretro" \
                 -DBUILD_LIBRETRO=ON -DCMAKE_BUILD_TYPE=Release \
                 -DCMAKE_INSTALL_PREFIX=/tmp/mgba_install \
             && cmake --build "${repo}/build_libretro" --parallel "$(nproc)" 2>&1 | tail -5; then
            try_static_link "gba" "${repo}/build_libretro" "mgba_libretro.so" || true
            ok "mGBA (CMake) build complete (see notes above)."
            BUILD_RESULTS+=("gba: built upstream .so – see PATH A/B notes above")
            return 0
        fi
    fi
    err "mGBA build failed (tried Makefile and CMake)."
    BUILD_RESULTS+=("gba: BUILD FAILED")
    return 1
}

build_md() {
    hdr "Mega Drive – Genesis Plus GX"
    local repo="${SRC_DIR}/Genesis-Plus-GX"
    clone_repo "https://github.com/libretro/Genesis-Plus-GX" "${repo}"

    echo "  Building Genesis Plus GX libretro core..."
    if make -C "${repo}" -f Makefile.libretro -j"$(nproc)" 2>&1 | tail -5; then
        try_static_link "md" "${repo}" "genesis_plus_gx_libretro.so" || true
        ok "Genesis Plus GX build complete (see notes above)."
        BUILD_RESULTS+=("md: built upstream .so – see PATH A/B notes above")
    else
        err "Genesis Plus GX build failed."
        BUILD_RESULTS+=("md: BUILD FAILED")
        return 1
    fi
}

# ── Summary ────────────────────────────────────────────────────────────────

print_summary() {
    hdr "Build Summary"
    for entry in "${BUILD_RESULTS[@]}"; do
        echo "  ${entry}"
    done
    echo ""
    echo "  PS5-ready flat binaries (cores/*.bin) already built by EmuC Makefile:"
    if ls "${OUT_DIR}"/*.bin &>/dev/null 2>&1; then
        for f in "${OUT_DIR}"/*.bin; do
            echo "    $(basename "${f}")  ($( wc -c < "${f}") bytes)"
        done
    else
        echo "    (none yet – run: make -C cores nes)"
    fi
    echo ""
    echo "  To build the NES flat binary core: make -C ${SCRIPT_DIR} nes"
    echo "  To build all native cores:         $0 all"
}

# ── Entry point ────────────────────────────────────────────────────────────

declare -a BUILD_RESULTS=()

main() {
    local target="${1:-all}"

    check_deps

    case "${target}" in
        nes)  build_nes ;;
        snes) build_snes ;;
        gb)   build_gb ;;
        gba)  build_gba ;;
        md)   build_md ;;
        all)
            build_nes  || true
            build_snes || true
            build_gb   || true
            build_gba  || true
            build_md   || true
            ;;
        *)
            err "Unknown target: ${target}"
            echo "Usage: $0 [nes|snes|gb|gba|md|all]"
            exit 1
            ;;
    esac

    print_summary
}

main "$@"
