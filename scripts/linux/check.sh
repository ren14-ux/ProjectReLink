#!/usr/bin/env bash
# Checks the Linux build without a game file (what .github/workflows/linux.yml runs):
#
#   1. the renderer: nx2/build.py's game-free mode (RX_VK_ONLY=1) builds relink_vk.so, which must link, leave no symbol
#      unresolved (nx2/build.py's own check), load with RTLD_NOW as the game loads it, and export what host/rx_backend.c asks for;
#   2. the runtime: every host/*.c the Linux build compiles is compiled here, against the headers the lift generates
#      (a64_suyu.h, a64_fpx.h), and linked into one object: what it leaves undefined must be the lifted game's (hos_*, a64_*,
#      recomp_*, f_*) or the system's, never a Win32 call host/posix has no replacement for (a CamelCase name);
#   3. the tests that need no game.
#
#   scripts/linux/check.sh [--skip-vk] [--skip-host] [--skip-tests]
#
# Needs install-deps.sh to have run. The renderer takes ~4 minutes on 2 cores.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

say() { printf 'check: %s\n' "$*"; }
fail() { printf 'check: FAILED: %s\n' "$*" >&2; exit 1; }

do_vk=1 do_host=1 do_tests=1
for arg in "$@"; do
    case "$arg" in
        --skip-vk) do_vk=0 ;;
        --skip-host) do_host=0 ;;
        --skip-tests) do_tests=0 ;;
        -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown option: $arg" ;;
    esac
done

PY="$(relink_python)" || exit 1
cd "$RELINK_ROOT"
jobs="$(nproc)"

# ---- 1. the renderer ----
if [ "$do_vk" = 1 ]; then
    say "1/3 building the Vulkan back end (relink_vk.so) for Linux, no game needed"
    RX_VK_ONLY=1 "$PY" -I -u nx2/build.py --standalone --jobs "$jobs" > "$RELINK_HOME/check-vk.log" 2>&1 \
        || { tail -40 "$RELINK_HOME/check-vk.log" >&2; fail "the renderer did not build (full log: $RELINK_HOME/check-vk.log)"; }
    so="$RELINK_ROOT/work/sa/build/relink_vk.so"
    [ -f "$so" ] || fail "no relink_vk.so was made"
    "$PY" -I - "$so" <<'EOF' || fail "relink_vk.so does not load"
import ctypes, os, sys
lib = ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW | os.RTLD_LOCAL)
for name in ("mkwvk_init", "mkwvk_close_asked", "relink_program_set"):   # (host/rx_backend.c: GetProcAddress)
    getattr(lib, name)
print("check: relink_vk.so loads (RTLD_NOW) and exports mkwvk_init, mkwvk_close_asked, relink_program_set")
EOF
    say "    ok: $(du -h "$so" | cut -f1) $so"
fi

# ---- 2. the runtime ----
if [ "$do_host" = 1 ]; then
    say "2/3 compiling the host runtime for Linux"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    "$PY" -I - "$tmp" <<'EOF' || fail "could not generate the lift's headers"
import os, sys
root, tmp = os.getcwd(), sys.argv[1]
sys.path[:0] = [os.path.join(root, "nx2"), os.path.join(root, "third_party", "a64-kit", "tools"),
                os.path.join(root, "third_party", "a64-kit", "lift"), os.path.join(root, "third_party", "a64-kit", "hos")]
