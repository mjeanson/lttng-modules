/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng-context-preemptible.c
 *
 * LTTng preemptible context.
 *
 * Copyright (C) 2009-2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/irqflags.h>
#include <lttng-events.h>
#include <wrapper/ringbuffer/frontend_types.h>
#include <lttng-tracer.h>

/*
 * We nest twice in preempt disabling within LTTng: one nesting is done
 * by the instrumentation (tracepoint, kprobes, kretprobes, syscall
 * tracepoint), and the second is within the lib ring buffer
 * lib_ring_buffer_get_cpu().
 */
#define LTTNG_PREEMPT_DISABLE_NESTING	2

static
size_t preemptible_get_size(size_t offset)
{
	size_t size = 0;

	size += lib_ring_buffer_align(offset, lttng_alignof(uint8_t));
	size += sizeof(uint8_t);
	return size;
}

static
void preemptible_record(struct lttng_ctx_field *field,
		struct lib_ring_buffer_ctx *ctx,
		struct lttng_channel *chan)
{
	int pc = preempt_count();
	uint8_t preemptible = 0;

	WARN_ON_ONCE(pc < LTTNG_PREEMPT_DISABLE_NESTING);
	if (pc == LTTNG_PREEMPT_DISABLE_NESTING)
		preemptible = 1;
	lib_ring_buffer_align_ctx(ctx, lttng_alignof(preemptible));
	chan->ops->event_write(ctx, &preemptible, sizeof(preemptible));
}

static
void preemptible_get_value(struct lttng_ctx_field *field,
		struct lttng_probe_ctx *lttng_probe_ctx,
		union lttng_ctx_value *value)
{
	int pc = preempt_count();

	WARN_ON_ONCE(pc < LTTNG_PREEMPT_DISABLE_NESTING);
	if (pc == LTTNG_PREEMPT_DISABLE_NESTING)
		value->s64 = 1;
	else
		value->s64 = 0;
}

int lttng_add_preemptible_to_ctx(struct lttng_ctx **ctx)
{
	struct lttng_ctx_field *field;

	field = lttng_append_context(ctx);
	if (!field)
		return -ENOMEM;
	if (lttng_find_context(*ctx, "preemptible")) {
		lttng_remove_context_field(ctx, field);
		return -EEXIST;
	}
	field->event_field.name = "preemptible";
	field->event_field.type.atype = atype_integer;
	field->event_field.type.u.integer.size = sizeof(uint8_t) * CHAR_BIT;
	field->event_field.type.u.integer.alignment = lttng_alignof(uint8_t) * CHAR_BIT;
	field->event_field.type.u.integer.signedness = lttng_is_signed_type(uint8_t);
	field->event_field.type.u.integer.reverse_byte_order = 0;
	field->event_field.type.u.integer.base = 10;
	field->event_field.type.u.integer.encoding = lttng_encode_none;
	field->get_size = preemptible_get_size;
	field->record = preemptible_record;
	field->get_value = preemptible_get_value;
	lttng_context_update(*ctx);
	return 0;
}
EXPORT_SYMBOL_GPL(lttng_add_preemptible_to_ctx);
