#!/bin/sh
# Build and run the variable-length dataset read overflow tests.
#
# Usage:
#   fortran/test/run_vl_string_overflow_tests.sh /path/to/hdf5-build
#
# The build must be a shared C + Fortran library produced from this
# source tree (HDF5_BUILD_FORTRAN=ON). AddressSanitizer is optional;
# the checks also fail on the returned lengths and on bytes written
# past a slot. When the library itself is instrumented, link with the
# same sanitizer.
set -eu

BUILD=${1:?usage: run_vl_string_overflow_tests.sh BUILD_DIR}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SRC="$ROOT/fortran/test"
OUT=${TMPDIR:-/tmp}/vl-string-overflow-tests
mkdir -p "$OUT"

INC="-I$BUILD/fortran -I$BUILD/src -I$ROOT/src -I$ROOT/src/H5FDsubfiling"
CC=${CC:-gcc}
FC=${FC:-gfortran}
# Leave SANITIZE empty unless the HDF5 build was itself compiled with the
# same sanitizer. An instrumented Fortran runtime aborts in h5open_f on a
# pre-existing flag-table write, before these reads run.
SAN=${SANITIZE:-}

echo "compiling C tests"
"$CC" $SAN $INC -o "$OUT/t_vl_string_overflow_c" "$SRC/t_vl_string_overflow.c" \
  -L"$BUILD/bin" -lhdf5_f90cstub -lhdf5 -Wl,-rpath,"$BUILD/bin"

echo "compiling Fortran tests"
"$FC" $SAN $INC -I"$BUILD/mod/shared" -o "$OUT/t_vl_string_overflow_f" \
  "$SRC/t_vl_string_overflow.F90" \
  -L"$BUILD/bin" -lhdf5_fortran -lhdf5_f90cstub -lhdf5 -Wl,-rpath,"$BUILD/bin"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1:halt_on_error=1}"

set +e
echo "running C tests"
"$OUT/t_vl_string_overflow_c"
c_rc=$?
echo "running Fortran tests"
f_rc=0
for case in fit exact past neighbor integer real; do
  echo "fortran $case"
  "$OUT/t_vl_string_overflow_f" "$case"
  one=$?
  if [ "$one" -ne 0 ]; then
    echo "fortran $case failed ($one)"
    if [ "$f_rc" -eq 0 ]; then
      f_rc=$one
    fi
  fi
done
set -e

echo "C exit $c_rc, Fortran exit $f_rc"
if [ "$c_rc" -ne 0 ]; then
  exit "$c_rc"
fi
exit "$f_rc"