import suyu_rt_fixes
suyu_rt_fixes.install()
import a64_lift, a64_rtgen, rtgen
os.makedirs(os.path.join(tmp, "gen"))
a64_rtgen.write(os.path.join(tmp, "gen"))                                  # (a64_suyu.h: the lift writes it too)
open(os.path.join(tmp, "gen", "a64_fpx.h"), "w", newline="\n").write(a64_lift.fpx_header(set()))   # (no FP guards: a base header)
rtgen.write(os.path.join(tmp, "rt64"))
EOF
    ff="$(pkg-config --cflags libavcodec libavutil libswresample libswscale)"
    cflags=(-march=x86-64-v3 -ffp-contract=off -fPIC -D_GNU_SOURCE -I"$RELINK_ROOT/host/posix" -I"$RELINK_ROOT/host"
            -I"$tmp/rt64" -I"$tmp/gen" -Wno-everything -O1 -g0 -mcx16 -DA64_POLLING -DA64_WRITE_TRACK
            -I"$RELINK_ROOT/third_party/opus/include" -I"$RELINK_ROOT/third_party/stb")
    # (the same list nx2/build.py compiles on Linux: display.c, mf_av1.c and rx_nowgpu.c are the Windows build's, pgo_rt.c is for profiling)
    srcs=()
    for f in host/*.c; do
        case "$(basename "$f")" in display.c|mf_av1.c|rx_nowgpu.c|pgo_rt.c) continue ;; esac
        srcs+=("$f")
    done
    srcs+=(host/posix/win32_posix.c host/posix/display_sdl.c host/posix/linux_stubs.c host/posix/prof.c)
    pids=() failed=0
    for f in "${srcs[@]}"; do
        obj="$tmp/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
        ( clang "${cflags[@]}" $ff -c "$f" -o "$obj" || { echo "check: host compile failed: $f" >&2; exit 1; } ) &
        pids+=($!)
        if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}" || failed=1; pids=("${pids[@]:1}"); fi
    done
    for p in "${pids[@]}"; do wait "$p" || failed=1; done
    [ "$failed" = 0 ] || fail "the host runtime did not compile"
    ld.lld -r -o "$tmp/relocatable.o.new" "$tmp"/*.o || fail "the host objects do not link together"
    mv "$tmp/relocatable.o.new" "$tmp/relocatable.o"
    missing="$(nm -u "$tmp/relocatable.o" | awk '{print $2}' | grep -E '^[A-Z][A-Za-z0-9]+$' || true)"
    if [ -n "$missing" ]; then
        printf '%s\n' "$missing" >&2
        fail "the host calls Win32 functions that host/posix does not provide (listed above)"
    fi
    say "    ok: ${#srcs[@]} files compile and need no Win32 call the POSIX layer lacks"
fi

# ---- 3. the tests ----
if [ "$do_tests" = 1 ]; then
    say "3/3 running the game-free tests"
    status=0 count=0
    # (the tests in nx2/ that need neither the game, a GPU nor a Windows fixture. Left out: test_insn and test_leaf (the game's
    # code), test_ocarina_tex/test_prompt_tex/test_button_glyphs (its files), test_dll (a Windows DLL build), test_bc_storage_views
    # (a Windows GPU), test_pgo_rt_linux (needs LLVM 19+'s llvm-profdata for the raw profile format; Ubuntu 24.04 has 18) and
    # test_vk_linux_presentation (asserts the source text of fsr_start(), which no longer holds in upstream itself), and
    # test_import_windows, test_depth_sample_types, test_texture_phi_dominance, test_unsigned_image_types (stale fixtures, all
    # systems))
    plain=(test_cbuf_table_extent test_direct_ub_snapshot test_dump_ssbo_read test_exefs_mods test_group_marks
           test_mapped_memory_info test_mem_gate test_mesh_dynamic_state test_nup_codec test_nup_listing test_nup_paths
           test_nup_read_name test_nup_verify test_radv_submit_thread test_romfs_version test_step_rate test_timeline_modes
           test_version test_vk_accessors_once test_vk_failed_upscale_external)
    with_headers=(test_coop_spirv test_default_point_size test_draw_parameters test_mesh_first_types
                  test_spirv_struct_identity test_texture_init_barrier)
    vkh="$(ls -d "$RELINK_CPM"/vulkanheaders/*/include | head -1)"
    run_test() {
        local name="$1"; shift
        count=$((count + 1))
        if "$PY" -I -B "nx2/$name.py" "$@" > "$RELINK_HOME/check-test.log" 2>&1; then
            :
        else
            tail -15 "$RELINK_HOME/check-test.log" >&2
            say "    FAILED: $name"
            status=1
        fi
    }
    for t in "${plain[@]}"; do run_test "$t"; done
    for t in "${with_headers[@]}"; do
        if [ "$t" = test_spirv_struct_identity ]; then
            command -v spirv-val >/dev/null || { say "    skipped $t (install spirv-tools)"; continue; }
            run_test "$t" --headers "$vkh" --spirv-val "$(command -v spirv-val)"
        else
            run_test "$t" --headers "$vkh"
        fi
    done
    [ "$status" = 0 ] || fail "a test failed"
    say "    ok: $count tests"
fi

say "all checks passed"
