#ifndef _LTTNG_WRAPPER_CGROUP_H
#define _LTTNG_WRAPPER_CGROUP_H

/*
 * wrapper/cgroup.h
 *
 * wrapper cgroup functions and data structures. Using
 * KALLSYMS to get its address when available, else we need to have a
 * kernel that exports this function to GPL modules.
 *
 * Loic Gelle
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

#include <linux/cgroup.h>

#ifdef CONFIG_KALLSYMS_ALL

#include <linux/kallsyms.h>
#include <wrapper/kallsyms.h>

/* static inline
char *wrapper_disk_name(struct gendisk *hd, int partno, char *buf)
{
	char *(*disk_name_sym)(struct gendisk *hd, int partno, char *buf);

	disk_name_sym = (void *) kallsyms_lookup_funcptr("disk_name");
	if (disk_name_sym) {
		return disk_name_sym(hd, partno, buf);
	} else {
		printk_once(KERN_WARNING "LTTng: disk_name symbol lookup failed.\n");
		return NULL;
	}
} */

static inline
bool wrapper_get_cgrp_dfl_visible(void)
{
	return (bool) kallsyms_lookup_dataptr("cgrp_dfl_visible");
}

static inline
struct list_head* wrapper_get_cgroup_roots(void)
{
	return (struct list_head*) kallsyms_lookup_dataptr("cgroup_roots");
}

static inline
struct cgroup_subsys** wrapper_get_cgroup_subsys(void)
{
	return (struct cgroup_subsys**) kallsyms_lookup_dataptr("cgroup_subsys");
}

#endif

#endif /* _LTTNG_WRAPPER_CGROUP_H */
