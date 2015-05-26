#undef TRACE_SYSTEM
#define TRACE_SYSTEM regmap

#if !defined(_TRACE_REGMAP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_REGMAP_H

#include <linux/ktime.h>
#include <linux/tracepoint.h>
#include <linux/version.h>

#ifndef _TRACE_REGMAP_DEF_
#define _TRACE_REGMAP_DEF_
struct device;
struct regmap;
#endif

/*
 * Log register events
 */
DECLARE_EVENT_CLASS(regmap_reg,

	TP_PROTO(struct device *dev, unsigned int reg,
		 unsigned int val),

	TP_ARGS(dev, reg, val),

	TP_STRUCT__entry(
		__string(	name,		dev_name(dev)	)
		__field(	unsigned int,	reg		)
		__field(	unsigned int,	val		)
	),

	TP_fast_assign(
		tp_strcpy(name, dev_name(dev))
		tp_assign(reg, reg)
		tp_assign(val, val)
	),

	TP_printk("%s reg=%x val=%x", __get_str(name),
		  (unsigned int)__entry->reg,
		  (unsigned int)__entry->val)
)

DEFINE_EVENT(regmap_reg, regmap_reg_write,

	TP_PROTO(struct device *dev, unsigned int reg,
		 unsigned int val),

	TP_ARGS(dev, reg, val)

)

DEFINE_EVENT(regmap_reg, regmap_reg_read,

	TP_PROTO(struct device *dev, unsigned int reg,
		 unsigned int val),

	TP_ARGS(dev, reg, val)

)

DECLARE_EVENT_CLASS(regmap_block,

	TP_PROTO(struct device *dev, unsigned int reg, int count),

	TP_ARGS(dev, reg, count),

	TP_STRUCT__entry(
		__string(	name,		dev_name(dev)	)
		__field(	unsigned int,	reg		)
		__field(	int,		count		)
	),

	TP_fast_assign(
		tp_strcpy(name, dev_name(dev))
		tp_assign(reg, reg)
		tp_assign(count, count)
	),

	TP_printk("%s reg=%x count=%d", __get_str(name),
		  (unsigned int)__entry->reg,
		  (int)__entry->count)
)

DEFINE_EVENT(regmap_block, regmap_hw_read_start,

	TP_PROTO(struct device *dev, unsigned int reg, int count),

	TP_ARGS(dev, reg, count)
)

DEFINE_EVENT(regmap_block, regmap_hw_read_done,

	TP_PROTO(struct device *dev, unsigned int reg, int count),

	TP_ARGS(dev, reg, count)
)

DEFINE_EVENT(regmap_block, regmap_hw_write_start,

	TP_PROTO(struct device *dev, unsigned int reg, int count),

	TP_ARGS(dev, reg, count)
)

DEFINE_EVENT(regmap_block, regmap_hw_write_done,

	TP_PROTO(struct device *dev, unsigned int reg, int count),

	TP_ARGS(dev, reg, count)
)

TRACE_EVENT(regcache_sync,

	TP_PROTO(struct device *dev, const char *type,
		 const char *status),

	TP_ARGS(dev, type, status),

	TP_STRUCT__entry(
		__string(       name,           dev_name(dev)   )
		__string(	status,		status		)
		__string(	type,		type		)
	),

	TP_fast_assign(
		tp_strcpy(name, dev_name(dev))
		tp_strcpy(status, status)
		tp_strcpy(type, type)
	),

	TP_printk("%s type=%s status=%s", __get_str(name),
		  __get_str(type), __get_str(status))
)

#endif /* _TRACE_REGMAP_H */

/* This part must be outside protection */
#include "../../../probes/define_trace.h"
