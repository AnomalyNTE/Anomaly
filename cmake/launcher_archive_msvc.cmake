# Platform configuration for the pinned zlib 1.4, xz 5.8.4 and libarchive 3.8.11.
# The supported VS2022 x64/UCRT toolchain has a fixed ABI; avoid hundreds of
# individual MSBuild try_compile invocations. Review this list on dependency or
# toolchain upgrades. These are normal variables local to the archive function,
# not a global CMake cache or overrides of CMake's compiler validation.

foreach(header IN ITEMS
        sys/types.h stdint.h stddef.h stdarg.h ctype.h direct.h errno.h fcntl.h
        inttypes.h io.h limits.h intsafe.h locale.h process.h signal.h stdio.h
        stdlib.h string.h sys/stat.h sys/utime.h time.h wchar.h wctype.h
        windows.h bcrypt.h winioctl.h immintrin.h)
    string(MAKE_C_IDENTIFIER "${header}" feature)
    string(TOUPPER "${feature}" feature)
    set(HAVE_${feature} 1)
endforeach()

foreach(header IN ITEMS
        acl/libacl.h attr/xattr.h copyfile.h dirent.h dlfcn.h ext2fs/ext2_fs.h
        fnmatch.h grp.h langinfo.h linux/types.h linux/fiemap.h linux/fs.h linux/magic.h
        membership.h ndir.h paths.h poll.h pthread.h pwd.h readpassphrase.h regex.h
        spawn.h strings.h sys/acl.h sys/cdefs.h sys/dir.h sys/extattr.h sys/ioctl.h
        sys/mkdev.h sys/mount.h sys/ndir.h sys/param.h sys/poll.h sys/richacl.h
        sys/select.h sys/single_threaded.h sys/statfs.h sys/statvfs.h sys/sysctl.h
        sys/sysmacros.h sys/time.h sys/utsname.h sys/vfs.h sys/wait.h sys/xattr.h
        unistd.h utime.h stdckdint.h cpuid.h byteswap.h sys/endian.h sys/byteorder.h)
    string(MAKE_C_IDENTIFIER "${header}" feature)
    string(TOUPPER "${feature}" feature)
    set(HAVE_${feature} FALSE)
endforeach()

foreach(function IN ITEMS
        fstat getpid mbrtowc mkdir setlocale strchr strdup strerror strncpy_s
        strnlen strrchr tzset utime wcrtomb wcscmp wcscpy wcslen wctomb
        _fseeki64 _get_timezone ctime_s gmtime_s localtime_s _mkgmtime strftime)
    string(TOUPPER "${function}" feature)
    set(HAVE_${feature} 1)
endforeach()
set(HAVE__CrtSetReportMode 1)

foreach(function IN ITEMS
        arc4random_buf chflags chown chroot closefrom close_range ctime_r
        explicit_bzero fchdir fchflags fchmod fchown fcntl fdopendir fnmatch fork
        fstatat fstatfs fstatvfs ftruncate futimens futimes futimesat getegid geteuid
        getgrgid_r getgrnam_r getline getpwnam_r getpwuid_r getresgid getresuid
        gettimeofday getvfsbyname gmtime_r issetugid lchflags lchmod lchown link linkat
        localtime_r lstat lutimes mkfifo mknod mkstemp nl_langinfo openat poll
        posix_spawn posix_spawnp readlink readpassphrase setenv sigaction statfs
        statvfs symlink sysconf tcgetattr tcsetattr timegm unlinkat unsetenv utimes
        utimensat vfork cygwin_conv_path fseeko strerror_r waitpid dirfd readlinkat
        memset_explicit pipe select clock_gettime getopt_long cap_rights_limit pledge)
    string(TOUPPER "${function}" feature)
    set(HAVE_${feature} FALSE)
endforeach()

# Keep libarchive's existing fallbacks for CRT functions whose bare link probes
# fail with MSVC (some are intrinsics or header inlines).
foreach(function IN ITEMS memmove vprintf wmemcmp wmemcpy wmemmove)
    string(TOUPPER "${function}" feature)
    set(HAVE_${feature} FALSE)
endforeach()

