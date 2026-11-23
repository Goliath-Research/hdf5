/*
 * Spec for the h5repack filter-count overflow.
 *
 * h5repack stores dataset filters in a six-element array (H5_REPACK_MAX_NFILTERS).
 * When it preserves filters already stored on a dataset, aux_copy_obj copies
 * every filter reported by the file. The file format allows 32. These tests
 * build such files through the library API and run h5repack under a preload
 * that puts an inaccessible page right after each six-slot record.
 *
 * Cases that preserve seven or more filters must not be killed by a signal.
 * They fail while the overflow is present. A fix may reject the file or keep
 * the filters, as long as that copy stays inside the allocation and a repack
 * of six filters still returns the original values.
 *
 * Build and run (from a configured HDF5 build tree that already built h5repack):
 *
 *   cmake --build <build> --target h5repack h5repack_filter_overflow h5repack_filter_overflow_guard
 *   <build>/bin/h5repack_filter_overflow \
 *       <build>/bin/h5repack \
 *       <build>/bin/libh5repack_filter_overflow_guard.so \
 *       /tmp/h5repack-filter-overflow
 */
#define _GNU_SOURCE
#include "hdf5.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NELMTS 32
/* Must match GUARD_CD_NELMTS in h5repack_filter_overflow_guard.c. */
#define CLIENT_VALUES 80

enum kind { KIND_NAMED, KIND_NATIVE, KIND_VLEN, KIND_WITH_OTHER };

struct expect {
    const char *name;
    enum kind   kind;
    int         nfilters;
    int         must_succeed; /* nonzero: exit 0 and original values; zero: any clean exit */
    const char *arg0;         /* optional h5repack argument, or NULL */
    const char *arg1;
};

static const struct expect cases[] = {
    {"preserve_six_named", KIND_NAMED, 6, 1, NULL, NULL},
    {"preserve_six_layout", KIND_NATIVE, 6, 1, "-l", "CHUNK=16"},
    {"plain_native_seven_uses_object_copy", KIND_NATIVE, 7, 1, NULL, NULL},
    {"preserve_seven_named", KIND_NAMED, 7, 0, NULL, NULL},
    {"preserve_seven_vlen", KIND_VLEN, 7, 0, NULL, NULL},
    {"preserve_seven_layout", KIND_NATIVE, 7, 0, "-l", "CHUNK=16"},
    {"preserve_seven_when_other_object_filtered", KIND_WITH_OTHER, 7, 0, "-f", "other:FLET"},
};

static void
fail_hdf5(const char *what)
{
    fprintf(stderr, "HDF5 call failed: %s\n", what);
    H5Eprint2(H5E_DEFAULT, stderr);
}

static int
add_filters(hid_t dcpl, int nfilters)
{
    unsigned cd[CLIENT_VALUES];
    int      i, f;

    for (i = 0; i < CLIENT_VALUES; i++)
        cd[i] = 0xA1000000u + (unsigned)i;

    for (f = 0; f < nfilters; f++) {
        if (H5Pset_filter(dcpl, H5Z_FILTER_FLETCHER32, H5Z_FLAG_MANDATORY, CLIENT_VALUES, cd) < 0) {
            fail_hdf5("H5Pset_filter");
            return -1;
        }
    }
    return 0;
}

static int
write_ints(hid_t dset, const int *values)
{
    if (H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) < 0) {
        fail_hdf5("H5Dwrite");
        return -1;
    }
    return 0;
}

