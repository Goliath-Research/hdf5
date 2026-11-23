/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the LICENSE file, which can be found at the root of the source code       *
 * distribution tree, or in https://www.hdfgroup.org/licenses.               *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Spec for the object-reference read in h5repack_refs.c.
 *
 * do_copy_refobjs() sizes the object-reference read buffer with
 * malloc((unsigned)(nelmts * msize)) and then H5Dread()s every element.
 * On a 64-bit build that cast drops the high half of a byte count above
 * 2^32. A chunked dataset whose chunks were never allocated stays small,
 * and the read fills the caller buffer for every element.
 *
 * These tests fail while that overflow is reachable and pass once repack
 * rejects the dataset before any truncated allocation is used. A signal
 * (including an allocator or sanitizer abort) is a failure. A clean
 * non-zero status from h5repack(), with no signal, is the required outcome
 * for the overflowing dataset. A small dataset of the same type must still
 * repack.
 *
 * The same unsigned product is used for attribute reference reads in
 * copy_refs_attr() (object references, dataset-region references, vlen
 * references, and reference fields of a compound). Those are not exercised
 * here: opening the attribute recomputes the payload size from the
 * dataspace and requires that many bytes to be stored, so a wrapping
 * count is a multi-gigabyte attribute rather than the unallocated-chunk
 * dataset this path reads. Dataset-region references in do_copy_refobjs()
 * allocate with size_t, which does not truncate on this platform.
 */

#include "h5repack.h"
#include "h5tools.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DSET_NAME     "refs"
#define SMALL_NELMTS  4
#define CHUNK_NELMTS  2
#define MAX_FILE_SIZE (1024 * 1024)
#define CHILD_TIMEOUT 60

enum {
    CHILD_OK = 0,
    CHILD_FAIL = 1
};

static int
native_ref_size(size_t *ref_size)
{
    hid_t  native = H5I_INVALID_HID;
    size_t size;

    if (H5open() < 0)
        return -1;
    if ((native = H5Tget_native_type(H5T_STD_REF_OBJ, H5T_DIR_DEFAULT)) < 0)
        return -1;
    size = H5Tget_size(native);
    H5Tclose(native);
    if (size == 0)
        return -1;
    *ref_size = size;
    return 0;
}

/* Smallest element count whose byte product does not fit in unsigned. */
static int
overflowing_nelmts(size_t ref_size, hsize_t *nelmts_out)
{
    hsize_t nelmts;
    hsize_t bytes;

    if (ref_size == 0 || ref_size > UINT32_MAX)
        return -1;

    nelmts = ((hsize_t)UINT32_MAX / (hsize_t)ref_size) + 1;
    bytes  = nelmts * (hsize_t)ref_size;
    /* (unsigned)(2^32) is 0, and malloc(0) can fail before the read. */
    if ((unsigned)bytes == 0) {
        nelmts++;
        bytes = nelmts * (hsize_t)ref_size;
    }
    if (bytes <= (hsize_t)UINT32_MAX)
        return -1;
    if ((unsigned)bytes == 0 || (unsigned)bytes >= ref_size * 2)
        return -1;

    *nelmts_out = nelmts;
    return 0;
}

