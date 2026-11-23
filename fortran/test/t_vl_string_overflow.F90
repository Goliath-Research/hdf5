!
! Public Fortran entry points for the variable-length dataset read overflow.
!
! Each invocation is one case, selected by the first argument, so a case that
! dies in the C helper does not hide the others. The safe contract matches
! t_vl_string_overflow.c: a string is copied up to one byte short of the
! character length, and an integer or real sequence is copied up to the slot.
!
PROGRAM t_vl_string_overflow
  USE HDF5
  IMPLICIT NONE

  CHARACTER(LEN=32) :: which
  INTEGER :: arglen, stat

  CALL GET_COMMAND_ARGUMENT(1, which, arglen, stat)
  IF (stat /= 0 .OR. arglen <= 0) THEN
    WRITE(*,*) "usage: t_vl_string_overflow <fit|exact|past|neighbor|integer|real>"
    ERROR STOP 2
  END IF

  SELECT CASE (which(1:arglen))
  CASE ("fit")
    CALL string_fit
  CASE ("exact")
    CALL string_exact
  CASE ("past")
    CALL string_past
  CASE ("neighbor")
    CALL string_neighbor
  CASE ("integer")
    CALL integer_overlong
  CASE ("real")
    CALL real_overlong
  CASE DEFAULT
    WRITE(*,*) "unknown case ", which(1:arglen)
    ERROR STOP 2
  END SELECT
