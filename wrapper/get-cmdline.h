#ifndef _LTTNG_WRAPPER_GET_CMDLINE_H
#define _LTTNG_WRAPPER_GET_CMDLINE_H

/*
 * wrapper/vmalloc.h
 *
 * wrapper around vmalloc_sync_all. Using KALLSYMS to get its address when
 * available, else we need to have a kernel that exports this function to GPL
 * modules.
 *
 * Copyright (C) 2011-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <linux/version.h>
#include <linux/mm.h>

#ifdef CONFIG_KALLSYMS

#include <linux/kallsyms.h>
#include <wrapper/kallsyms.h>

static inline
int wrapper_get_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	int (*get_cmdline_sym)(struct task_struct *task, char *buffer, int buflen);

	get_cmdline_sym = (void *) kallsyms_lookup_funcptr("get_cmdline");
	if (get_cmdline_sym) {
		return get_cmdline_sym(task, buffer, buflen);
	} else {
		return 0;
	}
}

#else

static inline
int wrapper_get_cmdline(struct task_struct *task, char *buffer, int buflen)
{
	return 0;
}

#endif

#endif /* _LTTNG_WRAPPER_GET_CMDLINE_H */
