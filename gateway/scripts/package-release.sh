#!/usr/bin/env bash
#
# Packages a built karshipta_gateway/karshipta_herald binary into a portable,
# self-contained archive: the binary plus every non-system runtime dependency
# it needs, relinked to look for them next to itself instead of wherever this
# build machine happened to have them installed (Homebrew/apt/vendored
# third_party paths that do not exist on a customer machine). Shared by every
# flavor/platform leg of .github/workflows/release-gateway.yml, and runnable
# by hand the same way for local testing.
#
# Usage: package-release.sh <path-to-built-binary> <output-name-without-extension>
# Produces <output-name>.tar.gz (+.sha256) on all three platforms, including
# Windows (run from Git Bash, which ships on GitHub's windows-latest runner -
# its tar handles gzip identically to macOS/Linux), in the current directory.
set -euo pipefail

# Copies every non-system dylib $1 (transitively) depends on into $2, and
# relinks $1 plus every copied dylib to reference each other via
# @executable_path/lib/<name> instead of wherever this build machine had
# them installed. Deliberately hand-rolled instead of using dylibbundler:
# verified directly that dylibbundler hangs indefinitely (8+ minutes, zero
# files copied) against this binary's ~90-dylib transitive dependency tree
# (MAVSDK + protobuf/abseil + relayly's own deps, the last one referenced
# via @rpath rather than an absolute path). This does one BFS pass over
# `otool -L` output, copies and relinks every dependency exactly once, then
# a single codesign pass at the end - ~15-20s for the same tree. No
# associative arrays (declare -A): this repo's contributors may only have
# macOS's system bash (3.2, no bash-4+ features) on PATH, and this must run
# identically there.
bundle_macos_dylibs() {
    local binary="$1"
    local lib_dir="$2"
    mkdir -p "$lib_dir"

    # @rpath must resolve against the LC_RPATH entries of whichever file is
    # currently being scanned, not the main binary's - found only by an
    # actual CI run: Homebrew LLVM's own libc++.1.dylib depends on
    # @rpath/libunwind.1.dylib via its own rpath (@loader_path/../unwind,
    # relative to libc++.1.dylib's own directory), completely unrelated to
    # the main binary's rpath list. Resolving every @rpath against only the
    # binary's own rpaths silently dropped libunwind, which only failed at
    # runtime ("Library not loaded"), not at bundle time - this machine's
    # own already-installed Homebrew LLVM never exercised the gap locally
    # since the dylib was already sitting right where dyld expected it.
    rpaths_for() {
        local file="$1"
        local file_dir
        file_dir="$(cd "$(dirname "$file")" && pwd)"
        otool -l "$file" | awk '/LC_RPATH/{getline; getline; print $2}' | while IFS= read -r rp; do
            rp="${rp/@loader_path/$file_dir}"
            rp="${rp/@executable_path/$file_dir}"
            printf '%s\n' "$rp"
        done
    }

    resolve_dep() {
        local ref="$1"
        local current_file="$2"
        if [[ "$ref" == @rpath/* ]]; then
            local name="${ref#@rpath/}"
            local rp
            while IFS= read -r rp; do
                [[ -z "$rp" ]] && continue
                if [[ -f "$rp/$name" ]]; then
                    printf '%s\n' "$rp/$name"
                    return 0
                fi
            done < <(rpaths_for "$current_file")
            return 1
        elif [[ "$ref" == @loader_path/* ]]; then
            local file_dir
            file_dir="$(cd "$(dirname "$current_file")" && pwd)"
            printf '%s\n' "$file_dir/${ref#@loader_path/}"
        else
            printf '%s\n' "$ref"
        fi
    }

    # BFS queue holds SOURCE paths (original Homebrew/build-tree locations),
    # never the copies in $lib_dir: rpath resolution must always happen
    # against a file's real, original location (see rpaths_for above) - a
    # copy's rpath entries, interpreted relative to its new location inside
    # $lib_dir, resolve to nonsense. `copied` tracks the actual copies
    # separately, only for the relink/codesign passes below.
    local seen_file
    seen_file="$(mktemp)"
    local copied=()
    local queue=("$binary")

    while [[ ${#queue[@]} -gt 0 ]]; do
        local current="${queue[0]}"
        queue=("${queue[@]:1}")

        local dep
        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue
            case "$dep" in
                /usr/lib/* | /System/*) continue ;;
            esac
            local resolved
            resolved="$(resolve_dep "$dep" "$current")" || continue
            local name
            name="$(basename "$resolved")"
            grep -qxF "$name" "$seen_file" 2>/dev/null && continue
            echo "$name" >>"$seen_file"
            local dest="$lib_dir/$name"
            cp "$resolved" "$dest"
            chmod u+w "$dest"
            copied+=("$dest")
            queue+=("$resolved")
        done < <(otool -L "$current" | tail -n +2 | awk '{print $1}')
    done
    rm -f "$seen_file"

    echo "bundle_macos_dylibs: copied ${#copied[@]} dylibs" >&2

    # Rewriting references only ever needs the dependency's basename (the
    # BFS pass above already guarantees a same-named copy exists in
    # $lib_dir for every non-system dependency) - no path resolution needed
    # here, deliberately not reusing resolve_dep: it would face the exact
    # copy-location bug the comment above warns about, since $target here is
    # the copy in $lib_dir, not the dependency's original location.
    local target
    for target in "$binary" "${copied[@]}"; do
        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue
            case "$dep" in
                /usr/lib/* | /System/*) continue ;;
            esac
            local name
            name="$(basename "$dep")"
            install_name_tool -change "$dep" "@executable_path/lib/$name" "$target" 2>/dev/null || true
        done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
    done

    for target in "${copied[@]}"; do
        install_name_tool -id "@executable_path/lib/$(basename "$target")" "$target" 2>/dev/null || true
    done

    codesign --force --sign - "$binary"
    for target in "${copied[@]}"; do
        codesign --force --sign - "$target"
    done
}

if [[ $# -ne 2 ]]; then
    echo "usage: package-release.sh <binary-path> <output-name>" >&2
    exit 1
fi

binary_path="$1"
out_name="$2"

if [[ ! -f "$binary_path" ]]; then
    echo "package-release.sh: no such binary: $binary_path" >&2
    exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

binary_name="$(basename "$binary_path")"
stage_dir="$work_dir/$out_name"
mkdir -p "$stage_dir"
cp "$binary_path" "$stage_dir/$binary_name"

uname_s="$(uname -s)"
case "$uname_s" in
    Darwin)
        # Hand-rolled instead of dylibbundler: verified directly that
        # dylibbundler hangs indefinitely (8+ minutes, zero files copied)
        # against this binary's ~90-dylib transitive dependency tree
        # (MAVSDK + protobuf/abseil + relayly's own deps). This does one BFS
        # pass over `otool -L` output, copying and relinking every
        # non-system dependency exactly once, then a single codesign pass -
        # takes ~15-20s for the same tree. Verified locally end to end: the
        # bundled binary, moved somewhere with zero Homebrew paths on the
        # system, loads all ~90 relinked dylibs (confirmed via
        # DYLD_PRINT_LIBRARIES=1) and logs its normal startup lines
        # identically to the unbundled original (allow ~15-20s before the
        # first log line - macOS's one-time Gatekeeper/AMFI verification of
        # a freshly re-signed binary+dylib set, not a bug).
        mkdir -p "$stage_dir/lib"
        bundle_macos_dylibs "$stage_dir/$binary_name" "$stage_dir/lib"
        ;;
    Linux)
        mkdir -p "$stage_dir/lib"
        # Copy every non-baseline shared library dependency next to the
        # binary, then point the binary (and each copied library, since they
        # can depend on each other, e.g. relayly on ixwebsocket) at that
        # directory via a relative rpath. Deliberately an allowlist of the
        # true OS/toolchain ABI baseline (the dynamic linker, glibc's own
        # pieces, libgcc_s), not a path-prefix blocklist: verified directly
        # in a clean Ubuntu 22.04 container that MAVSDK/spdlog/protobuf all
        # resolve under /usr/lib/x86_64-linux-gnu/ too, purely because
        # that's where apt happened to install them as *build* dependencies,
        # not because they're guaranteed present on any Linux machine this
        # binary might run on. Excluding by path prefix silently left every
        # one of those three unbundled - only caught by actually building
        # and running the packaged result in a fresh container.
        baseline_libs="ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libgcc_s.so.1"
        ldd "$binary_path" | awk '{print $3}' | while read -r dep; do
            [[ -z "$dep" || ! -f "$dep" ]] && continue
            dep_name="$(basename "$dep")"
            case " $baseline_libs " in
                *" $dep_name "*) continue ;;
            esac
            cp -n "$dep" "$stage_dir/lib/"
        done
        patchelf --set-rpath '$ORIGIN/lib' "$stage_dir/$binary_name"
        for lib in "$stage_dir"/lib/*; do
            [[ -f "$lib" ]] && patchelf --set-rpath '$ORIGIN' "$lib"
        done
        ;;
    MINGW* | MSYS* | CYGWIN*)
        # Windows: gateway/src/CMakeLists.txt's own post-build step
        # ($<TARGET_RUNTIME_DLLS:...>) already copied every needed DLL next
        # to the .exe - nothing to relink, just stage what is already there.
        binary_dir="$(dirname "$binary_path")"
        find "$binary_dir" -maxdepth 1 -iname '*.dll' -exec cp {} "$stage_dir/" \;
        ;;
    *)
        echo "package-release.sh: unsupported platform: $uname_s" >&2
        exit 1
        ;;
esac

# .tar.gz uniformly, including Windows: `zip` is not guaranteed on PATH in
# Git Bash (confirmed missing on GitHub's windows-latest runner - the
# original reason this was platform-branched at all), whereas Windows 10
# 1803+ ships a real tar.exe (bsdtar, in System32) with full gzip support,
# and Git Bash's own tar handles it identically either way. One code path
# for all three platforms beats one more platform-specific branch to break.
archive="$(pwd)/$out_name.tar.gz"
tar -czf "$archive" -C "$work_dir" "$out_name"

if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$archive" >"$archive.sha256"
else
    shasum -a 256 "$archive" >"$archive.sha256"
fi

echo "Packaged: $archive"
echo "Checksum: $archive.sha256"