foreach(feature IN ITEMS
        HAVE_WORKING_EXT2_IOC_GETFLAGS HAVE_WORKING_FS_IOC_GETFLAGS
        HAVE_STRUCT_VFSCONF HAVE_STRUCT_XVFSCONF HAVE_STRUCT_STATFS
        MAJOR_IN_MKDEV MAJOR_IN_SYSMACROS HAVE_D_MD_ORDER
        HAVE_STRUCT_TM_TM_GMTOFF HAVE_STRUCT_TM___TM_GMTOFF
        HAVE_STRUCT_STAT_ST_BIRTHTIME HAVE_STRUCT_STAT_ST_BIRTHTIMESPEC_TV_NSEC
        HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC
        HAVE_STRUCT_STAT_ST_MTIME_N HAVE_STRUCT_STAT_ST_UMTIME
        HAVE_STRUCT_STAT_ST_MTIME_USEC HAVE_STRUCT_STAT_ST_BLKSIZE
        HAVE_STRUCT_STAT_ST_FLAGS HAVE_VISIBILITY_ATTR HAVE___ATTR__VIS_HIDDEN
        HAVE_EFTYPE HAVE_DECL_SSIZE_MAX
        HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR HAVE___BUILTIN_BSWAPXX
        HAVE___BUILTIN_ASSUME_ALIGNED HAVE_CLOCK_GETTIME_LIBRT
        HAVE_ARM64_CRC32 HAVE_LOONGARCH_CRC32 HAVE_LINUX_LANDLOCK
        CMAKE_HAVE_LIBC_PTHREAD CMAKE_HAVE_PTHREADS_CREATE CMAKE_HAVE_PTHREAD_CREATE
        ARCHIVE_CRYPTO_RMD160_LIBC ARCHIVE_CRYPTO_RMD160_LIBMD)
    set(${feature} FALSE)
endforeach()

foreach(feature IN ITEMS
        SAFE_TO_DEFINE_EXTENSIONS HAVE_EILSEQ HAVE__MM_MOVEMASK_EPI8
        HAVE_USABLE_CLMUL TUKLIB_FAST_UNALIGNED_ACCESS)
    set(${feature} 1)
endforeach()

foreach(constant IN ITEMS
        INT32_MAX INT32_MIN INT64_MAX INT64_MIN INTMAX_MAX INTMAX_MIN
        UINT32_MAX UINT64_MAX UINTMAX_MAX SIZE_MAX)
    set(HAVE_DECL_${constant} 1)
endforeach()

# Windows x64 uses LLP64. UCRT off_t remains 32-bit; large files use _fseeki64.
set(UINT8_T 1)
foreach(type IN ITEMS SIZEOF_SHORT SIZEOF_UNSIGNED_SHORT INT16_T UINT16_T SIZEOF_WCHAR_T)
    set(${type} 2)
endforeach()
foreach(type IN ITEMS SIZEOF_INT SIZEOF_LONG SIZEOF_UNSIGNED SIZEOF_UNSIGNED_LONG
        INT32_T UINT32_T DEV_T OFF_T)
    set(${type} 4)
endforeach()
foreach(type IN ITEMS SIZEOF_LONG_LONG SIZEOF_UNSIGNED_LONG_LONG __INT64 UNSIGNED___INT64
        INT64_T INTMAX_T UINT64_T UINTMAX_T SIZE_T INTPTR_T UINTPTR_T)
    set(${type} 8)
endforeach()
foreach(type IN ITEMS
        SIZEOF_SHORT SIZEOF_INT SIZEOF_LONG SIZEOF_LONG_LONG SIZEOF_UNSIGNED_SHORT
        SIZEOF_UNSIGNED SIZEOF_UNSIGNED_LONG SIZEOF_UNSIGNED_LONG_LONG
        __INT64 UNSIGNED___INT64 INT16_T INT32_T INT64_T INTMAX_T
        UINT8_T UINT16_T UINT32_T UINT64_T UINTMAX_T DEV_T OFF_T SIZE_T
        INTPTR_T UINTPTR_T SIZEOF_WCHAR_T)
    set(HAVE_${type} 1)
endforeach()
foreach(type IN ITEMS OFF64_T GID_T ID_T MODE_T SSIZE_T UID_T PID_T)
    set(HAVE_${type} FALSE)
    set(${type} "")
endforeach()
set(_FILE_OFFSET_BITS "")

# XZ command-line tools and localization are disabled by the caller.
set(CMAKE_DISABLE_FIND_PACKAGE_Intl TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_Gettext TRUE)
