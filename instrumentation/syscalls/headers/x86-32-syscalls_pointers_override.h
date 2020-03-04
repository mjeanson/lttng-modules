/* SPDX-License-Identifier: (GPL-2.0 or LGPL-2.1) */

#ifndef CREATE_SYSCALL_TABLE

# ifndef CONFIG_UID16
#  define OVERRIDE_32_getgroups16
#  define OVERRIDE_32_setgroups16
#  define OVERRIDE_32_lchown16
#  define OVERRIDE_32_getresuid16
#  define OVERRIDE_32_getresgid16
#  define OVERRIDE_32_chown16
# endif

/*
 * Override fildes to ctf_user_array()
 */
#define OVERRIDE_32_pipe
#define OVERRIDE_64_pipe
SC_LTTNG_TRACEPOINT_EVENT(pipe,
    TP_PROTO(sc_exit(long ret,) int * fildes),
    TP_ARGS(sc_exit(ret,) fildes),
    TP_FIELDS(sc_exit(ctf_integer(long, ret, ret))
        sc_out(ctf_user_array(int, fildes, fildes, 2))
    )
)

#else	/* CREATE_SYSCALL_TABLE */

# ifndef CONFIG_UID16
#  define OVERRIDE_TABLE_32_getgroups16
#  define OVERRIDE_TABLE_32_setgroups16
#  define OVERRIDE_TABLE_32_lchown16
#  define OVERRIDE_TABLE_32_getresuid16
#  define OVERRIDE_TABLE_32_getresgid16
#  define OVERRIDE_TABLE_32_chown16
# endif

#endif /* CREATE_SYSCALL_TABLE */