static int
create_input(const char *path, enum kind kind, int nfilters)
{
    hid_t   file    = H5I_INVALID_HID;
    hid_t   type    = H5I_INVALID_HID;
    hid_t   space   = H5I_INVALID_HID;
    hid_t   dcpl    = H5I_INVALID_HID;
    hid_t   dset    = H5I_INVALID_HID;
    hid_t   other   = H5I_INVALID_HID;
    hsize_t dims[1] = {NELMTS};
    hsize_t chunk[1] = {NELMTS};
    int     values[NELMTS];
    int     i;
    int     rc = -1;

    for (i = 0; i < NELMTS; i++)
        values[i] = 1000 + i;

    file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file < 0) {
        fail_hdf5("H5Fcreate");
        goto done;
    }

    if (kind == KIND_NAMED) {
        type = H5Tcopy(H5T_STD_I32LE);
        if (type < 0 || H5Tcommit2(file, "i32", type, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) < 0) {
            fail_hdf5("commit i32");
            goto done;
        }
    }
    else if (kind == KIND_VLEN) {
        type = H5Tvlen_create(H5T_NATIVE_INT);
        if (type < 0) {
            fail_hdf5("H5Tvlen_create");
            goto done;
        }
    }
    else {
        type = H5Tcopy(H5T_NATIVE_INT);
        if (type < 0) {
            fail_hdf5("H5Tcopy");
            goto done;
        }
    }

    space = H5Screate_simple(1, dims, NULL);
    dcpl  = H5Pcreate(H5P_DATASET_CREATE);
    if (space < 0 || dcpl < 0) {
        fail_hdf5("dataspace or dcpl");
        goto done;
    }
    if (H5Pset_chunk(dcpl, 1, chunk) < 0) {
        fail_hdf5("H5Pset_chunk");
        goto done;
    }
    if (add_filters(dcpl, nfilters) < 0)
        goto done;

    dset = H5Dcreate2(file, "data", type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (dset < 0) {
        fail_hdf5("H5Dcreate2 data");
        goto done;
    }

    if (kind == KIND_VLEN) {
        hvl_t cells[NELMTS];
        int   one[NELMTS];

        for (i = 0; i < NELMTS; i++) {
            one[i]      = values[i];
            cells[i].len = 1;
            cells[i].p   = &one[i];
        }
        if (H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, cells) < 0) {
            fail_hdf5("H5Dwrite vlen");
            goto done;
        }
    }
    else if (write_ints(dset, values) < 0) {
        goto done;
    }

    if (kind == KIND_WITH_OTHER) {
        other = H5Dcreate2(file, "other", H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (other < 0 || write_ints(other, values) < 0) {
            fail_hdf5("other dataset");
            goto done;
        }
    }

    rc = 0;

done:
    if (other >= 0)
        H5Dclose(other);
    if (dset >= 0)
        H5Dclose(dset);
    if (dcpl >= 0)
        H5Pclose(dcpl);
    if (space >= 0)
        H5Sclose(space);
    if (type >= 0)
        H5Tclose(type);
    if (file >= 0)
        H5Fclose(file);
    return rc;
}

static int
read_back_ints(const char *path, const int *expect)
{
    hid_t file = H5I_INVALID_HID;
    hid_t dset = H5I_INVALID_HID;
    int   got[NELMTS];
    int   rc = -1;

    file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
        fail_hdf5("H5Fopen output");
        goto done;
    }
    dset = H5Dopen2(file, "data", H5P_DEFAULT);
    if (dset < 0) {
        fail_hdf5("H5Dopen2 output");
        goto done;
    }
    if (H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) < 0) {
        fail_hdf5("H5Dread output");
        goto done;
    }
    if (memcmp(got, expect, sizeof(got)) != 0) {
        fprintf(stderr, "output values differ from the input\n");
        goto done;
    }
    rc = 0;

done:
    if (dset >= 0)
        H5Dclose(dset);
    if (file >= 0)
        H5Fclose(file);
    return rc;
}