CONTAINS

  SUBROUTINE string_fit
    CHARACTER(LEN=8), DIMENSION(2) :: written, got
    INTEGER(SIZE_T), DIMENSION(2) :: slen, slen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: dims

    written(1) = "hello"
    written(2) = "ok"
    slen(1) = 5
    slen(2) = 2
    dims = (/8_HSIZE_T, 2_HSIZE_T/)
    CALL round_trip_strings(written, got, dims, slen, slen_out)
    CALL expect_string(got(1), slen_out(1), "hello", 5_SIZE_T, "fit(1)")
    CALL expect_string(got(2), slen_out(2), "ok", 2_SIZE_T, "fit(2)")
  END SUBROUTINE string_fit

  SUBROUTINE string_exact
    CHARACTER(LEN=8), DIMENSION(1) :: written, got
    INTEGER(SIZE_T), DIMENSION(1) :: slen, slen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: dims

    written(1) = "12345678"
    slen(1) = 8
    dims = (/8_HSIZE_T, 1_HSIZE_T/)
    CALL round_trip_strings(written, got, dims, slen, slen_out)
    ! The file string fills the character length. One byte stays blank.
    CALL expect_string(got(1), slen_out(1), "12345678", 8_SIZE_T, "exact")
  END SUBROUTINE string_exact

  SUBROUTINE string_past
    CHARACTER(LEN=256), DIMENSION(1) :: written
    CHARACTER(LEN=8), DIMENSION(1) :: got
    INTEGER(SIZE_T), DIMENSION(1) :: slen, slen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: write_dims, read_dims
    INTEGER :: i

    DO i = 1, 255
      written(1)(i:i) = "A"
    END DO
    written(1)(256:256) = " "
    slen(1) = 255
    write_dims = (/256_HSIZE_T, 1_HSIZE_T/)
    read_dims = (/8_HSIZE_T, 1_HSIZE_T/)
    CALL write_then_read_string(written, write_dims, slen, got, read_dims, slen_out)
    CALL expect_string(got(1), slen_out(1), written(1)(1:255), 255_SIZE_T, "past")
  END SUBROUTINE string_past

  SUBROUTINE string_neighbor
    CHARACTER(LEN=64), DIMENSION(3) :: written
    CHARACTER(LEN=8), DIMENSION(3) :: got
    INTEGER(SIZE_T), DIMENSION(3) :: slen, slen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: write_dims, read_dims
    INTEGER :: i

    written(1) = "ab"
    slen(1) = 2
    DO i = 1, 63
      written(2)(i:i) = "B"
    END DO
    written(2)(64:64) = " "
    slen(2) = 63
    written(3) = "yz"
    slen(3) = 2
    write_dims = (/64_HSIZE_T, 3_HSIZE_T/)
    read_dims = (/8_HSIZE_T, 3_HSIZE_T/)
    CALL write_then_read_string(written, write_dims, slen, got, read_dims, slen_out)
    CALL expect_string(got(1), slen_out(1), "ab", 2_SIZE_T, "neighbor(1)")
    CALL expect_string(got(2), slen_out(2), written(2)(1:63), 63_SIZE_T, "neighbor(2)")
    CALL expect_string(got(3), slen_out(3), "yz", 2_SIZE_T, "neighbor(3)")
  END SUBROUTINE string_neighbor

  SUBROUTINE integer_overlong
    INTEGER, DIMENSION(16, 1) :: written
    INTEGER, DIMENSION(4, 3) :: got
    INTEGER(SIZE_T), DIMENSION(1) :: seqlen, seqlen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: write_dims, read_dims
    INTEGER(HID_T) :: file_id, type_id, space_id, dset_id
    INTEGER(HSIZE_T), DIMENSION(1) :: dims
    INTEGER :: error, i
    CHARACTER(LEN=32) :: filename

    filename = "/tmp/vl_f_int.h5"
    DO i = 1, 16
      written(i, 1) = 1000 + i
    END DO
    seqlen(1) = 16
    write_dims = (/16_HSIZE_T, 1_HSIZE_T/)
    read_dims = (/4_HSIZE_T, 1_HSIZE_T/)
    got = -999
    CALL h5open_f(error)
    CALL check(error, "h5open_f")
    CALL h5fcreate_f(filename, H5F_ACC_TRUNC_F, file_id, error)
    CALL check(error, "h5fcreate_f")
    dims(1) = 1
    CALL h5screate_simple_f(1, dims, space_id, error)
    CALL check(error, "h5screate_simple_f")
    CALL h5tvlen_create_f(H5T_NATIVE_INTEGER, type_id, error)
    CALL check(error, "h5tvlen_create_f")
    CALL h5dcreate_f(file_id, "vl", type_id, space_id, dset_id, error)
    CALL check(error, "h5dcreate_f")
    CALL h5dwrite_vl_f(dset_id, type_id, written, write_dims, seqlen, error)
    CALL check(error, "h5dwrite_vl_f")
    CALL h5dclose_f(dset_id, error)
    CALL h5fclose_f(file_id, error)
    CALL h5fopen_f(filename, H5F_ACC_RDONLY_F, file_id, error)
    CALL check(error, "h5fopen_f")
    CALL h5dopen_f(file_id, "vl", dset_id, error)
    CALL check(error, "h5dopen_f")
    CALL h5dread_vl_f(dset_id, type_id, got, read_dims, seqlen_out, error)
    IF (error /= 0) CALL die("integer read returned an error")
    IF (seqlen_out(1) /= 4) CALL die("integer length was not capped at the slot")
    DO i = 1, 4
      IF (got(i, 1) /= written(i, 1)) CALL die("integer prefix mismatch")
    END DO
    DO i = 1, 4
      IF (got(i, 2) /= -999 .OR. got(i, 3) /= -999) CALL die("integer read wrote past the slot")
    END DO
    CALL h5dclose_f(dset_id, error)
    CALL h5sclose_f(space_id, error)
    CALL h5tclose_f(type_id, error)
    CALL h5fclose_f(file_id, error)
    CALL h5close_f(error)
  END SUBROUTINE integer_overlong

  SUBROUTINE real_overlong
    REAL, DIMENSION(16, 1) :: written
    REAL, DIMENSION(4, 3) :: got
    INTEGER(SIZE_T), DIMENSION(1) :: seqlen, seqlen_out
    INTEGER(HSIZE_T), DIMENSION(2) :: write_dims, read_dims
    INTEGER(HID_T) :: file_id, type_id, space_id, dset_id
    INTEGER(HSIZE_T), DIMENSION(1) :: dims
    INTEGER :: error, i
    CHARACTER(LEN=32) :: filename

    filename = "/tmp/vl_f_real.h5"
    DO i = 1, 16
      written(i, 1) = REAL(i)
    END DO
    seqlen(1) = 16
    write_dims = (/16_HSIZE_T, 1_HSIZE_T/)
    read_dims = (/4_HSIZE_T, 1_HSIZE_T/)
    got = -999.0
    CALL h5open_f(error)
    CALL check(error, "h5open_f")
    CALL h5fcreate_f(filename, H5F_ACC_TRUNC_F, file_id, error)
    CALL check(error, "h5fcreate_f")
    dims(1) = 1
    CALL h5screate_simple_f(1, dims, space_id, error)
    CALL check(error, "h5screate_simple_f")
    CALL h5tvlen_create_f(H5T_NATIVE_REAL, type_id, error)
    CALL check(error, "h5tvlen_create_f")
    CALL h5dcreate_f(file_id, "vl", type_id, space_id, dset_id, error)
    CALL check(error, "h5dcreate_f")
    CALL h5dwrite_vl_f(dset_id, type_id, written, write_dims, seqlen, error)
    CALL check(error, "h5dwrite_vl_f")
    CALL h5dclose_f(dset_id, error)
    CALL h5fclose_f(file_id, error)
    CALL h5fopen_f(filename, H5F_ACC_RDONLY_F, file_id, error)
    CALL check(error, "h5fopen_f")
    CALL h5dopen_f(file_id, "vl", dset_id, error)
    CALL check(error, "h5dopen_f")
    CALL h5dread_vl_f(dset_id, type_id, got, read_dims, seqlen_out, error)
    IF (error /= 0) CALL die("real read returned an error")
    IF (seqlen_out(1) /= 4) CALL die("real length was not capped at the slot")
    DO i = 1, 4
      IF (got(i, 1) /= written(i, 1)) CALL die("real prefix mismatch")
    END DO
    DO i = 1, 4
      IF (got(i, 2) /= -999.0 .OR. got(i, 3) /= -999.0) CALL die("real read wrote past the slot")
    END DO
    CALL h5dclose_f(dset_id, error)
    CALL h5sclose_f(space_id, error)
    CALL h5tclose_f(type_id, error)
    CALL h5fclose_f(file_id, error)
    CALL h5close_f(error)
  END SUBROUTINE real_overlong

  SUBROUTINE round_trip_strings(written, got, dims, slen, slen_out)
    CHARACTER(LEN=*), DIMENSION(:) :: written, got
    INTEGER(HSIZE_T), DIMENSION(2) :: dims
    INTEGER(SIZE_T), DIMENSION(*) :: slen, slen_out

    CALL write_then_read_string(written, dims, slen, got, dims, slen_out)
  END SUBROUTINE round_trip_strings

  SUBROUTINE write_then_read_string(written, write_dims, slen, got, read_dims, slen_out)
    CHARACTER(LEN=*), DIMENSION(:) :: written, got
    INTEGER(HSIZE_T), DIMENSION(2) :: write_dims, read_dims
    INTEGER(SIZE_T), DIMENSION(*) :: slen, slen_out
    INTEGER(HID_T) :: file_id, space_id, dset_id
    INTEGER(HSIZE_T), DIMENSION(1) :: dims
    INTEGER :: error
    CHARACTER(LEN=32) :: filename

    filename = "/tmp/vl_f_string.h5"
    CALL h5open_f(error)
    CALL check(error, "h5open_f")
    CALL h5fcreate_f(filename, H5F_ACC_TRUNC_F, file_id, error)
    CALL check(error, "h5fcreate_f")
    dims(1) = write_dims(2)
    CALL h5screate_simple_f(1, dims, space_id, error)
    CALL check(error, "h5screate_simple_f")
    CALL h5dcreate_f(file_id, "vl", H5T_STRING, space_id, dset_id, error)
    CALL check(error, "h5dcreate_f")
    CALL h5dwrite_vl_f(dset_id, H5T_STRING, written, write_dims, slen, error)
    CALL check(error, "h5dwrite_vl_f")
    CALL h5dclose_f(dset_id, error)
    CALL check(error, "h5dclose_f")
    CALL h5fclose_f(file_id, error)
    CALL check(error, "h5fclose_f")

    CALL h5fopen_f(filename, H5F_ACC_RDONLY_F, file_id, error)
    CALL check(error, "h5fopen_f")
    CALL h5dopen_f(file_id, "vl", dset_id, error)
    CALL check(error, "h5dopen_f")
    got = ""
    CALL h5dread_vl_f(dset_id, H5T_STRING, got, read_dims, slen_out, error)
    IF (error /= 0) CALL die("string read returned an error")
    CALL h5dclose_f(dset_id, error)
    CALL h5sclose_f(space_id, error)
    CALL h5fclose_f(file_id, error)
    CALL h5close_f(error)
  END SUBROUTINE write_then_read_string

  SUBROUTINE expect_string(got, reported, src, src_len, label)
    CHARACTER(LEN=*), INTENT(IN) :: got, src, label
    INTEGER(SIZE_T), INTENT(IN) :: reported, src_len
    INTEGER(SIZE_T) :: keep
    INTEGER :: i

    keep = src_len
    IF (keep > LEN(got) - 1) keep = LEN(got) - 1
    IF (reported /= keep) CALL die(TRIM(label) // " length was not capped at the slot")
    IF (keep > 0) THEN
      IF (got(1:keep) /= src(1:keep)) CALL die(TRIM(label) // " prefix mismatch")
    END IF
    IF (keep < LEN(got)) THEN
      DO i = keep + 1, LEN(got)
        IF (got(i:i) /= " ") CALL die(TRIM(label) // " pad is not blank")
      END DO
    END IF
  END SUBROUTINE expect_string

  SUBROUTINE check(error, what)
    INTEGER, INTENT(IN) :: error
    CHARACTER(LEN=*), INTENT(IN) :: what
    IF (error /= 0) CALL die(TRIM(what) // " failed")
  END SUBROUTINE check

  SUBROUTINE die(msg)
    CHARACTER(LEN=*), INTENT(IN) :: msg
    WRITE(*,*) "FAIL ", msg
    ERROR STOP 1
  END SUBROUTINE die

END PROGRAM t_vl_string_overflow
