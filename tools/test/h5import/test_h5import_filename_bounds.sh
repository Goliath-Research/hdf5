#!/usr/bin/env bash
# Regression tests for unbounded copies of h5import input and configuration paths.
#
# datafile[] and configfile[] are 255-byte fields in the heap options record
# (MAX_PATH_NAME_LENGTH). The output path is stored in a 256-byte field and is
# rejected when its length is greater than 255. An input or configuration path
# must be rejected when it does not fit with its terminating NUL, that is when
# its length is greater than 254, and that rejection must happen before h5import
# creates, truncates, or replaces the output file.
#
# These tests fail while those two copies are unbounded. They pass once both
# copies reject an overlong path before using it.
#
# Usage:
#   tools/test/h5import/test_h5import_filename_bounds.sh /path/to/h5import
#
# The script leaves the process's library path alone. A binary that is not
# installed needs the directory containing libhdf5 to be on LD_LIBRARY_PATH,
# or an rpath that already points there.

set -u

if [[ $# -ne 1 || -z ${1:-} ]]; then
    echo "usage: $0 /path/to/h5import" >&2
    exit 2
fi

H5IMPORT=$1
if [[ ! -x $H5IMPORT ]]; then
    echo "h5import is not executable: $H5IMPORT" >&2
    exit 2
fi

ulimit -c 0

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
cd "$ROOT" || exit 2

FAILS=0
PASSES=0

chars() {
    local n=$1
    local ch=$2
    local s
    printf -v s '%*s' "$n" ''
    printf '%s' "${s// /$ch}"
}

write_config() {
    local path=$1
    cat >"$path" <<'EOF'
PATH /dataset
INPUT-CLASS TEXTIN
INPUT-SIZE 8
OUTPUT-CLASS IN
OUTPUT-SIZE 8
OUTPUT-BYTE-ORDER LE
OUTPUT-ARCHITECTURE STD
RANK 1
DIMENSION-SIZES 1
EOF
}

write_data() {
    local path=$1
    local value=$2
    printf '%s\n' "$value" >"$path"
}

# Run h5import. Prints the status (negative on a signal) and leaves
# stdout/stderr in "$ROOT/out.txt" and "$ROOT/err.txt".
run_import() {
    "$H5IMPORT" "$@" >"$ROOT/out.txt" 2>"$ROOT/err.txt"
    printf '%s' "$?"
}

record() {
    local ok=$1
    local name=$2
    local detail=$3
    if [[ $ok -eq 0 ]]; then
        PASSES=$((PASSES + 1))
        printf 'PASS %s\n' "$name"
    else
        FAILS=$((FAILS + 1))
        printf 'FAIL %s: %s\n' "$name" "$detail"
        if [[ -s $ROOT/err.txt ]]; then
            printf '     stderr: %s\n' "$(head -c 240 "$ROOT/err.txt" | tr '\n' ' ')"
        fi
    fi
}

# Overlong paths must be refused with a normal failure status and must not
# create or replace the output file.
expect_rejected() {
    local name=$1
    shift
    local out=sentinel.h5
    local marker='SENTINEL-DATA-12345'
    printf '%s' "$marker" >"$out"
    local status
    status=$(run_import "$@" -o "$out")
    local detail=''
    local ok=0
    if [[ $status -eq 0 ]]; then
        ok=1
        detail='exited 0'
    elif [[ $status -ge 128 ]]; then
        ok=1
        detail="killed by signal (status $status)"
    fi
    if ! cmp -s "$out" <(printf '%s' "$marker"); then
        ok=1
        detail="${detail:+$detail; }output file was created or modified"
    fi
    # A refusal that still tried to open the caller's path reports an open
    # error. Length rejection happens before that.
    if grep -q 'Unable to open' "$ROOT/err.txt"; then
        ok=1
        detail="${detail:+$detail; }opened a path instead of rejecting its length"
    fi
    record "$ok" "$name" "$detail"
}

expect_imported() {
    local name=$1
    shift
    local out=imported.h5
    rm -f "$out"
    local status
    status=$(run_import "$@" -o "$out")
    local ok=0
    local detail=''
    if [[ $status -ne 0 ]]; then
        ok=1
        detail="exit status $status"
    elif [[ ! -s $out ]] || ! cmp -s <(printf '\211HDF') <(head -c 4 "$out"); then
        ok=1
        detail='output is not an HDF5 file'
    fi
    record "$ok" "$name" "$detail"
}

CONF=import.conf
write_config "$CONF"
SHORT=short.txt
write_data "$SHORT" 4

# --- bounds that already fit -------------------------------------------------
# Lengths are the argument strings themselves. The working directory is the
# temporary root, so a 254-character relative name is a 254-character argument.

IN254=$(chars 254 'b')
write_data "$IN254" 4
expect_imported input_length_254_fits "$IN254" -d 1 -t TEXTIN -s 8

CFG254=$(chars 254 'c')
write_config "$CFG254"
expect_imported config_length_254_fits "$SHORT" -c "$CFG254"

OUT255=$(chars 255 'o')
rm -f "$OUT255"
status=$(run_import "$SHORT" -d 1 -t TEXTIN -s 8 -o "$OUT255")
ok=0
detail=''
if [[ $status -ne 0 ]]; then
    ok=1
    detail="exit status $status"
elif [[ ! -s $OUT255 ]]; then
    ok=1
    detail='255-character output path was not created'
fi
record "$ok" output_length_255_fits "$detail"

OUT256=$(chars 256 'p')
status=$(run_import "$SHORT" -d 1 -t TEXTIN -s 8 -o "$OUT256")
ok=0
detail=''
if [[ $status -eq 0 ]]; then
    ok=1
    detail='exited 0'
elif [[ $status -ge 128 ]]; then
    ok=1
    detail="killed by signal (status $status)"
elif [[ -e $OUT256 ]]; then
    ok=1
    detail='overlong output path was created'
fi
record "$ok" output_length_256_rejected "$detail"

# --- input path copies (h5import.c datafile strcpy) --------------------------

IN255=$(chars 255 'd')
write_data "$IN255" 7
expect_rejected input_length_255_rejected "$IN255" -d 1 -t TEXTIN -s 8

# A one-byte-longer name whose 254-byte prefix is a real import file. Truncating
# to the field size and continuing would import that prefix.
IN_PREFIX=$(chars 254 'e')
write_data "$IN_PREFIX" 5
expect_rejected input_length_255_not_truncated "${IN_PREFIX}X" -d 1 -t TEXTIN -s 8

# 255 bytes plus the following configuration name is one C string, because the
# NUL is written into configfile[] and then replaced by the configuration path.
# The file that exists only at that joined path must not be imported.
JOIN_DIR=$(chars 254 'f')
mkdir -p "$JOIN_DIR/cfgdir" cfgdir
write_data "$JOIN_DIR/cfgdir/settings.txt" 9
write_config cfgdir/settings.txt
expect_rejected input_length_255_does_not_join_config_path \
    "$JOIN_DIR/" -c cfgdir/settings.txt

# The same copy is used for every input file, not only the first.
expect_rejected second_input_length_255_rejected \
    "$SHORT" -d 1 -t TEXTIN -s 8 "$IN255" -d 1 -t TEXTIN -s 8

# Long enough to overwrite pointer members that the error path frees
# (sizeOfDimension and the pointers after it).
expect_rejected input_length_overwrites_pointers \
    "$(chars 6000 'g')" -d 1 -t TEXTIN -s 8

# The last accepted input slot is near the end of the heap allocation. A path
# longer than the bytes that remain must still be rejected, not copied.
args=()
for i in $(seq 0 27); do
    slot=slot$i.txt
    write_data "$slot" 1
    args+=("$slot" -d 1 -t TEXTIN -s 8)
done
args+=("$(chars 20000 'h')")
expect_rejected last_input_length_past_allocation "${args[@]}"

# --- configuration path copies (h5import.c configfile strcpy) ---------------

expect_rejected config_length_255_rejected "$SHORT" -c "$(chars 255 'i')"

CFG_PREFIX=$(chars 254 'j')
write_config "$CFG_PREFIX"
expect_rejected config_length_255_not_truncated "$SHORT" -c "${CFG_PREFIX}Z"

expect_rejected second_config_length_255_rejected \
    "$SHORT" -c "$CONF" "$SHORT" -c "$(chars 255 'k')"

expect_rejected config_length_overwrites_pointers \
    "$SHORT" -c "$(chars 6000 'm')"

args=()
for i in $(seq 0 27); do
    slot=cslot$i.txt
    write_data "$slot" 1
    args+=("$slot" -c "$CONF")
done
write_data clast.txt 1
args+=("clast.txt" -c "$(chars 20000 'n')")
expect_rejected last_config_length_past_allocation "${args[@]}"

printf '\n%d passed, %d failed\n' "$PASSES" "$FAILS"
if [[ $FAILS -ne 0 ]]; then
    exit 1
fi
exit 0