static int
write_ref_dataset(const char *path, hsize_t nelmts)
{
    hid_t   fid      = H5I_INVALID_HID;
    hid_t   space    = H5I_INVALID_HID;
    hid_t   dcpl     = H5I_INVALID_HID;
    hid_t   dset     = H5I_INVALID_HID;
    hsize_t dims[1]  = {nelmts};
    hsize_t chunk[1] = {CHUNK_NELMTS};
    H5D_space_status_t status;
    struct stat        st;
    int                ret = CHILD_FAIL;

    if (nelmts < CHUNK_NELMTS) {
        fprintf(stderr, "element count %llu is smaller than the chunk\n", (unsigned long long)nelmts);
        return CHILD_FAIL;
    }
    if (H5open() < 0) {
        fprintf(stderr, "H5open failed\n");
        return CHILD_FAIL;
    }

    if ((fid = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT)) < 0) {
        fprintf(stderr, "H5Fcreate failed for %s\n", path);
        goto done;
    }
    if ((space = H5Screate_simple(1, dims, NULL)) < 0) {
        fprintf(stderr, "H5Screate_simple failed\n");
        goto done;
    }
    if ((dcpl = H5Pcreate(H5P_DATASET_CREATE)) < 0) {
        fprintf(stderr, "H5Pcreate failed\n");
        goto done;
    }
    if (H5Pset_chunk(dcpl, 1, chunk) < 0) {
        fprintf(stderr, "H5Pset_chunk failed\n");
        goto done;
    }
    if ((dset = H5Dcreate2(fid, DSET_NAME, H5T_STD_REF_OBJ, space, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
        fprintf(stderr, "H5Dcreate2 failed\n");
        goto done;
    }
    if (H5Dget_space_status(dset, &status) < 0) {
        fprintf(stderr, "H5Dget_space_status failed\n");
        goto done;
    }
    if (status != H5D_SPACE_STATUS_NOT_ALLOCATED) {
        fprintf(stderr, "chunks were allocated; the read would not take the fill path\n");
        goto done;
    }

    H5Dclose(dset);
    dset = H5I_INVALID_HID;
    H5Pclose(dcpl);
    dcpl = H5I_INVALID_HID;
    H5Sclose(space);
    space = H5I_INVALID_HID;
    H5Fclose(fid);
    fid = H5I_INVALID_HID;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "stat failed: %s\n", strerror(errno));
        goto done;
    }
    if ((unsigned long long)st.st_size > MAX_FILE_SIZE) {
        fprintf(stderr, "input file is %llu bytes; chunks were stored\n",
                (unsigned long long)st.st_size);
        goto done;
    }

    fprintf(stderr, "wrote %s nelmts=%llu bytes=%llu\n", path, (unsigned long long)nelmts,
            (unsigned long long)st.st_size);
    ret = CHILD_OK;

done:
    if (dset >= 0)
        H5Dclose(dset);
    if (dcpl >= 0)
        H5Pclose(dcpl);
    if (space >= 0)
        H5Sclose(space);
    if (fid >= 0)
        H5Fclose(fid);
    return ret;
}

static int
run_repack(const char *infile, const char *outfile)
{
    pack_opt_t options;
    int        rc;

    h5tools_init();
    if (h5repack_init(&options, 0, false) < 0) {
        fprintf(stderr, "h5repack_init failed\n");
        return CHILD_FAIL;
    }
    rc = h5repack(infile, outfile, &options);
    h5repack_end(&options);
    if (rc < 0) {
        fprintf(stderr, "h5repack rejected %s\n", infile);
        return CHILD_FAIL;
    }
    return CHILD_OK;
}

static int
child_main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "write-small") == 0)
        return write_ref_dataset(argv[2], (hsize_t)strtoull(argv[3], NULL, 10));
    if (argc == 4 && strcmp(argv[1], "write-overflow") == 0)
        return write_ref_dataset(argv[2], (hsize_t)strtoull(argv[3], NULL, 10));
    if (argc == 4 && strcmp(argv[1], "repack") == 0)
        return run_repack(argv[2], argv[3]);
    fprintf(stderr, "unknown child command\n");
    return CHILD_FAIL;
}

static int
wait_child(pid_t pid, int *status_out)
{
    struct timespec start;
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        return -1;

    for (;;) {
        pid_t got = waitpid(pid, status_out, WNOHANG);

        if (got == pid)
            return 0;
        if (got < 0)
            return -1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -1;
        if ((now.tv_sec - start.tv_sec) >= CHILD_TIMEOUT) {
            kill(pid, SIGKILL);
            waitpid(pid, status_out, 0);
            fprintf(stderr, "child exceeded %d seconds\n", CHILD_TIMEOUT);
            return -1;
        }
        usleep(10000);
    }
}

static int
spawn(char **argv, int *exited, int *code)
{
    pid_t pid;
    int   status = 0;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        perror("execv");
        _exit(127);
    }
    if (wait_child(pid, &status) != 0)
        return -1;

    if (WIFEXITED(status)) {
        *exited = 1;
        *code   = WEXITSTATUS(status);
        return 0;
    }
    if (WIFSIGNALED(status)) {
        *exited = 0;
        *code   = WTERMSIG(status);
        return 0;
    }
    fprintf(stderr, "child ended in an unexpected state\n");
    return -1;
}

