/* SPDX-License-Identifier: MIT
 *
 * lttng/filter.h
 *
 * LTTng modules filter header.
 *
 * Copyright (C) 2010-2016 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _LTTNG_FILTER_H
#define _LTTNG_FILTER_H

#include <linux/kernel.h>

#include <lttng/events.h>
#include <lttng/filter-bytecode.h>

/* Filter stack length, in number of entries */
#define FILTER_STACK_LEN	10	/* includes 2 dummy */
#define FILTER_STACK_EMPTY	1

#define FILTER_MAX_DATA_LEN	65536

#ifdef DEBUG
#define dbg_printk(fmt, args...)				\
	printk(KERN_DEBUG "LTTng: [debug bytecode in %s:%s@%u] " fmt,		\
		__FILE__, __func__, __LINE__, ## args)
#else
#define dbg_printk(fmt, args...)				\
do {								\
	/* do nothing but check printf format */		\
	if (0)							\
		printk(KERN_DEBUG "LTTng: [debug bytecode in %s:%s@%u] " fmt,	\
			__FILE__, __func__, __LINE__, ## args);	\
} while (0)
#endif

/* Linked bytecode. Child of struct lttng_bytecode_runtime. */
struct bytecode_runtime {
	struct lttng_bytecode_runtime p;
	size_t data_len;
	size_t data_alloc_len;
	char *data;
	uint16_t len;
	char code[0];
};

enum entry_type {
	REG_S64,
	REG_DOUBLE,
	REG_STRING,
	REG_STAR_GLOB_STRING,
	REG_TYPE_UNKNOWN,
	REG_PTR,
};

enum load_type {
	LOAD_ROOT_CONTEXT,
	LOAD_ROOT_APP_CONTEXT,
	LOAD_ROOT_PAYLOAD,
	LOAD_OBJECT,
};

enum object_type {
	OBJECT_TYPE_S8,
	OBJECT_TYPE_S16,
	OBJECT_TYPE_S32,
	OBJECT_TYPE_S64,
	OBJECT_TYPE_U8,
	OBJECT_TYPE_U16,
	OBJECT_TYPE_U32,
	OBJECT_TYPE_U64,

	OBJECT_TYPE_DOUBLE,
	OBJECT_TYPE_STRING,
	OBJECT_TYPE_STRING_SEQUENCE,

	OBJECT_TYPE_SEQUENCE,
	OBJECT_TYPE_ARRAY,
	OBJECT_TYPE_STRUCT,
	OBJECT_TYPE_VARIANT,

	OBJECT_TYPE_DYNAMIC,
};

struct filter_get_index_data {
	uint64_t offset;	/* in bytes */
	size_t ctx_index;
	size_t array_len;
	struct {
		size_t len;
		enum object_type type;
		bool rev_bo;	/* reverse byte order */
	} elem;
};

/* Validation stack */
struct vstack_load {
	enum load_type type;
	enum object_type object_type;
	const struct lttng_event_field *field;
	bool rev_bo;	/* reverse byte order */
};

struct vstack_entry {
	enum entry_type type;
	struct vstack_load load;
};

struct vstack {
	int top;	/* top of stack */
	struct vstack_entry e[FILTER_STACK_LEN];
};

static inline
void vstack_init(struct vstack *stack)
{
	stack->top = -1;
}

static inline
struct vstack_entry *vstack_ax(struct vstack *stack)
{
	if (unlikely(stack->top < 0))
		return NULL;
	return &stack->e[stack->top];
}

static inline
struct vstack_entry *vstack_bx(struct vstack *stack)
{
	if (unlikely(stack->top < 1))
		return NULL;
	return &stack->e[stack->top - 1];
}

static inline
int vstack_push(struct vstack *stack)
{
	if (stack->top >= FILTER_STACK_LEN - 1) {
		printk(KERN_WARNING "LTTng: filter: Stack full\n");
		return -EINVAL;
	}
	++stack->top;
	return 0;
}

static inline
int vstack_pop(struct vstack *stack)
{
	if (unlikely(stack->top < 0)) {
		printk(KERN_WARNING "LTTng: filter: Stack empty\n");
		return -EINVAL;
	}
	stack->top--;
	return 0;
}

/* Execution stack */
enum estack_string_literal_type {
	ESTACK_STRING_LITERAL_TYPE_NONE,
	ESTACK_STRING_LITERAL_TYPE_PLAIN,
	ESTACK_STRING_LITERAL_TYPE_STAR_GLOB,
};

struct load_ptr {
	enum load_type type;
	enum object_type object_type;
	const void *ptr;
	bool rev_bo;
	/* Temporary place-holders for contexts. */
	union {
		int64_t s64;
		uint64_t u64;
		double d;
	} u;
	/*
	 * "field" is only needed when nested under a variant, in which
	 * case we cannot specialize the nested operations.
	 */
	const struct lttng_event_field *field;
};

struct estack_entry {
	union {
		int64_t v;

		struct {
			const char *str;
			const char __user *user_str;
			size_t seq_len;
			enum estack_string_literal_type literal_type;
			int user;		/* is string from userspace ? */
		} s;
		struct load_ptr ptr;
	} u;
};

struct estack {
	int top;	/* top of stack */
	struct estack_entry e[FILTER_STACK_LEN];
};

#define estack_ax_v	ax
#define estack_bx_v	bx

#define estack_ax(stack, top)					\
	({							\
		BUG_ON((top) <= FILTER_STACK_EMPTY);		\
		&(stack)->e[top];				\
	})

#define estack_bx(stack, top)					\
	({							\
		BUG_ON((top) <= FILTER_STACK_EMPTY + 1);	\
		&(stack)->e[(top) - 1];				\
	})

#define estack_push(stack, top, ax, bx)				\
	do {							\
		BUG_ON((top) >= FILTER_STACK_LEN - 1);		\
		(stack)->e[(top) - 1].u.v = (bx);		\
		(bx) = (ax);					\
		++(top);					\
	} while (0)

#define estack_pop(stack, top, ax, bx)				\
	do {							\
		BUG_ON((top) <= FILTER_STACK_EMPTY);		\
		(ax) = (bx);					\
		(bx) = (stack)->e[(top) - 2].u.v;		\
		(top)--;					\
	} while (0)

const char *lttng_filter_print_op(enum filter_op op);

int lttng_filter_validate_bytecode(struct bytecode_runtime *bytecode);
int lttng_filter_specialize_bytecode(const struct lttng_event_desc *event_desc,
		struct bytecode_runtime *bytecode);

uint64_t lttng_filter_false(void *filter_data,
		struct lttng_probe_ctx *lttng_probe_ctx,
		const char *filter_stack_data);
uint64_t lttng_filter_interpret_bytecode(void *filter_data,
		struct lttng_probe_ctx *lttng_probe_ctx,
		const char *filter_stack_data);

#endif /* _LTTNG_FILTER_H */
