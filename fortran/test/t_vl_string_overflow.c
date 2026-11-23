/*
 * Spec for the Fortran variable-length dataset read overflow.
 *
 * These checks fail while h5dread_vl_string_c copies a file string with
 * strlen() into a slot of dims[0] bytes, and while the integer and real
 * readers copy a file sequence longer than dims[0] into the caller buffer.
 * They pass once each reader copies at most the slot (strings: slot - 1),
 * refuses a null string pointer, and rejects a slot*count product that
 * wraps size_t.
 *
 * h5pget_fapl_multi_c has the same uncapped memcpy shape, but the names it
 * copies come from the file-access property list the caller configured, not
 * from a dataset in a file the caller opened. It is not covered here.
 */
#include "hdf5.h"
#include "H5f90i_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int_f h5dread_vl_string_c(hid_t_f *dset_id, hid_t_f *mem_type_id, hid_t_f *mem_space_id,
                          hid_t_f *file_space_id, hid_t_f *xfer_prp, char *buf, hsize_t_f *dims,
                          size_t_f *len);
int_f h5dread_vl_integer_c(hid_t_f *dset_id, hid_t_f *mem_type_id, hid_t_f *mem_space_id,
                           hid_t_f *file_space_id, hid_t_f *xfer_prp, int_f *buf, hsize_t_f *dims,
                           size_t_f *len);
int_f h5dread_vl_real_c(hid_t_f *dset_id, hid_t_f *mem_type_id, hid_t_f *mem_space_id,
                        hid_t_f *file_space_id, hid_t_f *xfer_prp, real_f *buf, hsize_t_f *dims,
                        size_t_f *len);

struct outcome {
    int  status; /* 0 contract held, 1 contract broken, 2 harness error */
    char detail[512];
};

static void
fail(struct outcome *o, const char *msg)
{
    o->status = 1;
    snprintf(o->detail, sizeof(o->detail), "%s", msg);
}

static hid_t
vl_string_type(void)
{
    hid_t type = H5Tcopy(H5T_C_S1);

    if (type < 0)
        return type;
    if (H5Tset_size(type, H5T_VARIABLE) < 0 || H5Tset_strpad(type, H5T_STR_NULLTERM) < 0 ||
        H5Tset_cset(type, H5T_CSET_ASCII) < 0) {
        H5Tclose(type);
        return H5I_INVALID_HID;
    }
    return type;
}