static int
expect_clean(char **argv, int want_code, const char *label)
{
    int exited = 0;
    int code   = 0;

    if (spawn(argv, &exited, &code) != 0) {
        fprintf(stderr, "FAIL %s: child did not finish cleanly\n", label);
        return -1;
    }
    if (!exited) {
        fprintf(stderr, "FAIL %s: child died with signal %d\n", label, code);
        return -1;
    }
    if (code != want_code) {
        fprintf(stderr, "FAIL %s: exit %d, expected %d\n", label, code, want_code);
        return -1;
    }
    fprintf(stderr, "PASS %s\n", label);
    return 0;
}

int
main(int argc, char **argv)
{
    char    dir[] = "/tmp/h5repack-ref-overflow-XXXXXX";
    char    small_in[256];
    char    small_out[256];
    char    over_in[256];
    char    over_out[256];
    char    nelmts_arg[64];
    char        write_small_cmd[] = "write-small";
    char        write_over_cmd[]  = "write-overflow";
    char        repack_cmd[]      = "repack";
    char       *write_small[5];
    char       *write_over[5];
    char       *repack_small[5];
    char       *repack_over[5];
    size_t  ref_size = 0;
    hsize_t over_nelmts = 0;
    int     failed = 0;

    if (argc > 1)
        _exit(child_main(argc, argv));

    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(small_in, sizeof(small_in), "%s/small.h5", dir);
    snprintf(small_out, sizeof(small_out), "%s/small-out.h5", dir);
    snprintf(over_in, sizeof(over_in), "%s/over.h5", dir);
    snprintf(over_out, sizeof(over_out), "%s/over-out.h5", dir);

    if (native_ref_size(&ref_size) < 0 || overflowing_nelmts(ref_size, &over_nelmts) < 0) {
        fprintf(stderr, "could not compute an overflowing reference count\n");
        return 1;
    }
    fprintf(stderr, "reference size %zu, overflowing nelmts %llu, truncated bytes %u\n", ref_size,
            (unsigned long long)over_nelmts,
            (unsigned)(over_nelmts * (hsize_t)ref_size));

    snprintf(nelmts_arg, sizeof(nelmts_arg), "%llu", (unsigned long long)SMALL_NELMTS);
    write_small[0] = argv[0];
    write_small[1] = write_small_cmd;
    write_small[2] = small_in;
    write_small[3] = nelmts_arg;
    write_small[4] = NULL;
    if (expect_clean(write_small, CHILD_OK, "write small object-reference dataset") < 0)
        failed = 1;

    snprintf(nelmts_arg, sizeof(nelmts_arg), "%llu", (unsigned long long)over_nelmts);
    write_over[0] = argv[0];
    write_over[1] = write_over_cmd;
    write_over[2] = over_in;
    write_over[3] = nelmts_arg;
    write_over[4] = NULL;
    if (expect_clean(write_over, CHILD_OK, "write unallocated overflowing object-reference dataset") < 0)
        failed = 1;

    repack_small[0] = argv[0];
    repack_small[1] = repack_cmd;
    repack_small[2] = small_in;
    repack_small[3] = small_out;
    repack_small[4] = NULL;
    if (!failed && expect_clean(repack_small, CHILD_OK, "repack small object-reference dataset") < 0)
        failed = 1;

    repack_over[0] = argv[0];
    repack_over[1] = repack_cmd;
    repack_over[2] = over_in;
    repack_over[3] = over_out;
    repack_over[4] = NULL;
    /*
     * CHILD_FAIL is h5repack()'s error return. The child _exit()s with
     * that status. A truncated read that runs off the allocation raises a
     * signal instead and fails this check.
     */
    if (!failed &&
        expect_clean(repack_over, CHILD_FAIL, "repack rejects overflowing object-reference dataset") < 0)
        failed = 1;

    return failed ? 1 : 0;
}
