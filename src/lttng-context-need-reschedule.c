/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng-context-need-reschedule.c
 *
 * LTTng need_reschedule context.
 *
 * Copyright (C) 2009-2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/irqflags.h>
#include <lttng/events.h>
#include <ringbuffer/frontend_types.h>
#include <wrapper/vmalloc.h>
#include <lttng/tracer.h>

static
size_t need_reschedule_get_size(size_t offset)
{
	size_t size = 0;

	size += lib_ring_buffer_align(offset, lttng_alignof(uint8_t));
	size += sizeof(uint8_t);
	return size;
}

static
void need_reschedule_record(struct lttng_ctx_field *field,
		struct lib_ring_buffer_ctx *ctx,
		struct lttng_channel *chan)
{
	uint8_t need_reschedule = test_tsk_need_resched(current);

	lib_ring_buffer_align_ctx(ctx, lttng_alignof(need_reschedule));
	chan->ops->event_write(ctx, &need_reschedule, sizeof(need_reschedule));
}

static
void need_reschedule_get_value(struct lttng_ctx_field *field,
		struct lttng_probe_ctx *lttng_probe_ctx,
		union lttng_ctx_value *value)
{
	value->s64 = test_tsk_need_resched(current);;
}

int lttng_add_need_reschedule_to_ctx(struct lttng_ctx **ctx)
{
	struct lttng_ctx_field *field;

	field = lttng_append_context(ctx);
	if (!field)
		return -ENOMEM;
	if (lttng_find_context(*ctx, "need_reschedule")) {
		lttng_remove_context_field(ctx, field);
		return -EEXIST;
	}
	field->event_field.name = "need_reschedule";
	field->event_field.type.atype = atype_integer;
	field->event_field.type.u.integer.size = sizeof(uint8_t) * CHAR_BIT;
	field->event_field.type.u.integer.alignment = lttng_alignof(uint8_t) * CHAR_BIT;
	field->event_field.type.u.integer.signedness = lttng_is_signed_type(uint8_t);
	field->event_field.type.u.integer.reverse_byte_order = 0;
	field->event_field.type.u.integer.base = 10;
	field->event_field.type.u.integer.encoding = lttng_encode_none;
	field->get_size = need_reschedule_get_size;
	field->record = need_reschedule_record;
	field->get_value = need_reschedule_get_value;
	lttng_context_update(*ctx);
	wrapper_vmalloc_sync_mappings();
	return 0;
}
EXPORT_SYMBOL_GPL(lttng_add_need_reschedule_to_ctx);