/* Write n variable-length C strings. A NULL entry is stored as a nil string. */
static int
write_strings(const char *path, char **values, hsize_t n, hid_t *dset, hid_t *type, hid_t *file)
{
    hid_t space = H5I_INVALID_HID;
    herr_t status;

    *file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (*file < 0)
        return -1;
    space = H5Screate_simple(1, &n, NULL);
    *type = vl_string_type();
    if (space < 0 || *type < 0)
        return -1;
    *dset = H5Dcreate2(*file, "vl", *type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (*dset < 0)
        return -1;
    status = H5Dwrite(*dset, *type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values);
    H5Sclose(space);
    return status < 0 ? -1 : 0;
}

static int_f
read_strings(hid_t dset, hid_t type, char *buf, hsize_t_f slot, hsize_t_f count, size_t_f *len)
{
    hid_t_f    dset_f = (hid_t_f)dset;
    hid_t_f    type_f = (hid_t_f)type;
    hid_t_f    space  = (hid_t_f)H5S_ALL;
    hid_t_f    xfer   = (hid_t_f)H5P_DEFAULT;
    hsize_t_f  dims[2];

    dims[0] = slot;
    dims[1] = count;
    return h5dread_vl_string_c(&dset_f, &type_f, &space, &space, &xfer, buf, dims, len);
}

static void
expect_slot(struct outcome *o, const char *slot, size_t slot_len, const char *src, size_t src_len,
            size_t_f reported, const char *label)
{
    size_t keep = src_len;
    size_t i;

    if (slot_len == 0) {
        fail(o, "internal: slot length is zero");
        return;
    }
    if (keep > slot_len - 1)
        keep = slot_len - 1;
    if ((size_t)reported != keep) {
        snprintf(o->detail, sizeof(o->detail), "%s length %llu, want %llu", label,
                 (unsigned long long)reported, (unsigned long long)keep);
        o->status = 1;
        return;
    }
    if (keep > 0 && memcmp(slot, src, keep) != 0) {
        snprintf(o->detail, sizeof(o->detail), "%s prefix mismatch", label);
        o->status = 1;
        return;
    }
    for (i = keep; i < slot_len; i++) {
        if (slot[i] != ' ') {
            snprintf(o->detail, sizeof(o->detail), "%s byte %zu is not a blank pad", label, i);
            o->status = 1;
            return;
        }
    }
}

static void
case_strings_fit(struct outcome *o)
{
    char  *values[2];
    char   out[8 * 2];
    size_t_f len[2];
    hid_t  file, dset, type;
    int_f  ret;

    values[0] = "hello";
    values[1] = "ok";
    memset(out, 'X', sizeof(out));
    if (write_strings("/tmp/vl_fit.h5", values, 2, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for fitting strings");
        return;
    }
    ret = read_strings(dset, type, out, 8, 2, len);
    if (ret != 0) {
        fail(o, "fitting strings were rejected");
        return;
    }
    expect_slot(o, out, 8, "hello", 5, len[0], "fit[0]");
    if (o->status)
        return;
    expect_slot(o, out + 8, 8, "ok", 2, len[1], "fit[1]");
}

static void
case_string_exact_slot(struct outcome *o)
{
    char  *values[1];
    char   out[8];
    size_t_f len[1];
    hid_t  file, dset, type;
    int_f  ret;

    /* Exactly one slot long: the allocation still holds this copy, but the
     * safe contract keeps the last byte as the blank pad. */
    values[0] = "12345678";
    memset(out, 'X', sizeof(out));
    if (write_strings("/tmp/vl_exact.h5", values, 1, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for exact-slot string");
        return;
    }
    ret = read_strings(dset, type, out, 8, 1, len);
    if (ret != 0) {
        fail(o, "exact-slot string was rejected");
        return;
    }
    expect_slot(o, out, 8, "12345678", 8, len[0], "exact");
}

static void
case_string_past_heap(struct outcome *o)
{
    char   payload[256];
    char  *values[1];
    char   out[8];
    size_t_f len[1];
    hid_t  file, dset, type;
    int_f  ret;
    size_t i;

    for (i = 0; i < sizeof(payload) - 1; i++)
        payload[i] = 'A';
    payload[sizeof(payload) - 1] = '\0';
    values[0] = payload;
    memset(out, 'X', sizeof(out));
    if (write_strings("/tmp/vl_past.h5", values, 1, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for overlong string");
        return;
    }
    ret = read_strings(dset, type, out, 8, 1, len);
    if (ret != 0) {
        fail(o, "overlong string was rejected");
        return;
    }
    expect_slot(o, out, 8, payload, strlen(payload), len[0], "past");
}

static void
case_string_spills_neighbor(struct outcome *o)
{
    char   payload[64];
    char  *values[3];
    char   out[8 * 3];
    size_t_f len[3];
    hid_t  file, dset, type;
    int_f  ret;
    size_t i;

    for (i = 0; i < sizeof(payload) - 1; i++)
        payload[i] = 'B';
    payload[sizeof(payload) - 1] = '\0';
    values[0] = "ab";
    values[1] = payload;
    values[2] = "yz";
    memset(out, 'X', sizeof(out));
    if (write_strings("/tmp/vl_neighbor.h5", values, 3, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for neighbor spill");
        return;
    }
    ret = read_strings(dset, type, out, 8, 3, len);
    if (ret != 0) {
        fail(o, "neighbor read was rejected");
        return;
    }
    expect_slot(o, out, 8, "ab", 2, len[0], "neighbor[0]");
    if (o->status)
        return;
    expect_slot(o, out + 8, 8, payload, strlen(payload), len[1], "neighbor[1]");
    if (o->status)
        return;
    expect_slot(o, out + 16, 8, "yz", 2, len[2], "neighbor[2]");
}

static void
case_string_null(struct outcome *o)
{
    char  *values[2];
    char   out[8 * 2];
    size_t_f len[2];
    hid_t  file, dset, type;
    int_f  ret;

    values[0] = "ok";
    values[1] = NULL;
    memset(out, 'X', sizeof(out));
    len[0] = len[1] = 99;
    if (write_strings("/tmp/vl_null.h5", values, 2, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for nil string");
        return;
    }
    ret = read_strings(dset, type, out, 8, 2, len);
    if (ret != -1) {
        snprintf(o->detail, sizeof(o->detail), "nil string returned %d, want -1", (int)ret);
        o->status = 1;
    }
}

static void
case_string_slot_zero(struct outcome *o)
{
    char  *values[1];
    char   out[1];
    size_t_f len[1];
    hid_t  file, dset, type;
    int_f  ret;

    values[0] = "hi";
    out[0]    = 'X';
    len[0]    = 99;
    if (write_strings("/tmp/vl_slot0.h5", values, 1, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for zero slot");
        return;
    }
    ret = read_strings(dset, type, out, 0, 1, len);
    if (ret != -1) {
        snprintf(o->detail, sizeof(o->detail), "zero slot returned %d, want -1", (int)ret);
        o->status = 1;
        return;
    }
    if (out[0] != 'X')
        fail(o, "zero slot wrote the caller buffer");
}

static void
case_string_size_overflow(struct outcome *o)
{
    char  *values[2];
    char   out[2];
    size_t_f len[2];
    hid_t  file, dset, type;
    hid_t_f dset_f, type_f, space, xfer;
    hsize_t_f dims[2];
    int_f  ret;

    if (sizeof(size_t) < 8 || sizeof(hsize_t_f) < 8) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "size_t is too narrow to represent the wrap");
        return;
    }
    values[0] = "a";
    values[1] = "b";
    out[0] = out[1] = 'X';
    if (write_strings("/tmp/vl_wrap.h5", values, 2, &dset, &type, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for size wrap");
        return;
    }
    /* dims[0] * dims[1] wraps size_t. The file itself holds two short strings. */
    dims[0] = (hsize_t_f)(SIZE_MAX / 2) + 2;
    dims[1] = 2;
    dset_f  = (hid_t_f)dset;
    type_f  = (hid_t_f)type;
    space   = (hid_t_f)H5S_ALL;
    xfer    = (hid_t_f)H5P_DEFAULT;
    ret     = h5dread_vl_string_c(&dset_f, &type_f, &space, &space, &xfer, out, dims, len);
    if (ret != -1) {
        snprintf(o->detail, sizeof(o->detail), "wrapping slot*count returned %d, want -1", (int)ret);
        o->status = 1;
        return;
    }
    if (out[0] != 'X' || out[1] != 'X')
        fail(o, "wrapping slot*count wrote the caller buffer");
}

static int
write_sequences(const char *path, hid_t base, const void *elem, size_t elem_size, size_t seq_len, hid_t *dset,
                hid_t *type, hid_t *space, hid_t *file)
{
    hsize_t n = 1;
    hvl_t   vl;
    herr_t  status;

    *file = H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (*file < 0)
        return -1;
    *space = H5Screate_simple(1, &n, NULL);
    *type  = H5Tvlen_create(base);
    if (*space < 0 || *type < 0)
        return -1;
    *dset = H5Dcreate2(*file, "vl", *type, *space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (*dset < 0)
        return -1;
    vl.len = seq_len;
    vl.p   = (void *)elem;
    status = H5Dwrite(*dset, *type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &vl);
    return status < 0 ? -1 : 0;
}

static void
case_integer_overlong(struct outcome *o)
{
    enum { SLOT = 4, STORED = 16, GUARD = 8 };
    int      stored[STORED];
    int_f    buf[SLOT + GUARD];
    size_t_f len;
    hid_t    file, dset, type, space;
    hid_t_f  dset_f, type_f, space_f, xfer;
    hsize_t_f dims[2];
    int_f    ret;
    int      i;
    const int_f sentinel = (int_f)0x13579BDF;

    if (sizeof(int_f) != sizeof(int)) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "int_f is not the C int used to write the file");
        return;
    }
    for (i = 0; i < STORED; i++)
        stored[i] = 1000 + i;
    for (i = 0; i < SLOT + GUARD; i++)
        buf[i] = sentinel;
    if (write_sequences("/tmp/vl_int.h5", H5T_NATIVE_INT, stored, sizeof(stored[0]), STORED, &dset, &type,
                        &space, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for integer sequence");
        return;
    }
    dims[0] = SLOT;
    dims[1] = 1;
    dset_f  = (hid_t_f)dset;
    type_f  = (hid_t_f)type;
    space_f = (hid_t_f)space;
    xfer    = (hid_t_f)H5P_DEFAULT;
    ret     = h5dread_vl_integer_c(&dset_f, &type_f, &space_f, &space_f, &xfer, buf, dims, &len);
    if (ret != 0) {
        fail(o, "overlong integer sequence was rejected");
        return;
    }
    if ((size_t)len != SLOT) {
        snprintf(o->detail, sizeof(o->detail), "integer length %llu, want %d", (unsigned long long)len, SLOT);
        o->status = 1;
        return;
    }
    for (i = 0; i < SLOT; i++) {
        if (buf[i] != (int_f)stored[i]) {
            snprintf(o->detail, sizeof(o->detail), "integer element %d mismatch", i);
            o->status = 1;
            return;
        }
    }
    for (i = SLOT; i < SLOT + GUARD; i++) {
        if (buf[i] != sentinel) {
            snprintf(o->detail, sizeof(o->detail), "integer read wrote past the slot at %d", i);
            o->status = 1;
            return;
        }
    }
}

static void
case_real_overlong(struct outcome *o)
{
    enum { SLOT = 4, STORED = 16, GUARD = 8 };
    float    stored[STORED];
    real_f   buf[SLOT + GUARD];
    size_t_f len;
    hid_t    file, dset, type, space;
    hid_t_f  dset_f, type_f, space_f, xfer;
    hsize_t_f dims[2];
    int_f    ret;
    int      i;
    const real_f sentinel = (real_f)-12345.0f;

    if (sizeof(real_f) != sizeof(float)) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "real_f is not the C float used to write the file");
        return;
    }
    for (i = 0; i < STORED; i++)
        stored[i] = (float)(i + 1);
    for (i = 0; i < SLOT + GUARD; i++)
        buf[i] = sentinel;
    if (write_sequences("/tmp/vl_real.h5", H5T_NATIVE_FLOAT, stored, sizeof(stored[0]), STORED, &dset, &type,
                        &space, &file) < 0) {
        o->status = 2;
        snprintf(o->detail, sizeof(o->detail), "setup failed for real sequence");
        return;
    }
    dims[0] = SLOT;
    dims[1] = 1;
    dset_f  = (hid_t_f)dset;
    type_f  = (hid_t_f)type;
    space_f = (hid_t_f)space;
    xfer    = (hid_t_f)H5P_DEFAULT;
    ret     = h5dread_vl_real_c(&dset_f, &type_f, &space_f, &space_f, &xfer, buf, dims, &len);
    if (ret != 0) {
        fail(o, "overlong real sequence was rejected");
        return;
    }
    if ((size_t)len != SLOT) {
        snprintf(o->detail, sizeof(o->detail), "real length %llu, want %d", (unsigned long long)len, SLOT);
        o->status = 1;
        return;
    }
    for (i = 0; i < SLOT; i++) {
        if (buf[i] != (real_f)stored[i]) {
            snprintf(o->detail, sizeof(o->detail), "real element %d mismatch", i);
            o->status = 1;
            return;
        }
    }
    for (i = SLOT; i < SLOT + GUARD; i++) {
        if (buf[i] != sentinel) {
            snprintf(o->detail, sizeof(o->detail), "real read wrote past the slot at %d", i);
            o->status = 1;
            return;
        }
    }
}

struct named_case {
    const char *name;
    void (*fn)(struct outcome *);
};

static int
run_case(const struct named_case *c)
{
    int            fds[2];
    pid_t          pid;
    int            status;
    struct outcome outcome;
    ssize_t        n;

    if (pipe(fds) < 0) {
        perror("pipe");
        return 2;
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 2;
    }
    if (pid == 0) {
        struct outcome local;

        close(fds[0]);
        memset(&local, 0, sizeof(local));
        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        c->fn(&local);
        if (write(fds[1], &local, sizeof(local)) != (ssize_t)sizeof(local))
            _exit(2);
        _exit(0);
    }
    close(fds[1]);
    memset(&outcome, 0, sizeof(outcome));
    n = read(fds[0], &outcome, sizeof(outcome));
    close(fds[0]);
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 2;
    }
    printf("%s: ", c->name);
    if (WIFSIGNALED(status)) {
        printf("FAIL signal %d (read wrote outside the slot or the packing allocation)\n", WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n != (ssize_t)sizeof(outcome)) {
        printf("FAIL child exit without a contract result\n");
        return 2;
    }
    if (outcome.status == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("%s %s\n", outcome.status == 2 ? "HARNESS" : "FAIL", outcome.detail);
    return outcome.status;
}

int
main(void)
{
    static const struct named_case cases[] = {
        {"string_fits", case_strings_fit},
        {"string_exact_slot", case_string_exact_slot},
        {"string_past_heap", case_string_past_heap},
        {"string_spills_neighbor", case_string_spills_neighbor},
        {"string_null", case_string_null},
        {"string_slot_zero", case_string_slot_zero},
        {"string_size_overflow", case_string_size_overflow},
        {"integer_overlong", case_integer_overlong},
        {"real_overlong", case_real_overlong},
    };
    size_t i;
    int    rc = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int one = run_case(&cases[i]);

        if (one != 0 && rc == 0)
            rc = one;
        else if (one == 1)
            rc = 1;
    }
    return rc;
}