static int
run_h5repack(const char *h5repack, const char *guard, const char *marker, const char *infile,
             const char *outfile, const char *arg0, const char *arg1, int *code, int *signal)
{
    pid_t pid;

    *code   = -1;
    *signal = 0;
    pid     = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        const char *argv[8];
        int         argc = 0;

        argv[argc++] = h5repack;
        if (arg0 != NULL) {
            argv[argc++] = arg0;
            argv[argc++] = arg1;
        }
        argv[argc++] = infile;
        argv[argc++] = outfile;
        argv[argc]   = NULL;

        if (setenv("LD_PRELOAD", guard, 1) != 0 || setenv("GUARD_MARKER", marker, 1) != 0) {
            perror("setenv");
            _exit(127);
        }
        /*
         * The guard is loaded ahead of the sanitizer runtime when h5repack
         * itself was built with one. Do not treat that order as fatal, and
         * do not turn a sanitizer report into a successful exit.
         */
        setenv("ASAN_OPTIONS",
               "detect_leaks=0:halt_on_error=1:abort_on_error=1:verify_asan_link_order=0", 1);
        execv(h5repack, (char *const *)argv);
        perror("execv");
        _exit(127);
    }

    {
        int status = 0;

        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return -1;
        }
        if (WIFSIGNALED(status))
            *signal = WTERMSIG(status);
        else if (WIFEXITED(status))
            *code = WEXITSTATUS(status);
        else
            *signal = -1;
    }
    return 0;
}

static int
marker_ok(const char *path)
{
    FILE *in = fopen(path, "r");
    char  buf[64];

    if (in == NULL)
        return 0;
    if (fgets(buf, (int)sizeof(buf), in) == NULL) {
        fclose(in);
        return 0;
    }
    fclose(in);
    return strncmp(buf, "pack_info_t ", 12) == 0;
}

int
main(int argc, char **argv)
{
    const char *h5repack;
    const char *guard;
    const char *workdir;
    int         values[NELMTS];
    int         failed = 0;
    size_t      c;
    int         i;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <h5repack> <guard-library> <workdir>\n", argv[0]);
        return 2;
    }
    h5repack = argv[1];
    guard    = argv[2];
    workdir  = argv[3];

    if (mkdir(workdir, 0700) != 0 && errno != EEXIST) {
        perror(workdir);
        return 1;
    }

    for (i = 0; i < NELMTS; i++)
        values[i] = 1000 + i;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const struct expect *spec = &cases[c];
        char                  inpath[512];
        char                  outpath[512];
        char                  marker[512];
        int                   code   = -1;
        int                   signal = 0;

        snprintf(inpath, sizeof(inpath), "%s/%s-in.h5", workdir, spec->name);
        snprintf(outpath, sizeof(outpath), "%s/%s-out.h5", workdir, spec->name);
        snprintf(marker, sizeof(marker), "%s/%s-guard", workdir, spec->name);
        unlink(outpath);
        unlink(marker);

        if (create_input(inpath, spec->kind, spec->nfilters) != 0) {
            fprintf(stderr, "FAIL %s: could not build the input file\n", spec->name);
            failed = 1;
            continue;
        }
        if (run_h5repack(h5repack, guard, marker, inpath, outpath, spec->arg0, spec->arg1, &code,
                         &signal) != 0) {
            fprintf(stderr, "FAIL %s: could not run h5repack\n", spec->name);
            failed = 1;
            continue;
        }
        if (!marker_ok(marker)) {
            fprintf(stderr, "FAIL %s: guard library did not load\n", spec->name);
            failed = 1;
            continue;
        }
        if (signal != 0) {
            fprintf(stderr, "FAIL %s: h5repack died with signal %d\n", spec->name, signal);
            failed = 1;
            continue;
        }
        if (spec->must_succeed) {
            if (code != 0) {
                fprintf(stderr, "FAIL %s: h5repack exited %d\n", spec->name, code);
                failed = 1;
                continue;
            }
            if (spec->kind != KIND_VLEN && read_back_ints(outpath, values) != 0) {
                fprintf(stderr, "FAIL %s: repacked values\n", spec->name);
                failed = 1;
                continue;
            }
        }
        fprintf(stderr, "PASS %s (exit %d)\n", spec->name, code);
    }

    return failed ? 1 : 0;
}
