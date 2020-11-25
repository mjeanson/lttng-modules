/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng-events.c
 *
 * Holds LTTng per-session event registry.
 *
 * Copyright (C) 2010-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

/*
 * This page_alloc.h wrapper needs to be included before gfpflags.h because it
 * overrides a function with a define.
 */
#include "wrapper/page_alloc.h"

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/utsname.h>
#include <linux/err.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <wrapper/file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/dmi.h>

#include <wrapper/uuid.h>
#include <wrapper/vmalloc.h>	/* for wrapper_vmalloc_sync_mappings() */
#include <wrapper/random.h>
#include <wrapper/tracepoint.h>
#include <wrapper/list.h>
#include <wrapper/types.h>
#include <lttng/kernel-version.h>
#include <lttng/events.h>
#include <lttng/tracer.h>
#include <lttng/event-notifier-notification.h>
#include <lttng/abi-old.h>
#include <lttng/endian.h>
#include <lttng/string-utils.h>
#include <lttng/utils.h>
#include <ringbuffer/backend.h>
#include <ringbuffer/frontend.h>
#include <wrapper/time.h>

#define METADATA_CACHE_DEFAULT_SIZE 4096

static LIST_HEAD(sessions);
static LIST_HEAD(event_notifier_groups);
static LIST_HEAD(lttng_transport_list);
/*
 * Protect the sessions and metadata caches.
 */
static DEFINE_MUTEX(sessions_mutex);
static struct kmem_cache *event_cache;
static struct kmem_cache *event_notifier_cache;

static void lttng_session_lazy_sync_event_enablers(struct lttng_session *session);
static void lttng_session_sync_event_enablers(struct lttng_session *session);
static void lttng_event_enabler_destroy(struct lttng_event_enabler *event_enabler);
static void lttng_event_notifier_enabler_destroy(struct lttng_event_notifier_enabler *event_notifier_enabler);
static void lttng_event_notifier_group_sync_enablers(struct lttng_event_notifier_group *event_notifier_group);

static void _lttng_event_destroy(struct lttng_event *event);
static void _lttng_event_notifier_destroy(struct lttng_event_notifier *event_notifier);
static void _lttng_channel_destroy(struct lttng_channel *chan);
static int _lttng_event_unregister(struct lttng_event *event);
static int _lttng_event_notifier_unregister(struct lttng_event_notifier *event_notifier);
static
int _lttng_event_metadata_statedump(struct lttng_session *session,
				  struct lttng_channel *chan,
				  struct lttng_event *event);
static
int _lttng_session_metadata_statedump(struct lttng_session *session);
static
void _lttng_metadata_channel_hangup(struct lttng_metadata_stream *stream);
static
int _lttng_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting);
static
int _lttng_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting);

void synchronize_trace(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0))
	synchronize_rcu();
#else
	synchronize_sched();
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,4,0))
#ifdef CONFIG_PREEMPT_RT_FULL
	synchronize_rcu();
#endif
#else /* (LINUX_VERSION_CODE >= KERNEL_VERSION(3,4,0)) */
#ifdef CONFIG_PREEMPT_RT
	synchronize_rcu();
#endif
#endif /* (LINUX_VERSION_CODE >= KERNEL_VERSION(3,4,0)) */
}

void lttng_lock_sessions(void)
{
	mutex_lock(&sessions_mutex);
}

void lttng_unlock_sessions(void)
{
	mutex_unlock(&sessions_mutex);
}

static struct lttng_transport *lttng_transport_find(const char *name)
{
	struct lttng_transport *transport;

	list_for_each_entry(transport, &lttng_transport_list, node) {
		if (!strcmp(transport->name, name))
			return transport;
	}
	return NULL;
}

/*
 * Called with sessions lock held.
 */
int lttng_session_active(void)
{
	struct lttng_session *iter;

	list_for_each_entry(iter, &sessions, list) {
		if (iter->active)
			return 1;
	}
	return 0;
}

struct lttng_session *lttng_session_create(void)
{
	struct lttng_session *session;
	struct lttng_metadata_cache *metadata_cache;
	int i;

	mutex_lock(&sessions_mutex);
	session = lttng_kvzalloc(sizeof(struct lttng_session), GFP_KERNEL);
	if (!session)
		goto err;
	INIT_LIST_HEAD(&session->chan);
	INIT_LIST_HEAD(&session->events);
	lttng_guid_gen(&session->uuid);

	metadata_cache = kzalloc(sizeof(struct lttng_metadata_cache),
			GFP_KERNEL);
	if (!metadata_cache)
		goto err_free_session;
	metadata_cache->data = vzalloc(METADATA_CACHE_DEFAULT_SIZE);
	if (!metadata_cache->data)
		goto err_free_cache;
	metadata_cache->cache_alloc = METADATA_CACHE_DEFAULT_SIZE;
	kref_init(&metadata_cache->refcount);
	mutex_init(&metadata_cache->lock);
	session->metadata_cache = metadata_cache;
	INIT_LIST_HEAD(&metadata_cache->metadata_stream);
	memcpy(&metadata_cache->uuid, &session->uuid,
		sizeof(metadata_cache->uuid));
	INIT_LIST_HEAD(&session->enablers_head);
	for (i = 0; i < LTTNG_EVENT_HT_SIZE; i++)
		INIT_HLIST_HEAD(&session->events_ht.table[i]);
	list_add(&session->list, &sessions);
	session->pid_tracker.session = session;
	session->pid_tracker.tracker_type = TRACKER_PID;
	session->vpid_tracker.session = session;
	session->vpid_tracker.tracker_type = TRACKER_VPID;
	session->uid_tracker.session = session;
	session->uid_tracker.tracker_type = TRACKER_UID;
	session->vuid_tracker.session = session;
	session->vuid_tracker.tracker_type = TRACKER_VUID;
	session->gid_tracker.session = session;
	session->gid_tracker.tracker_type = TRACKER_GID;
	session->vgid_tracker.session = session;
	session->vgid_tracker.tracker_type = TRACKER_VGID;
	mutex_unlock(&sessions_mutex);
	return session;

err_free_cache:
	kfree(metadata_cache);
err_free_session:
	lttng_kvfree(session);
err:
	mutex_unlock(&sessions_mutex);
	return NULL;
}

struct lttng_event_notifier_group *lttng_event_notifier_group_create(void)
{
	struct lttng_transport *transport = NULL;
	struct lttng_event_notifier_group *event_notifier_group;
	const char *transport_name = "relay-event-notifier";
	size_t subbuf_size = 4096;	//TODO
	size_t num_subbuf = 16;		//TODO
	unsigned int switch_timer_interval = 0;
	unsigned int read_timer_interval = 0;
	int i;

	mutex_lock(&sessions_mutex);

	transport = lttng_transport_find(transport_name);
	if (!transport) {
		printk(KERN_WARNING "LTTng: transport %s not found\n",
		       transport_name);
		goto notransport;
	}
	if (!try_module_get(transport->owner)) {
		printk(KERN_WARNING "LTTng: Can't lock transport %s module.\n",
		       transport_name);
		goto notransport;
	}

	event_notifier_group = lttng_kvzalloc(sizeof(struct lttng_event_notifier_group),
				       GFP_KERNEL);
	if (!event_notifier_group)
		goto nomem;

	/*
	 * Initialize the ring buffer used to store event notifier
	 * notifications.
	 */
	event_notifier_group->ops = &transport->ops;
	event_notifier_group->chan = transport->ops.channel_create(
			transport_name, event_notifier_group, NULL,
			subbuf_size, num_subbuf, switch_timer_interval,
			read_timer_interval);
	if (!event_notifier_group->chan)
		goto create_error;

	event_notifier_group->transport = transport;

	INIT_LIST_HEAD(&event_notifier_group->enablers_head);
	INIT_LIST_HEAD(&event_notifier_group->event_notifiers_head);
	for (i = 0; i < LTTNG_EVENT_NOTIFIER_HT_SIZE; i++)
		INIT_HLIST_HEAD(&event_notifier_group->event_notifiers_ht.table[i]);

	list_add(&event_notifier_group->node, &event_notifier_groups);

	mutex_unlock(&sessions_mutex);

	return event_notifier_group;

create_error:
	lttng_kvfree(event_notifier_group);
nomem:
	if (transport)
		module_put(transport->owner);
notransport:
	mutex_unlock(&sessions_mutex);
	return NULL;
}

void metadata_cache_destroy(struct kref *kref)
{
	struct lttng_metadata_cache *cache =
		container_of(kref, struct lttng_metadata_cache, refcount);
	vfree(cache->data);
	kfree(cache);
}

void lttng_session_destroy(struct lttng_session *session)
{
	struct lttng_channel *chan, *tmpchan;
	struct lttng_event *event, *tmpevent;
	struct lttng_metadata_stream *metadata_stream;
	struct lttng_event_enabler *event_enabler, *tmp_event_enabler;
	int ret;

	mutex_lock(&sessions_mutex);
	WRITE_ONCE(session->active, 0);
	list_for_each_entry(chan, &session->chan, list) {
		ret = lttng_syscalls_unregister_event(chan);
		WARN_ON(ret);
	}
	list_for_each_entry(event, &session->events, list) {
		ret = _lttng_event_unregister(event);
		WARN_ON(ret);
	}
	synchronize_trace();	/* Wait for in-flight events to complete */
	list_for_each_entry(chan, &session->chan, list) {
		ret = lttng_syscalls_destroy_event(chan);
		WARN_ON(ret);
	}
	list_for_each_entry_safe(event_enabler, tmp_event_enabler,
			&session->enablers_head, node)
		lttng_event_enabler_destroy(event_enabler);
	list_for_each_entry_safe(event, tmpevent, &session->events, list)
		_lttng_event_destroy(event);
	list_for_each_entry_safe(chan, tmpchan, &session->chan, list) {
		BUG_ON(chan->channel_type == METADATA_CHANNEL);
		_lttng_channel_destroy(chan);
	}
	mutex_lock(&session->metadata_cache->lock);
	list_for_each_entry(metadata_stream, &session->metadata_cache->metadata_stream, list)
		_lttng_metadata_channel_hangup(metadata_stream);
	mutex_unlock(&session->metadata_cache->lock);
	lttng_id_tracker_destroy(&session->pid_tracker, false);
	lttng_id_tracker_destroy(&session->vpid_tracker, false);
	lttng_id_tracker_destroy(&session->uid_tracker, false);
	lttng_id_tracker_destroy(&session->vuid_tracker, false);
	lttng_id_tracker_destroy(&session->gid_tracker, false);
	lttng_id_tracker_destroy(&session->vgid_tracker, false);
	kref_put(&session->metadata_cache->refcount, metadata_cache_destroy);
	list_del(&session->list);
	mutex_unlock(&sessions_mutex);
	lttng_kvfree(session);
}

void lttng_event_notifier_group_destroy(
		struct lttng_event_notifier_group *event_notifier_group)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler, *tmp_event_notifier_enabler;
	struct lttng_event_notifier *event_notifier, *tmpevent_notifier;
	int ret;

	if (!event_notifier_group)
		return;

	mutex_lock(&sessions_mutex);

	ret = lttng_syscalls_unregister_event_notifier(event_notifier_group);
	WARN_ON(ret);

	list_for_each_entry_safe(event_notifier, tmpevent_notifier,
			&event_notifier_group->event_notifiers_head, list) {
		ret = _lttng_event_notifier_unregister(event_notifier);
		WARN_ON(ret);
	}

	/* Wait for in-flight event notifier to complete */
	synchronize_trace();

	irq_work_sync(&event_notifier_group->wakeup_pending);

	kfree(event_notifier_group->sc_filter);

	list_for_each_entry_safe(event_notifier_enabler, tmp_event_notifier_enabler,
			&event_notifier_group->enablers_head, node)
		lttng_event_notifier_enabler_destroy(event_notifier_enabler);

	list_for_each_entry_safe(event_notifier, tmpevent_notifier,
			&event_notifier_group->event_notifiers_head, list)
		_lttng_event_notifier_destroy(event_notifier);

	event_notifier_group->ops->channel_destroy(event_notifier_group->chan);
	module_put(event_notifier_group->transport->owner);
	list_del(&event_notifier_group->node);

	mutex_unlock(&sessions_mutex);
	lttng_kvfree(event_notifier_group);
}

int lttng_session_statedump(struct lttng_session *session)
{
	int ret;

	mutex_lock(&sessions_mutex);
	ret = lttng_statedump_start(session);
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_session_enable(struct lttng_session *session)
{
	int ret = 0;
	struct lttng_channel *chan;

	mutex_lock(&sessions_mutex);
	if (session->active) {
		ret = -EBUSY;
		goto end;
	}

	/* Set transient enabler state to "enabled" */
	session->tstate = 1;

	/* We need to sync enablers with session before activation. */
	lttng_session_sync_event_enablers(session);

	/*
	 * Snapshot the number of events per channel to know the type of header
	 * we need to use.
	 */
	list_for_each_entry(chan, &session->chan, list) {
		if (chan->header_type)
			continue;		/* don't change it if session stop/restart */
		if (chan->free_event_id < 31)
			chan->header_type = 1;	/* compact */
		else
			chan->header_type = 2;	/* large */
	}

	/* Clear each stream's quiescent state. */
	list_for_each_entry(chan, &session->chan, list) {
		if (chan->channel_type != METADATA_CHANNEL)
			lib_ring_buffer_clear_quiescent_channel(chan->chan);
	}

	WRITE_ONCE(session->active, 1);
	WRITE_ONCE(session->been_active, 1);
	ret = _lttng_session_metadata_statedump(session);
	if (ret) {
		WRITE_ONCE(session->active, 0);
		goto end;
	}
	ret = lttng_statedump_start(session);
	if (ret)
		WRITE_ONCE(session->active, 0);
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_session_disable(struct lttng_session *session)
{
	int ret = 0;
	struct lttng_channel *chan;

	mutex_lock(&sessions_mutex);
	if (!session->active) {
		ret = -EBUSY;
		goto end;
	}
	WRITE_ONCE(session->active, 0);

	/* Set transient enabler state to "disabled" */
	session->tstate = 0;
	lttng_session_sync_event_enablers(session);

	/* Set each stream's quiescent state. */
	list_for_each_entry(chan, &session->chan, list) {
		if (chan->channel_type != METADATA_CHANNEL)
			lib_ring_buffer_set_quiescent_channel(chan->chan);
	}
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_session_metadata_regenerate(struct lttng_session *session)
{
	int ret = 0;
	struct lttng_channel *chan;
	struct lttng_event *event;
	struct lttng_metadata_cache *cache = session->metadata_cache;
	struct lttng_metadata_stream *stream;

	mutex_lock(&sessions_mutex);
	if (!session->active) {
		ret = -EBUSY;
		goto end;
	}

	mutex_lock(&cache->lock);
	memset(cache->data, 0, cache->cache_alloc);
	cache->metadata_written = 0;
	cache->version++;
	list_for_each_entry(stream, &session->metadata_cache->metadata_stream, list) {
		stream->metadata_out = 0;
		stream->metadata_in = 0;
	}
	mutex_unlock(&cache->lock);

	session->metadata_dumped = 0;
	list_for_each_entry(chan, &session->chan, list) {
		chan->metadata_dumped = 0;
	}

	list_for_each_entry(event, &session->events, list) {
		event->metadata_dumped = 0;
	}

	ret = _lttng_session_metadata_statedump(session);

end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_channel_enable(struct lttng_channel *channel)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (channel->channel_type == METADATA_CHANNEL) {
		ret = -EPERM;
		goto end;
	}
	if (channel->enabled) {
		ret = -EEXIST;
		goto end;
	}
	/* Set transient enabler state to "enabled" */
	channel->tstate = 1;
	lttng_session_sync_event_enablers(channel->session);
	/* Set atomically the state to "enabled" */
	WRITE_ONCE(channel->enabled, 1);
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_channel_disable(struct lttng_channel *channel)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (channel->channel_type == METADATA_CHANNEL) {
		ret = -EPERM;
		goto end;
	}
	if (!channel->enabled) {
		ret = -EEXIST;
		goto end;
	}
	/* Set atomically the state to "disabled" */
	WRITE_ONCE(channel->enabled, 0);
	/* Set transient enabler state to "enabled" */
	channel->tstate = 0;
	lttng_session_sync_event_enablers(channel->session);
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_event_enable(struct lttng_event *event)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (event->chan->channel_type == METADATA_CHANNEL) {
		ret = -EPERM;
		goto end;
	}
	if (event->enabled) {
		ret = -EEXIST;
		goto end;
	}
	switch (event->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
	case LTTNG_KERNEL_SYSCALL:
		ret = -EINVAL;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
	case LTTNG_KERNEL_NOOP:
		WRITE_ONCE(event->enabled, 1);
		break;
	case LTTNG_KERNEL_KRETPROBE:
		ret = lttng_kretprobes_event_enable_state(event, 1);
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through. */
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
	}
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_event_disable(struct lttng_event *event)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (event->chan->channel_type == METADATA_CHANNEL) {
		ret = -EPERM;
		goto end;
	}
	if (!event->enabled) {
		ret = -EEXIST;
		goto end;
	}
	switch (event->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
	case LTTNG_KERNEL_SYSCALL:
		ret = -EINVAL;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
	case LTTNG_KERNEL_NOOP:
		WRITE_ONCE(event->enabled, 0);
		break;
	case LTTNG_KERNEL_KRETPROBE:
		ret = lttng_kretprobes_event_enable_state(event, 0);
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through. */
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
	}
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_event_notifier_enable(struct lttng_event_notifier *event_notifier)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (event_notifier->enabled) {
		ret = -EEXIST;
		goto end;
	}
	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
	case LTTNG_KERNEL_SYSCALL:
		ret = -EINVAL;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
		WRITE_ONCE(event_notifier->enabled, 1);
		break;
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_KRETPROBE:
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
	}
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_event_notifier_disable(struct lttng_event_notifier *event_notifier)
{
	int ret = 0;

	mutex_lock(&sessions_mutex);
	if (!event_notifier->enabled) {
		ret = -EEXIST;
		goto end;
	}
	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
	case LTTNG_KERNEL_SYSCALL:
		ret = -EINVAL;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
		WRITE_ONCE(event_notifier->enabled, 0);
		break;
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_KRETPROBE:
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
	}
end:
	mutex_unlock(&sessions_mutex);
	return ret;
}

struct lttng_channel *lttng_channel_create(struct lttng_session *session,
				       const char *transport_name,
				       void *buf_addr,
				       size_t subbuf_size, size_t num_subbuf,
				       unsigned int switch_timer_interval,
				       unsigned int read_timer_interval,
				       enum channel_type channel_type)
{
	struct lttng_channel *chan;
	struct lttng_transport *transport = NULL;

	mutex_lock(&sessions_mutex);
	if (session->been_active && channel_type != METADATA_CHANNEL)
		goto active;	/* Refuse to add channel to active session */
	transport = lttng_transport_find(transport_name);
	if (!transport) {
		printk(KERN_WARNING "LTTng: transport %s not found\n",
		       transport_name);
		goto notransport;
	}
	if (!try_module_get(transport->owner)) {
		printk(KERN_WARNING "LTTng: Can't lock transport module.\n");
		goto notransport;
	}
	chan = kzalloc(sizeof(struct lttng_channel), GFP_KERNEL);
	if (!chan)
		goto nomem;
	chan->session = session;
	chan->id = session->free_chan_id++;
	chan->ops = &transport->ops;
	/*
	 * Note: the channel creation op already writes into the packet
	 * headers. Therefore the "chan" information used as input
	 * should be already accessible.
	 */
	chan->chan = transport->ops.channel_create(transport_name,
			chan, buf_addr, subbuf_size, num_subbuf,
			switch_timer_interval, read_timer_interval);
	if (!chan->chan)
		goto create_error;
	chan->tstate = 1;
	chan->enabled = 1;
	chan->transport = transport;
	chan->channel_type = channel_type;
	list_add(&chan->list, &session->chan);
	mutex_unlock(&sessions_mutex);
	return chan;

create_error:
	kfree(chan);
nomem:
	if (transport)
		module_put(transport->owner);
notransport:
active:
	mutex_unlock(&sessions_mutex);
	return NULL;
}

/*
 * Only used internally at session destruction for per-cpu channels, and
 * when metadata channel is released.
 * Needs to be called with sessions mutex held.
 */
static
void _lttng_channel_destroy(struct lttng_channel *chan)
{
	chan->ops->channel_destroy(chan->chan);
	module_put(chan->transport->owner);
	list_del(&chan->list);
	lttng_destroy_context(chan->ctx);
	kfree(chan);
}

void lttng_metadata_channel_destroy(struct lttng_channel *chan)
{
	BUG_ON(chan->channel_type != METADATA_CHANNEL);

	/* Protect the metadata cache with the sessions_mutex. */
	mutex_lock(&sessions_mutex);
	_lttng_channel_destroy(chan);
	mutex_unlock(&sessions_mutex);
}
EXPORT_SYMBOL_GPL(lttng_metadata_channel_destroy);

static
void _lttng_metadata_channel_hangup(struct lttng_metadata_stream *stream)
{
	stream->finalized = 1;
	wake_up_interruptible(&stream->read_wait);
}

/*
 * Supports event creation while tracing session is active.
 * Needs to be called with sessions mutex held.
 */
struct lttng_event *_lttng_event_create(struct lttng_channel *chan,
				struct lttng_kernel_event *event_param,
				void *filter,
				const struct lttng_event_desc *event_desc,
				enum lttng_kernel_instrumentation itype)
{
	struct lttng_session *session = chan->session;
	struct lttng_event *event;
	const char *event_name;
	struct hlist_head *head;
	int ret;

	if (chan->free_event_id == -1U) {
		ret = -EMFILE;
		goto full;
	}

	switch (itype) {
	case LTTNG_KERNEL_TRACEPOINT:
		event_name = event_desc->name;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
		event_name = event_param->name;
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through. */
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto type_error;
	}

	head = utils_borrow_hash_table_bucket(session->events_ht.table,
		LTTNG_EVENT_HT_SIZE, event_name);
	lttng_hlist_for_each_entry(event, head, hlist) {
		WARN_ON_ONCE(!event->desc);
		if (!strncmp(event->desc->name, event_name,
					LTTNG_KERNEL_SYM_NAME_LEN - 1)
				&& chan == event->chan) {
			ret = -EEXIST;
			goto exist;
		}
	}

	event = kmem_cache_zalloc(event_cache, GFP_KERNEL);
	if (!event) {
		ret = -ENOMEM;
		goto cache_error;
	}
	event->chan = chan;
	event->filter = filter;
	event->id = chan->free_event_id++;
	event->instrumentation = itype;
	event->evtype = LTTNG_TYPE_EVENT;
	INIT_LIST_HEAD(&event->filter_bytecode_runtime_head);
	INIT_LIST_HEAD(&event->enablers_ref_head);

	switch (itype) {
	case LTTNG_KERNEL_TRACEPOINT:
		/* Event will be enabled by enabler sync. */
		event->enabled = 0;
		event->registered = 0;
		event->desc = lttng_event_desc_get(event_name);
		if (!event->desc) {
			ret = -ENOENT;
			goto register_error;
		}
		/* Populate lttng_event structure before event registration. */
		smp_wmb();
		break;
	case LTTNG_KERNEL_KPROBE:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event->enabled = 0;
		event->registered = 1;
		/*
		 * Populate lttng_event structure before event
		 * registration.
		 */
		smp_wmb();
		ret = lttng_kprobes_register_event(event_name,
				event_param->u.kprobe.symbol_name,
				event_param->u.kprobe.offset,
				event_param->u.kprobe.addr,
				event);
		if (ret) {
			ret = -EINVAL;
			goto register_error;
		}
		ret = try_module_get(event->desc->owner);
		WARN_ON_ONCE(!ret);
		break;
	case LTTNG_KERNEL_KRETPROBE:
	{
		struct lttng_event *event_return;

		/* kretprobe defines 2 events */
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event->enabled = 0;
		event->registered = 1;
		event_return =
			kmem_cache_zalloc(event_cache, GFP_KERNEL);
		if (!event_return) {
			ret = -ENOMEM;
			goto register_error;
		}
		event_return->chan = chan;
		event_return->filter = filter;
		event_return->id = chan->free_event_id++;
		event_return->enabled = 0;
		event_return->registered = 1;
		event_return->instrumentation = itype;
		/*
		 * Populate lttng_event structure before kretprobe registration.
		 */
		smp_wmb();
		ret = lttng_kretprobes_register(event_name,
				event_param->u.kretprobe.symbol_name,
				event_param->u.kretprobe.offset,
				event_param->u.kretprobe.addr,
				event, event_return);
		if (ret) {
			kmem_cache_free(event_cache, event_return);
			ret = -EINVAL;
			goto register_error;
		}
		/* Take 2 refs on the module: one per event. */
		ret = try_module_get(event->desc->owner);
		WARN_ON_ONCE(!ret);
		ret = try_module_get(event->desc->owner);
		WARN_ON_ONCE(!ret);
		ret = _lttng_event_metadata_statedump(chan->session, chan,
						    event_return);
		WARN_ON_ONCE(ret > 0);
		if (ret) {
			kmem_cache_free(event_cache, event_return);
			module_put(event->desc->owner);
			module_put(event->desc->owner);
			goto statedump_error;
		}
		list_add(&event_return->list, &chan->session->events);
		break;
	}
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event->enabled = 0;
		event->registered = 0;
		event->desc = event_desc;
		switch (event_param->u.syscall.entryexit) {
		case LTTNG_KERNEL_SYSCALL_ENTRYEXIT:
			ret = -EINVAL;
			goto register_error;
		case LTTNG_KERNEL_SYSCALL_ENTRY:
			event->u.syscall.entryexit = LTTNG_SYSCALL_ENTRY;
			break;
		case LTTNG_KERNEL_SYSCALL_EXIT:
			event->u.syscall.entryexit = LTTNG_SYSCALL_EXIT;
			break;
		}
		switch (event_param->u.syscall.abi) {
		case LTTNG_KERNEL_SYSCALL_ABI_ALL:
			ret = -EINVAL;
			goto register_error;
		case LTTNG_KERNEL_SYSCALL_ABI_NATIVE:
			event->u.syscall.abi = LTTNG_SYSCALL_ABI_NATIVE;
			break;
		case LTTNG_KERNEL_SYSCALL_ABI_COMPAT:
			event->u.syscall.abi = LTTNG_SYSCALL_ABI_COMPAT;
			break;
		}
		if (!event->desc) {
			ret = -EINVAL;
			goto register_error;
		}
		break;
	case LTTNG_KERNEL_UPROBE:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event->enabled = 0;
		event->registered = 1;

		/*
		 * Populate lttng_event structure before event
		 * registration.
		 */
		smp_wmb();

		ret = lttng_uprobes_register_event(event_param->name,
				event_param->u.uprobe.fd,
				event);
		if (ret)
			goto register_error;
		ret = try_module_get(event->desc->owner);
		WARN_ON_ONCE(!ret);
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through */
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto register_error;
	}
	ret = _lttng_event_metadata_statedump(chan->session, chan, event);
	WARN_ON_ONCE(ret > 0);
	if (ret) {
		goto statedump_error;
	}
	hlist_add_head(&event->hlist, head);
	list_add(&event->list, &chan->session->events);
	return event;

statedump_error:
	/* If a statedump error occurs, events will not be readable. */
register_error:
	kmem_cache_free(event_cache, event);
cache_error:
exist:
type_error:
full:
	return ERR_PTR(ret);
}

struct lttng_event_notifier *_lttng_event_notifier_create(
		const struct lttng_event_desc *event_desc,
		uint64_t token, struct lttng_event_notifier_group *event_notifier_group,
		struct lttng_kernel_event_notifier *event_notifier_param,
		void *filter, enum lttng_kernel_instrumentation itype)
{
	struct lttng_event_notifier *event_notifier;
	const char *event_name;
	struct hlist_head *head;
	int ret;

	switch (itype) {
	case LTTNG_KERNEL_TRACEPOINT:
		event_name = event_desc->name;
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
	case LTTNG_KERNEL_SYSCALL:
		event_name = event_notifier_param->event.name;
		break;
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto type_error;
	}

	head = utils_borrow_hash_table_bucket(event_notifier_group->event_notifiers_ht.table,
		LTTNG_EVENT_NOTIFIER_HT_SIZE, event_name);
	lttng_hlist_for_each_entry(event_notifier, head, hlist) {
		WARN_ON_ONCE(!event_notifier->desc);
		if (!strncmp(event_notifier->desc->name, event_name,
					LTTNG_KERNEL_SYM_NAME_LEN - 1)
				&& event_notifier_group == event_notifier->group
				&& token == event_notifier->user_token) {
			ret = -EEXIST;
			goto exist;
		}
	}

	event_notifier = kmem_cache_zalloc(event_notifier_cache, GFP_KERNEL);
	if (!event_notifier) {
		ret = -ENOMEM;
		goto cache_error;
	}

	event_notifier->group = event_notifier_group;
	event_notifier->user_token = token;
	event_notifier->filter = filter;
	event_notifier->instrumentation = itype;
	event_notifier->evtype = LTTNG_TYPE_EVENT;
	event_notifier->send_notification = lttng_event_notifier_notification_send;
	INIT_LIST_HEAD(&event_notifier->filter_bytecode_runtime_head);
	INIT_LIST_HEAD(&event_notifier->enablers_ref_head);

	switch (itype) {
	case LTTNG_KERNEL_TRACEPOINT:
		/* Event will be enabled by enabler sync. */
		event_notifier->enabled = 0;
		event_notifier->registered = 0;
		event_notifier->desc = lttng_event_desc_get(event_name);
		if (!event_notifier->desc) {
			ret = -ENOENT;
			goto register_error;
		}
		/* Populate lttng_event_notifier structure before event registration. */
		smp_wmb();
		break;
	case LTTNG_KERNEL_KPROBE:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event_notifier->enabled = 0;
		event_notifier->registered = 1;
		/*
		 * Populate lttng_event_notifier structure before event
		 * registration.
		 */
		smp_wmb();
		ret = lttng_kprobes_register_event_notifier(
				event_notifier_param->event.u.kprobe.symbol_name,
				event_notifier_param->event.u.kprobe.offset,
				event_notifier_param->event.u.kprobe.addr,
				event_notifier);
		if (ret) {
			ret = -EINVAL;
			goto register_error;
		}
		ret = try_module_get(event_notifier->desc->owner);
		WARN_ON_ONCE(!ret);
		break;
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event_notifier->enabled = 0;
		event_notifier->registered = 0;
		event_notifier->desc = event_desc;
		switch (event_notifier_param->event.u.syscall.entryexit) {
		case LTTNG_KERNEL_SYSCALL_ENTRYEXIT:
			ret = -EINVAL;
			goto register_error;
		case LTTNG_KERNEL_SYSCALL_ENTRY:
			event_notifier->u.syscall.entryexit = LTTNG_SYSCALL_ENTRY;
			break;
		case LTTNG_KERNEL_SYSCALL_EXIT:
			event_notifier->u.syscall.entryexit = LTTNG_SYSCALL_EXIT;
			break;
		}
		switch (event_notifier_param->event.u.syscall.abi) {
		case LTTNG_KERNEL_SYSCALL_ABI_ALL:
			ret = -EINVAL;
			goto register_error;
		case LTTNG_KERNEL_SYSCALL_ABI_NATIVE:
			event_notifier->u.syscall.abi = LTTNG_SYSCALL_ABI_NATIVE;
			break;
		case LTTNG_KERNEL_SYSCALL_ABI_COMPAT:
			event_notifier->u.syscall.abi = LTTNG_SYSCALL_ABI_COMPAT;
			break;
		}

		if (!event_notifier->desc) {
			ret = -EINVAL;
			goto register_error;
		}
		break;
	case LTTNG_KERNEL_UPROBE:
		/*
		 * Needs to be explicitly enabled after creation, since
		 * we may want to apply filters.
		 */
		event_notifier->enabled = 0;
		event_notifier->registered = 1;

		/*
		 * Populate lttng_event_notifier structure before
		 * event_notifier registration.
		 */
		smp_wmb();

		ret = lttng_uprobes_register_event_notifier(
				event_notifier_param->event.name,
				event_notifier_param->event.u.uprobe.fd,
				event_notifier);
		if (ret)
			goto register_error;
		ret = try_module_get(event_notifier->desc->owner);
		WARN_ON_ONCE(!ret);
		break;
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_FUNCTION:
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto register_error;
	}

	list_add(&event_notifier->list, &event_notifier_group->event_notifiers_head);
	hlist_add_head(&event_notifier->hlist, head);
	return event_notifier;

register_error:
	kmem_cache_free(event_notifier_cache, event_notifier);
cache_error:
exist:
type_error:
	return ERR_PTR(ret);
}

struct lttng_event *lttng_event_create(struct lttng_channel *chan,
				struct lttng_kernel_event *event_param,
				void *filter,
				const struct lttng_event_desc *event_desc,
				enum lttng_kernel_instrumentation itype)
{
	struct lttng_event *event;

	mutex_lock(&sessions_mutex);
	event = _lttng_event_create(chan, event_param, filter, event_desc,
				itype);
	mutex_unlock(&sessions_mutex);
	return event;
}

struct lttng_event_notifier *lttng_event_notifier_create(
		const struct lttng_event_desc *event_desc,
		uint64_t id, struct lttng_event_notifier_group *event_notifier_group,
		struct lttng_kernel_event_notifier *event_notifier_param,
		void *filter, enum lttng_kernel_instrumentation itype)
{
	struct lttng_event_notifier *event_notifier;

	mutex_lock(&sessions_mutex);
	event_notifier = _lttng_event_notifier_create(event_desc, id,
		event_notifier_group, event_notifier_param, filter, itype);
	mutex_unlock(&sessions_mutex);
	return event_notifier;
}

/* Only used for tracepoints for now. */
static
void register_event(struct lttng_event *event)
{
	const struct lttng_event_desc *desc;
	int ret = -EINVAL;

	if (event->registered)
		return;

	desc = event->desc;
	switch (event->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		ret = lttng_wrapper_tracepoint_probe_register(desc->kname,
						  desc->probe_callback,
						  event);
		break;
	case LTTNG_KERNEL_SYSCALL:
		ret = lttng_syscall_filter_enable_event(event->chan, event);
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_NOOP:
		ret = 0;
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through */
	default:
		WARN_ON_ONCE(1);
	}
	if (!ret)
		event->registered = 1;
}

/*
 * Only used internally at session destruction.
 */
int _lttng_event_unregister(struct lttng_event *event)
{
	const struct lttng_event_desc *desc;
	int ret = -EINVAL;

	if (!event->registered)
		return 0;

	desc = event->desc;
	switch (event->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		ret = lttng_wrapper_tracepoint_probe_unregister(event->desc->kname,
						  event->desc->probe_callback,
						  event);
		break;
	case LTTNG_KERNEL_KPROBE:
		lttng_kprobes_unregister_event(event);
		ret = 0;
		break;
	case LTTNG_KERNEL_KRETPROBE:
		lttng_kretprobes_unregister(event);
		ret = 0;
		break;
	case LTTNG_KERNEL_SYSCALL:
		ret = lttng_syscall_filter_disable_event(event->chan, event);
		break;
	case LTTNG_KERNEL_NOOP:
		ret = 0;
		break;
	case LTTNG_KERNEL_UPROBE:
		lttng_uprobes_unregister_event(event);
		ret = 0;
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through */
	default:
		WARN_ON_ONCE(1);
	}
	if (!ret)
		event->registered = 0;
	return ret;
}

/* Only used for tracepoints for now. */
static
void register_event_notifier(struct lttng_event_notifier *event_notifier)
{
	const struct lttng_event_desc *desc;
	int ret = -EINVAL;

	if (event_notifier->registered)
		return;

	desc = event_notifier->desc;
	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		ret = lttng_wrapper_tracepoint_probe_register(desc->kname,
						  desc->event_notifier_callback,
						  event_notifier);
		break;
	case LTTNG_KERNEL_SYSCALL:
		ret = lttng_syscall_filter_enable_event_notifier(event_notifier);
		break;
	case LTTNG_KERNEL_KPROBE:
	case LTTNG_KERNEL_UPROBE:
		ret = 0;
		break;
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	default:
		WARN_ON_ONCE(1);
	}
	if (!ret)
		event_notifier->registered = 1;
}

static
int _lttng_event_notifier_unregister(
		struct lttng_event_notifier *event_notifier)
{
	const struct lttng_event_desc *desc;
	int ret = -EINVAL;

	if (!event_notifier->registered)
		return 0;

	desc = event_notifier->desc;
	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		ret = lttng_wrapper_tracepoint_probe_unregister(event_notifier->desc->kname,
						  event_notifier->desc->event_notifier_callback,
						  event_notifier);
		break;
	case LTTNG_KERNEL_KPROBE:
		lttng_kprobes_unregister_event_notifier(event_notifier);
		ret = 0;
		break;
	case LTTNG_KERNEL_UPROBE:
		lttng_uprobes_unregister_event_notifier(event_notifier);
		ret = 0;
		break;
	case LTTNG_KERNEL_SYSCALL:
		ret = lttng_syscall_filter_disable_event_notifier(event_notifier);
		break;
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	default:
		WARN_ON_ONCE(1);
	}
	if (!ret)
		event_notifier->registered = 0;
	return ret;
}

/*
 * Only used internally at session destruction.
 */
static
void _lttng_event_destroy(struct lttng_event *event)
{
	switch (event->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		lttng_event_desc_put(event->desc);
		break;
	case LTTNG_KERNEL_KPROBE:
		module_put(event->desc->owner);
		lttng_kprobes_destroy_event_private(event);
		break;
	case LTTNG_KERNEL_KRETPROBE:
		module_put(event->desc->owner);
		lttng_kretprobes_destroy_private(event);
		break;
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
		break;
	case LTTNG_KERNEL_UPROBE:
		module_put(event->desc->owner);
		lttng_uprobes_destroy_event_private(event);
		break;
	case LTTNG_KERNEL_FUNCTION:	/* Fall-through */
	default:
		WARN_ON_ONCE(1);
	}
	list_del(&event->list);
	lttng_destroy_context(event->ctx);
	kmem_cache_free(event_cache, event);
}

/*
 * Only used internally at session destruction.
 */
static
void _lttng_event_notifier_destroy(struct lttng_event_notifier *event_notifier)
{
	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		lttng_event_desc_put(event_notifier->desc);
		break;
	case LTTNG_KERNEL_KPROBE:
		module_put(event_notifier->desc->owner);
		lttng_kprobes_destroy_event_notifier_private(event_notifier);
		break;
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
		break;
	case LTTNG_KERNEL_UPROBE:
		module_put(event_notifier->desc->owner);
		lttng_uprobes_destroy_event_notifier_private(event_notifier);
		break;
	case LTTNG_KERNEL_KRETPROBE:
	case LTTNG_KERNEL_FUNCTION:
	default:
		WARN_ON_ONCE(1);
	}
	list_del(&event_notifier->list);
	kmem_cache_free(event_notifier_cache, event_notifier);
}

struct lttng_id_tracker *get_tracker(struct lttng_session *session,
		enum tracker_type tracker_type)
{
	switch (tracker_type) {
	case TRACKER_PID:
		return &session->pid_tracker;
	case TRACKER_VPID:
		return &session->vpid_tracker;
	case TRACKER_UID:
		return &session->uid_tracker;
	case TRACKER_VUID:
		return &session->vuid_tracker;
	case TRACKER_GID:
		return &session->gid_tracker;
	case TRACKER_VGID:
		return &session->vgid_tracker;
	default:
		WARN_ON_ONCE(1);
		return NULL;
	}
}

int lttng_session_track_id(struct lttng_session *session,
		enum tracker_type tracker_type, int id)
{
	struct lttng_id_tracker *tracker;
	int ret;

	tracker = get_tracker(session, tracker_type);
	if (!tracker)
		return -EINVAL;
	if (id < -1)
		return -EINVAL;
	mutex_lock(&sessions_mutex);
	if (id == -1) {
		/* track all ids: destroy tracker. */
		lttng_id_tracker_destroy(tracker, true);
		ret = 0;
	} else {
		ret = lttng_id_tracker_add(tracker, id);
	}
	mutex_unlock(&sessions_mutex);
	return ret;
}

int lttng_session_untrack_id(struct lttng_session *session,
		enum tracker_type tracker_type, int id)
{
	struct lttng_id_tracker *tracker;
	int ret;

	tracker = get_tracker(session, tracker_type);
	if (!tracker)
		return -EINVAL;
	if (id < -1)
		return -EINVAL;
	mutex_lock(&sessions_mutex);
	if (id == -1) {
		/* untrack all ids: replace by empty tracker. */
		ret = lttng_id_tracker_empty_set(tracker);
	} else {
		ret = lttng_id_tracker_del(tracker, id);
	}
	mutex_unlock(&sessions_mutex);
	return ret;
}

static
void *id_list_start(struct seq_file *m, loff_t *pos)
{
	struct lttng_id_tracker *id_tracker = m->private;
	struct lttng_id_tracker_rcu *id_tracker_p = id_tracker->p;
	struct lttng_id_hash_node *e;
	int iter = 0, i;

	mutex_lock(&sessions_mutex);
	if (id_tracker_p) {
		for (i = 0; i < LTTNG_ID_TABLE_SIZE; i++) {
			struct hlist_head *head = &id_tracker_p->id_hash[i];

			lttng_hlist_for_each_entry(e, head, hlist) {
				if (iter++ >= *pos)
					return e;
			}
		}
	} else {
		/* ID tracker disabled. */
		if (iter >= *pos && iter == 0) {
			return id_tracker_p;	/* empty tracker */
		}
		iter++;
	}
	/* End of list */
	return NULL;
}

/* Called with sessions_mutex held. */
static
void *id_list_next(struct seq_file *m, void *p, loff_t *ppos)
{
	struct lttng_id_tracker *id_tracker = m->private;
	struct lttng_id_tracker_rcu *id_tracker_p = id_tracker->p;
	struct lttng_id_hash_node *e;
	int iter = 0, i;

	(*ppos)++;
	if (id_tracker_p) {
		for (i = 0; i < LTTNG_ID_TABLE_SIZE; i++) {
			struct hlist_head *head = &id_tracker_p->id_hash[i];

			lttng_hlist_for_each_entry(e, head, hlist) {
				if (iter++ >= *ppos)
					return e;
			}
		}
	} else {
		/* ID tracker disabled. */
		if (iter >= *ppos && iter == 0)
			return p;	/* empty tracker */
		iter++;
	}

	/* End of list */
	return NULL;
}

static
void id_list_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&sessions_mutex);
}

static
int id_list_show(struct seq_file *m, void *p)
{
	struct lttng_id_tracker *id_tracker = m->private;
	struct lttng_id_tracker_rcu *id_tracker_p = id_tracker->p;
	int id;

	if (p == id_tracker_p) {
		/* Tracker disabled. */
		id = -1;
	} else {
		const struct lttng_id_hash_node *e = p;

		id = lttng_id_tracker_get_node_id(e);
	}
	switch (id_tracker->tracker_type) {
	case TRACKER_PID:
		seq_printf(m,	"process { pid = %d; };\n", id);
		break;
	case TRACKER_VPID:
		seq_printf(m,	"process { vpid = %d; };\n", id);
		break;
	case TRACKER_UID:
		seq_printf(m,	"user { uid = %d; };\n", id);
		break;
	case TRACKER_VUID:
		seq_printf(m,	"user { vuid = %d; };\n", id);
		break;
	case TRACKER_GID:
		seq_printf(m,	"group { gid = %d; };\n", id);
		break;
	case TRACKER_VGID:
		seq_printf(m,	"group { vgid = %d; };\n", id);
		break;
	default:
		seq_printf(m,	"UNKNOWN { field = %d };\n", id);
	}
	return 0;
}

static
const struct seq_operations lttng_tracker_ids_list_seq_ops = {
	.start = id_list_start,
	.next = id_list_next,
	.stop = id_list_stop,
	.show = id_list_show,
};

static
int lttng_tracker_ids_list_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &lttng_tracker_ids_list_seq_ops);
}

static
int lttng_tracker_ids_list_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct lttng_id_tracker *id_tracker = m->private;
	int ret;

	WARN_ON_ONCE(!id_tracker);
	ret = seq_release(inode, file);
	if (!ret)
		fput(id_tracker->session->file);
	return ret;
}

const struct file_operations lttng_tracker_ids_list_fops = {
	.owner = THIS_MODULE,
	.open = lttng_tracker_ids_list_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = lttng_tracker_ids_list_release,
};

int lttng_session_list_tracker_ids(struct lttng_session *session,
		enum tracker_type tracker_type)
{
	struct file *tracker_ids_list_file;
	struct seq_file *m;
	int file_fd, ret;

	file_fd = lttng_get_unused_fd();
	if (file_fd < 0) {
		ret = file_fd;
		goto fd_error;
	}

	tracker_ids_list_file = anon_inode_getfile("[lttng_tracker_ids_list]",
					  &lttng_tracker_ids_list_fops,
					  NULL, O_RDWR);
	if (IS_ERR(tracker_ids_list_file)) {
		ret = PTR_ERR(tracker_ids_list_file);
		goto file_error;
	}
	if (!atomic_long_add_unless(&session->file->f_count, 1, LONG_MAX)) {
		ret = -EOVERFLOW;
		goto refcount_error;
	}
	ret = lttng_tracker_ids_list_fops.open(NULL, tracker_ids_list_file);
	if (ret < 0)
		goto open_error;
	m = tracker_ids_list_file->private_data;

	m->private = get_tracker(session, tracker_type);
	BUG_ON(!m->private);
	fd_install(file_fd, tracker_ids_list_file);

	return file_fd;

open_error:
	atomic_long_dec(&session->file->f_count);
refcount_error:
	fput(tracker_ids_list_file);
file_error:
	put_unused_fd(file_fd);
fd_error:
	return ret;
}

/*
 * Enabler management.
 */
static
int lttng_match_enabler_star_glob(const char *desc_name,
		const char *pattern)
{
	if (!strutils_star_glob_match(pattern, LTTNG_SIZE_MAX,
			desc_name, LTTNG_SIZE_MAX))
		return 0;
	return 1;
}

static
int lttng_match_enabler_name(const char *desc_name,
		const char *name)
{
	if (strcmp(desc_name, name))
		return 0;
	return 1;
}

int lttng_desc_match_enabler(const struct lttng_event_desc *desc,
		struct lttng_enabler *enabler)
{
	const char *desc_name, *enabler_name;
	bool compat = false, entry = false;

	enabler_name = enabler->event_param.name;
	switch (enabler->event_param.instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		desc_name = desc->name;
		switch (enabler->format_type) {
		case LTTNG_ENABLER_FORMAT_STAR_GLOB:
			return lttng_match_enabler_star_glob(desc_name, enabler_name);
		case LTTNG_ENABLER_FORMAT_NAME:
			return lttng_match_enabler_name(desc_name, enabler_name);
		default:
			return -EINVAL;
		}
		break;
	case LTTNG_KERNEL_SYSCALL:
		desc_name = desc->name;
		if (!strncmp(desc_name, "compat_", strlen("compat_"))) {
			desc_name += strlen("compat_");
			compat = true;
		}
		if (!strncmp(desc_name, "syscall_exit_",
				strlen("syscall_exit_"))) {
			desc_name += strlen("syscall_exit_");
		} else if (!strncmp(desc_name, "syscall_entry_",
				strlen("syscall_entry_"))) {
			desc_name += strlen("syscall_entry_");
			entry = true;
		} else {
			WARN_ON_ONCE(1);
			return -EINVAL;
		}
		switch (enabler->event_param.u.syscall.entryexit) {
		case LTTNG_KERNEL_SYSCALL_ENTRYEXIT:
			break;
		case LTTNG_KERNEL_SYSCALL_ENTRY:
			if (!entry)
				return 0;
			break;
		case LTTNG_KERNEL_SYSCALL_EXIT:
			if (entry)
				return 0;
			break;
		default:
			return -EINVAL;
		}
		switch (enabler->event_param.u.syscall.abi) {
		case LTTNG_KERNEL_SYSCALL_ABI_ALL:
			break;
		case LTTNG_KERNEL_SYSCALL_ABI_NATIVE:
			if (compat)
				return 0;
			break;
		case LTTNG_KERNEL_SYSCALL_ABI_COMPAT:
			if (!compat)
				return 0;
			break;
		default:
			return -EINVAL;
		}
		switch (enabler->event_param.u.syscall.match) {
		case LTTNG_KERNEL_SYSCALL_MATCH_NAME:
			switch (enabler->format_type) {
			case LTTNG_ENABLER_FORMAT_STAR_GLOB:
				return lttng_match_enabler_star_glob(desc_name, enabler_name);
			case LTTNG_ENABLER_FORMAT_NAME:
				return lttng_match_enabler_name(desc_name, enabler_name);
			default:
				return -EINVAL;
			}
			break;
		case LTTNG_KERNEL_SYSCALL_MATCH_NR:
			return -EINVAL;	/* Not implemented. */
		default:
			return -EINVAL;
		}
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
}

static
int lttng_event_enabler_match_event(struct lttng_event_enabler *event_enabler,
		struct lttng_event *event)
{
	struct lttng_enabler *base_enabler = lttng_event_enabler_as_enabler(
		event_enabler);

	if (base_enabler->event_param.instrumentation != event->instrumentation)
		return 0;
	if (lttng_desc_match_enabler(event->desc, base_enabler)
			&& event->chan == event_enabler->chan)
		return 1;
	else
		return 0;
}

static
int lttng_event_notifier_enabler_match_event_notifier(struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_event_notifier *event_notifier)
{
	struct lttng_enabler *base_enabler = lttng_event_notifier_enabler_as_enabler(
		event_notifier_enabler);

	if (base_enabler->event_param.instrumentation != event_notifier->instrumentation)
		return 0;
	if (lttng_desc_match_enabler(event_notifier->desc, base_enabler)
			&& event_notifier->group == event_notifier_enabler->group
			&& event_notifier->user_token == event_notifier_enabler->base.user_token)
		return 1;
	else
		return 0;
}

static
struct lttng_enabler_ref *lttng_enabler_ref(
		struct list_head *enablers_ref_list,
		struct lttng_enabler *enabler)
{
	struct lttng_enabler_ref *enabler_ref;

	list_for_each_entry(enabler_ref, enablers_ref_list, node) {
		if (enabler_ref->ref == enabler)
			return enabler_ref;
	}
	return NULL;
}

static
void lttng_create_tracepoint_event_if_missing(struct lttng_event_enabler *event_enabler)
{
	struct lttng_session *session = event_enabler->chan->session;
	struct lttng_probe_desc *probe_desc;
	const struct lttng_event_desc *desc;
	int i;
	struct list_head *probe_list;

	probe_list = lttng_get_probe_list_head();
	/*
	 * For each probe event, if we find that a probe event matches
	 * our enabler, create an associated lttng_event if not
	 * already present.
	 */
	list_for_each_entry(probe_desc, probe_list, head) {
		for (i = 0; i < probe_desc->nr_events; i++) {
			int found = 0;
			struct hlist_head *head;
			struct lttng_event *event;

			desc = probe_desc->event_desc[i];
			if (!lttng_desc_match_enabler(desc,
					lttng_event_enabler_as_enabler(event_enabler)))
				continue;

			/*
			 * Check if already created.
			 */
			head = utils_borrow_hash_table_bucket(
				session->events_ht.table, LTTNG_EVENT_HT_SIZE,
				desc->name);
			lttng_hlist_for_each_entry(event, head, hlist) {
				if (event->desc == desc
						&& event->chan == event_enabler->chan)
					found = 1;
			}
			if (found)
				continue;

			/*
			 * We need to create an event for this
			 * event probe.
			 */
			event = _lttng_event_create(event_enabler->chan,
					NULL, NULL, desc,
					LTTNG_KERNEL_TRACEPOINT);
			if (!event) {
				printk(KERN_INFO "LTTng: Unable to create event %s\n",
					probe_desc->event_desc[i]->name);
			}
		}
	}
}

static
void lttng_create_tracepoint_event_notifier_if_missing(struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	struct lttng_event_notifier_group *event_notifier_group = event_notifier_enabler->group;
	struct lttng_probe_desc *probe_desc;
	const struct lttng_event_desc *desc;
	int i;
	struct list_head *probe_list;

	probe_list = lttng_get_probe_list_head();
	/*
	 * For each probe event, if we find that a probe event matches
	 * our enabler, create an associated lttng_event_notifier if not
	 * already present.
	 */
	list_for_each_entry(probe_desc, probe_list, head) {
		for (i = 0; i < probe_desc->nr_events; i++) {
			int found = 0;
			struct hlist_head *head;
			struct lttng_event_notifier *event_notifier;

			desc = probe_desc->event_desc[i];
			if (!lttng_desc_match_enabler(desc,
					lttng_event_notifier_enabler_as_enabler(event_notifier_enabler)))
				continue;

			/*
			 * Check if already created.
			 */
			head = utils_borrow_hash_table_bucket(
				event_notifier_group->event_notifiers_ht.table,
				LTTNG_EVENT_NOTIFIER_HT_SIZE, desc->name);
			lttng_hlist_for_each_entry(event_notifier, head, hlist) {
				if (event_notifier->desc == desc
						&& event_notifier->user_token == event_notifier_enabler->base.user_token)
					found = 1;
			}
			if (found)
				continue;

			/*
			 * We need to create a event_notifier for this event probe.
			 */
			event_notifier = _lttng_event_notifier_create(desc,
				event_notifier_enabler->base.user_token,
				event_notifier_group, NULL, NULL,
				LTTNG_KERNEL_TRACEPOINT);
			if (IS_ERR(event_notifier)) {
				printk(KERN_INFO "Unable to create event_notifier %s\n",
					probe_desc->event_desc[i]->name);
			}
		}
	}
}

static
void lttng_create_syscall_event_if_missing(struct lttng_event_enabler *event_enabler)
{
	int ret;

	ret = lttng_syscalls_register_event(event_enabler->chan, NULL);
	WARN_ON_ONCE(ret);
}

static
void lttng_create_syscall_event_notifier_if_missing(struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	int ret;

	ret = lttng_syscalls_register_event_notifier(event_notifier_enabler, NULL);
	WARN_ON_ONCE(ret);
	ret = lttng_syscals_create_matching_event_notifiers(event_notifier_enabler, NULL);
	WARN_ON_ONCE(ret);
}

/*
 * Create struct lttng_event if it is missing and present in the list of
 * tracepoint probes.
 * Should be called with sessions mutex held.
 */
static
void lttng_create_event_if_missing(struct lttng_event_enabler *event_enabler)
{
	switch (event_enabler->base.event_param.instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		lttng_create_tracepoint_event_if_missing(event_enabler);
		break;
	case LTTNG_KERNEL_SYSCALL:
		lttng_create_syscall_event_if_missing(event_enabler);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * Create events associated with an event_enabler (if not already present),
 * and add backward reference from the event to the enabler.
 * Should be called with sessions mutex held.
 */
static
int lttng_event_enabler_ref_events(struct lttng_event_enabler *event_enabler)
{
	struct lttng_channel *chan = event_enabler->chan;
	struct lttng_session *session = event_enabler->chan->session;
	struct lttng_enabler *base_enabler = lttng_event_enabler_as_enabler(event_enabler);
	struct lttng_event *event;

	if (base_enabler->event_param.instrumentation == LTTNG_KERNEL_SYSCALL &&
			base_enabler->event_param.u.syscall.entryexit == LTTNG_KERNEL_SYSCALL_ENTRYEXIT &&
			base_enabler->event_param.u.syscall.abi == LTTNG_KERNEL_SYSCALL_ABI_ALL &&
			base_enabler->event_param.u.syscall.match == LTTNG_KERNEL_SYSCALL_MATCH_NAME &&
			!strcmp(base_enabler->event_param.name, "*")) {
		if (base_enabler->enabled)
			WRITE_ONCE(chan->syscall_all, 1);
		else
			WRITE_ONCE(chan->syscall_all, 0);
	}

	/* First ensure that probe events are created for this enabler. */
	lttng_create_event_if_missing(event_enabler);

	/* For each event matching event_enabler in session event list. */
	list_for_each_entry(event, &session->events, list) {
		struct lttng_enabler_ref *enabler_ref;

		if (!lttng_event_enabler_match_event(event_enabler, event))
			continue;
		enabler_ref = lttng_enabler_ref(&event->enablers_ref_head,
			lttng_event_enabler_as_enabler(event_enabler));
		if (!enabler_ref) {
			/*
			 * If no backward ref, create it.
			 * Add backward ref from event to event_enabler.
			 */
			enabler_ref = kzalloc(sizeof(*enabler_ref), GFP_KERNEL);
			if (!enabler_ref)
				return -ENOMEM;
			enabler_ref->ref = lttng_event_enabler_as_enabler(event_enabler);
			list_add(&enabler_ref->node,
				&event->enablers_ref_head);
		}

		/*
		 * Link filter bytecodes if not linked yet.
		 */
		lttng_enabler_link_bytecode(event->desc,
			lttng_static_ctx,
			&event->filter_bytecode_runtime_head,
			lttng_event_enabler_as_enabler(event_enabler));

		/* TODO: merge event context. */
	}
	return 0;
}

/*
 * Create struct lttng_event_notifier if it is missing and present in the list of
 * tracepoint probes.
 * Should be called with sessions mutex held.
 */
static
void lttng_create_event_notifier_if_missing(struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	switch (event_notifier_enabler->base.event_param.instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
		lttng_create_tracepoint_event_notifier_if_missing(event_notifier_enabler);
		break;
	case LTTNG_KERNEL_SYSCALL:
		lttng_create_syscall_event_notifier_if_missing(event_notifier_enabler);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}
}

/*
 * Create event_notifiers associated with a event_notifier enabler (if not already present).
 */
static
int lttng_event_notifier_enabler_ref_event_notifiers(
		struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	struct lttng_event_notifier_group *event_notifier_group = event_notifier_enabler->group;
	struct lttng_enabler *base_enabler = lttng_event_notifier_enabler_as_enabler(event_notifier_enabler);
	struct lttng_event_notifier *event_notifier;

	if (base_enabler->event_param.instrumentation == LTTNG_KERNEL_SYSCALL &&
			base_enabler->event_param.u.syscall.abi == LTTNG_KERNEL_SYSCALL_ABI_ALL &&
			base_enabler->event_param.u.syscall.match == LTTNG_KERNEL_SYSCALL_MATCH_NAME &&
			!strcmp(base_enabler->event_param.name, "*")) {

		int enabled = base_enabler->enabled;
		enum lttng_kernel_syscall_entryexit entryexit = base_enabler->event_param.u.syscall.entryexit;

		if (entryexit == LTTNG_KERNEL_SYSCALL_ENTRY || entryexit == LTTNG_KERNEL_SYSCALL_ENTRYEXIT)
			WRITE_ONCE(event_notifier_group->syscall_all_entry, enabled);

		if (entryexit == LTTNG_KERNEL_SYSCALL_EXIT || entryexit == LTTNG_KERNEL_SYSCALL_ENTRYEXIT)
			WRITE_ONCE(event_notifier_group->syscall_all_exit, enabled);

	}

	/* First ensure that probe event_notifiers are created for this enabler. */
	lttng_create_event_notifier_if_missing(event_notifier_enabler);

	/* Link the created event_notifier with its associated enabler. */
	list_for_each_entry(event_notifier, &event_notifier_group->event_notifiers_head, list) {
		struct lttng_enabler_ref *enabler_ref;

		if (!lttng_event_notifier_enabler_match_event_notifier(event_notifier_enabler, event_notifier))
			continue;

		enabler_ref = lttng_enabler_ref(&event_notifier->enablers_ref_head,
			lttng_event_notifier_enabler_as_enabler(event_notifier_enabler));
		if (!enabler_ref) {
			/*
			 * If no backward ref, create it.
			 * Add backward ref from event_notifier to enabler.
			 */
			enabler_ref = kzalloc(sizeof(*enabler_ref), GFP_KERNEL);
			if (!enabler_ref)
				return -ENOMEM;

			enabler_ref->ref = lttng_event_notifier_enabler_as_enabler(
				event_notifier_enabler);
			list_add(&enabler_ref->node,
				&event_notifier->enablers_ref_head);
		}

		/*
		 * Link filter bytecodes if not linked yet.
		 */
		lttng_enabler_link_bytecode(event_notifier->desc,
			lttng_static_ctx, &event_notifier->filter_bytecode_runtime_head,
			lttng_event_notifier_enabler_as_enabler(event_notifier_enabler));
	}
	return 0;
}

/*
 * Called at module load: connect the probe on all enablers matching
 * this event.
 * Called with sessions lock held.
 */
int lttng_fix_pending_events(void)
{
	struct lttng_session *session;

	list_for_each_entry(session, &sessions, list)
		lttng_session_lazy_sync_event_enablers(session);
	return 0;
}

static bool lttng_event_notifier_group_has_active_event_notifiers(
		struct lttng_event_notifier_group *event_notifier_group)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler;

	list_for_each_entry(event_notifier_enabler, &event_notifier_group->enablers_head,
			node) {
		if (event_notifier_enabler->base.enabled)
			return true;
	}
	return false;
}

bool lttng_event_notifier_active(void)
{
	struct lttng_event_notifier_group *event_notifier_group;

	list_for_each_entry(event_notifier_group, &event_notifier_groups, node) {
		if (lttng_event_notifier_group_has_active_event_notifiers(event_notifier_group))
			return true;
	}
	return false;
}

int lttng_fix_pending_event_notifiers(void)
{
	struct lttng_event_notifier_group *event_notifier_group;

	list_for_each_entry(event_notifier_group, &event_notifier_groups, node)
		lttng_event_notifier_group_sync_enablers(event_notifier_group);
	return 0;
}

struct lttng_event_enabler *lttng_event_enabler_create(
		enum lttng_enabler_format_type format_type,
		struct lttng_kernel_event *event_param,
		struct lttng_channel *chan)
{
	struct lttng_event_enabler *event_enabler;

	event_enabler = kzalloc(sizeof(*event_enabler), GFP_KERNEL);
	if (!event_enabler)
		return NULL;
	event_enabler->base.format_type = format_type;
	INIT_LIST_HEAD(&event_enabler->base.filter_bytecode_head);
	memcpy(&event_enabler->base.event_param, event_param,
		sizeof(event_enabler->base.event_param));
	event_enabler->chan = chan;
	/* ctx left NULL */
	event_enabler->base.enabled = 0;
	event_enabler->base.evtype = LTTNG_TYPE_ENABLER;
	mutex_lock(&sessions_mutex);
	list_add(&event_enabler->node, &event_enabler->chan->session->enablers_head);
	lttng_session_lazy_sync_event_enablers(event_enabler->chan->session);
	mutex_unlock(&sessions_mutex);
	return event_enabler;
}

int lttng_event_enabler_enable(struct lttng_event_enabler *event_enabler)
{
	mutex_lock(&sessions_mutex);
	lttng_event_enabler_as_enabler(event_enabler)->enabled = 1;
	lttng_session_lazy_sync_event_enablers(event_enabler->chan->session);
	mutex_unlock(&sessions_mutex);
	return 0;
}

int lttng_event_enabler_disable(struct lttng_event_enabler *event_enabler)
{
	mutex_lock(&sessions_mutex);
	lttng_event_enabler_as_enabler(event_enabler)->enabled = 0;
	lttng_session_lazy_sync_event_enablers(event_enabler->chan->session);
	mutex_unlock(&sessions_mutex);
	return 0;
}

static
int lttng_enabler_attach_filter_bytecode(struct lttng_enabler *enabler,
		struct lttng_kernel_filter_bytecode __user *bytecode)
{
	struct lttng_filter_bytecode_node *bytecode_node;
	uint32_t bytecode_len;
	int ret;

	ret = get_user(bytecode_len, &bytecode->len);
	if (ret)
		return ret;
	bytecode_node = kzalloc(sizeof(*bytecode_node) + bytecode_len,
			GFP_KERNEL);
	if (!bytecode_node)
		return -ENOMEM;
	ret = copy_from_user(&bytecode_node->bc, bytecode,
		sizeof(*bytecode) + bytecode_len);
	if (ret)
		goto error_free;

	bytecode_node->enabler = enabler;
	/* Enforce length based on allocated size */
	bytecode_node->bc.len = bytecode_len;
	list_add_tail(&bytecode_node->node, &enabler->filter_bytecode_head);

	return 0;

error_free:
	kfree(bytecode_node);
	return ret;
}

int lttng_event_enabler_attach_filter_bytecode(struct lttng_event_enabler *event_enabler,
		struct lttng_kernel_filter_bytecode __user *bytecode)
{
	int ret;
	ret = lttng_enabler_attach_filter_bytecode(
		lttng_event_enabler_as_enabler(event_enabler), bytecode);
	if (ret)
		goto error;

	lttng_session_lazy_sync_event_enablers(event_enabler->chan->session);
	return 0;

error:
	return ret;
}

int lttng_event_add_callsite(struct lttng_event *event,
		struct lttng_kernel_event_callsite __user *callsite)
{

	switch (event->instrumentation) {
	case LTTNG_KERNEL_UPROBE:
		return lttng_uprobes_event_add_callsite(event, callsite);
	default:
		return -EINVAL;
	}
}

int lttng_event_enabler_attach_context(struct lttng_event_enabler *event_enabler,
		struct lttng_kernel_context *context_param)
{
	return -ENOSYS;
}

static
void lttng_enabler_destroy(struct lttng_enabler *enabler)
{
	struct lttng_filter_bytecode_node *filter_node, *tmp_filter_node;

	/* Destroy filter bytecode */
	list_for_each_entry_safe(filter_node, tmp_filter_node,
			&enabler->filter_bytecode_head, node) {
		kfree(filter_node);
	}
}

static
void lttng_event_enabler_destroy(struct lttng_event_enabler *event_enabler)
{
	lttng_enabler_destroy(lttng_event_enabler_as_enabler(event_enabler));

	/* Destroy contexts */
	lttng_destroy_context(event_enabler->ctx);

	list_del(&event_enabler->node);
	kfree(event_enabler);
}

struct lttng_event_notifier_enabler *lttng_event_notifier_enabler_create(
		struct lttng_event_notifier_group *event_notifier_group,
		enum lttng_enabler_format_type format_type,
		struct lttng_kernel_event_notifier *event_notifier_param)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler;

	event_notifier_enabler = kzalloc(sizeof(*event_notifier_enabler), GFP_KERNEL);
	if (!event_notifier_enabler)
		return NULL;

	event_notifier_enabler->base.format_type = format_type;
	INIT_LIST_HEAD(&event_notifier_enabler->base.filter_bytecode_head);

	memcpy(&event_notifier_enabler->base.event_param, &event_notifier_param->event,
		sizeof(event_notifier_enabler->base.event_param));
	event_notifier_enabler->base.evtype = LTTNG_TYPE_ENABLER;

	event_notifier_enabler->base.enabled = 0;
	event_notifier_enabler->base.user_token = event_notifier_param->event.token;
	event_notifier_enabler->group = event_notifier_group;

	mutex_lock(&sessions_mutex);
	list_add(&event_notifier_enabler->node, &event_notifier_enabler->group->enablers_head);
	lttng_event_notifier_group_sync_enablers(event_notifier_enabler->group);

	mutex_unlock(&sessions_mutex);

	return event_notifier_enabler;
}

int lttng_event_notifier_enabler_enable(
		struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	mutex_lock(&sessions_mutex);
	lttng_event_notifier_enabler_as_enabler(event_notifier_enabler)->enabled = 1;
	lttng_event_notifier_group_sync_enablers(event_notifier_enabler->group);
	mutex_unlock(&sessions_mutex);
	return 0;
}

int lttng_event_notifier_enabler_disable(
		struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	mutex_lock(&sessions_mutex);
	lttng_event_notifier_enabler_as_enabler(event_notifier_enabler)->enabled = 0;
	lttng_event_notifier_group_sync_enablers(event_notifier_enabler->group);
	mutex_unlock(&sessions_mutex);
	return 0;
}

int lttng_event_notifier_enabler_attach_filter_bytecode(
		struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_kernel_filter_bytecode __user *bytecode)
{
	int ret;

	ret = lttng_enabler_attach_filter_bytecode(
		lttng_event_notifier_enabler_as_enabler(event_notifier_enabler),
		bytecode);
	if (ret)
		goto error;

	lttng_event_notifier_group_sync_enablers(event_notifier_enabler->group);
	return 0;

error:
	return ret;
}

int lttng_event_notifier_add_callsite(struct lttng_event_notifier *event_notifier,
		struct lttng_kernel_event_callsite __user *callsite)
{

	switch (event_notifier->instrumentation) {
	case LTTNG_KERNEL_UPROBE:
		return lttng_uprobes_event_notifier_add_callsite(event_notifier,
				callsite);
	default:
		return -EINVAL;
	}
}

int lttng_event_notifier_enabler_attach_context(
		struct lttng_event_notifier_enabler *event_notifier_enabler,
		struct lttng_kernel_context *context_param)
{
	return -ENOSYS;
}

static
void lttng_event_notifier_enabler_destroy(
		struct lttng_event_notifier_enabler *event_notifier_enabler)
{
	if (!event_notifier_enabler) {
		return;
	}

	list_del(&event_notifier_enabler->node);

	lttng_enabler_destroy(lttng_event_notifier_enabler_as_enabler(event_notifier_enabler));
	kfree(event_notifier_enabler);
}

/*
 * lttng_session_sync_event_enablers should be called just before starting a
 * session.
 * Should be called with sessions mutex held.
 */
static
void lttng_session_sync_event_enablers(struct lttng_session *session)
{
	struct lttng_event_enabler *event_enabler;
	struct lttng_event *event;

	list_for_each_entry(event_enabler, &session->enablers_head, node)
		lttng_event_enabler_ref_events(event_enabler);
	/*
	 * For each event, if at least one of its enablers is enabled,
	 * and its channel and session transient states are enabled, we
	 * enable the event, else we disable it.
	 */
	list_for_each_entry(event, &session->events, list) {
		struct lttng_enabler_ref *enabler_ref;
		struct lttng_bytecode_runtime *runtime;
		int enabled = 0, has_enablers_without_bytecode = 0;

		switch (event->instrumentation) {
		case LTTNG_KERNEL_TRACEPOINT:
		case LTTNG_KERNEL_SYSCALL:
			/* Enable events */
			list_for_each_entry(enabler_ref,
					&event->enablers_ref_head, node) {
				if (enabler_ref->ref->enabled) {
					enabled = 1;
					break;
				}
			}
			break;
		default:
			/* Not handled with lazy sync. */
			continue;
		}
		/*
		 * Enabled state is based on union of enablers, with
		 * intesection of session and channel transient enable
		 * states.
		 */
		enabled = enabled && session->tstate && event->chan->tstate;

		WRITE_ONCE(event->enabled, enabled);
		/*
		 * Sync tracepoint registration with event enabled
		 * state.
		 */
		if (enabled) {
			register_event(event);
		} else {
			_lttng_event_unregister(event);
		}

		/* Check if has enablers without bytecode enabled */
		list_for_each_entry(enabler_ref,
				&event->enablers_ref_head, node) {
			if (enabler_ref->ref->enabled
					&& list_empty(&enabler_ref->ref->filter_bytecode_head)) {
				has_enablers_without_bytecode = 1;
				break;
			}
		}
		event->has_enablers_without_bytecode =
			has_enablers_without_bytecode;

		/* Enable filters */
		list_for_each_entry(runtime,
				&event->filter_bytecode_runtime_head, node)
			lttng_filter_sync_state(runtime);
	}
}

/*
 * Apply enablers to session events, adding events to session if need
 * be. It is required after each modification applied to an active
 * session, and right before session "start".
 * "lazy" sync means we only sync if required.
 * Should be called with sessions mutex held.
 */
static
void lttng_session_lazy_sync_event_enablers(struct lttng_session *session)
{
	/* We can skip if session is not active */
	if (!session->active)
		return;
	lttng_session_sync_event_enablers(session);
}

static
void lttng_event_notifier_group_sync_enablers(struct lttng_event_notifier_group *event_notifier_group)
{
	struct lttng_event_notifier_enabler *event_notifier_enabler;
	struct lttng_event_notifier *event_notifier;

	list_for_each_entry(event_notifier_enabler, &event_notifier_group->enablers_head, node)
		lttng_event_notifier_enabler_ref_event_notifiers(event_notifier_enabler);

	/*
	 * For each event_notifier, if at least one of its enablers is enabled,
	 * we enable the event_notifier, else we disable it.
	 */
	list_for_each_entry(event_notifier, &event_notifier_group->event_notifiers_head, list) {
		struct lttng_enabler_ref *enabler_ref;
		struct lttng_bytecode_runtime *runtime;
		int enabled = 0, has_enablers_without_bytecode = 0;

		switch (event_notifier->instrumentation) {
		case LTTNG_KERNEL_TRACEPOINT:
		case LTTNG_KERNEL_SYSCALL:
			/* Enable event_notifiers */
			list_for_each_entry(enabler_ref,
					&event_notifier->enablers_ref_head, node) {
				if (enabler_ref->ref->enabled) {
					enabled = 1;
					break;
				}
			}
			break;
		default:
			/* Not handled with sync. */
			continue;
		}

		WRITE_ONCE(event_notifier->enabled, enabled);
		/*
		 * Sync tracepoint registration with event_notifier enabled
		 * state.
		 */
		if (enabled) {
			if (!event_notifier->registered)
				register_event_notifier(event_notifier);
		} else {
			if (event_notifier->registered)
				_lttng_event_notifier_unregister(event_notifier);
		}

		/* Check if has enablers without bytecode enabled */
		list_for_each_entry(enabler_ref,
				&event_notifier->enablers_ref_head, node) {
			if (enabler_ref->ref->enabled
					&& list_empty(&enabler_ref->ref->filter_bytecode_head)) {
				has_enablers_without_bytecode = 1;
				break;
			}
		}
		event_notifier->has_enablers_without_bytecode =
			has_enablers_without_bytecode;

		/* Enable filters */
		list_for_each_entry(runtime,
				&event_notifier->filter_bytecode_runtime_head, node)
				lttng_filter_sync_state(runtime);
	}
}

/*
 * Serialize at most one packet worth of metadata into a metadata
 * channel.
 * We grab the metadata cache mutex to get exclusive access to our metadata
 * buffer and to the metadata cache. Exclusive access to the metadata buffer
 * allows us to do racy operations such as looking for remaining space left in
 * packet and write, since mutual exclusion protects us from concurrent writes.
 * Mutual exclusion on the metadata cache allow us to read the cache content
 * without racing against reallocation of the cache by updates.
 * Returns the number of bytes written in the channel, 0 if no data
 * was written and a negative value on error.
 */
int lttng_metadata_output_channel(struct lttng_metadata_stream *stream,
		struct channel *chan, bool *coherent)
{
	struct lib_ring_buffer_ctx ctx;
	int ret = 0;
	size_t len, reserve_len;

	/*
	 * Ensure we support mutiple get_next / put sequences followed by
	 * put_next. The metadata cache lock protects reading the metadata
	 * cache. It can indeed be read concurrently by "get_next_subbuf" and
	 * "flush" operations on the buffer invoked by different processes.
	 * Moreover, since the metadata cache memory can be reallocated, we
	 * need to have exclusive access against updates even though we only
	 * read it.
	 */
	mutex_lock(&stream->metadata_cache->lock);
	WARN_ON(stream->metadata_in < stream->metadata_out);
	if (stream->metadata_in != stream->metadata_out)
		goto end;

	/* Metadata regenerated, change the version. */
	if (stream->metadata_cache->version != stream->version)
		stream->version = stream->metadata_cache->version;

	len = stream->metadata_cache->metadata_written -
		stream->metadata_in;
	if (!len)
		goto end;
	reserve_len = min_t(size_t,
			stream->transport->ops.packet_avail_size(chan),
			len);
	lib_ring_buffer_ctx_init(&ctx, chan, NULL, reserve_len,
			sizeof(char), -1);
	/*
	 * If reservation failed, return an error to the caller.
	 */
	ret = stream->transport->ops.event_reserve(&ctx, 0);
	if (ret != 0) {
		printk(KERN_WARNING "LTTng: Metadata event reservation failed\n");
		stream->coherent = false;
		goto end;
	}
	stream->transport->ops.event_write(&ctx,
			stream->metadata_cache->data + stream->metadata_in,
			reserve_len);
	stream->transport->ops.event_commit(&ctx);
	stream->metadata_in += reserve_len;
	if (reserve_len < len)
		stream->coherent = false;
	else
		stream->coherent = true;
	ret = reserve_len;

end:
	if (coherent)
		*coherent = stream->coherent;
	mutex_unlock(&stream->metadata_cache->lock);
	return ret;
}

static
void lttng_metadata_begin(struct lttng_session *session)
{
	if (atomic_inc_return(&session->metadata_cache->producing) == 1)
		mutex_lock(&session->metadata_cache->lock);
}

static
void lttng_metadata_end(struct lttng_session *session)
{
	WARN_ON_ONCE(!atomic_read(&session->metadata_cache->producing));
	if (atomic_dec_return(&session->metadata_cache->producing) == 0) {
		struct lttng_metadata_stream *stream;

		list_for_each_entry(stream, &session->metadata_cache->metadata_stream, list)
			wake_up_interruptible(&stream->read_wait);
		mutex_unlock(&session->metadata_cache->lock);
	}
}

/*
 * Write the metadata to the metadata cache.
 * Must be called with sessions_mutex held.
 * The metadata cache lock protects us from concurrent read access from
 * thread outputting metadata content to ring buffer.
 * The content of the printf is printed as a single atomic metadata
 * transaction.
 */
int lttng_metadata_printf(struct lttng_session *session,
			  const char *fmt, ...)
{
	char *str;
	size_t len;
	va_list ap;

	WARN_ON_ONCE(!LTTNG_READ_ONCE(session->active));

	va_start(ap, fmt);
	str = kvasprintf(GFP_KERNEL, fmt, ap);
	va_end(ap);
	if (!str)
		return -ENOMEM;

	len = strlen(str);
	WARN_ON_ONCE(!atomic_read(&session->metadata_cache->producing));
	if (session->metadata_cache->metadata_written + len >
			session->metadata_cache->cache_alloc) {
		char *tmp_cache_realloc;
		unsigned int tmp_cache_alloc_size;

		tmp_cache_alloc_size = max_t(unsigned int,
				session->metadata_cache->cache_alloc + len,
				session->metadata_cache->cache_alloc << 1);
		tmp_cache_realloc = vzalloc(tmp_cache_alloc_size);
		if (!tmp_cache_realloc)
			goto err;
		if (session->metadata_cache->data) {
			memcpy(tmp_cache_realloc,
				session->metadata_cache->data,
				session->metadata_cache->cache_alloc);
			vfree(session->metadata_cache->data);
		}

		session->metadata_cache->cache_alloc = tmp_cache_alloc_size;
		session->metadata_cache->data = tmp_cache_realloc;
	}
	memcpy(session->metadata_cache->data +
			session->metadata_cache->metadata_written,
			str, len);
	session->metadata_cache->metadata_written += len;
	kfree(str);

	return 0;

err:
	kfree(str);
	return -ENOMEM;
}

static
int print_tabs(struct lttng_session *session, size_t nesting)
{
	size_t i;

	for (i = 0; i < nesting; i++) {
		int ret;

		ret = lttng_metadata_printf(session, "	");
		if (ret) {
			return ret;
		}
	}
	return 0;
}

static
int lttng_field_name_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	return lttng_metadata_printf(session, " _%s;\n", field->name);
}

static
int _lttng_integer_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	int ret;

	WARN_ON_ONCE(type->atype != atype_integer);
	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	ret = lttng_metadata_printf(session,
		"integer { size = %u; align = %u; signed = %u; encoding = %s; base = %u;%s }",
		type->u.integer.size,
		type->u.integer.alignment,
		type->u.integer.signedness,
		(type->u.integer.encoding == lttng_encode_none)
			? "none"
			: (type->u.integer.encoding == lttng_encode_UTF8)
				? "UTF8"
				: "ASCII",
		type->u.integer.base,
#if __BYTE_ORDER == __BIG_ENDIAN
		type->u.integer.reverse_byte_order ? " byte_order = le;" : ""
#else
		type->u.integer.reverse_byte_order ? " byte_order = be;" : ""
#endif
	);
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_struct_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	int ret;
	uint32_t i, nr_fields;
	unsigned int alignment;

	WARN_ON_ONCE(type->atype != atype_struct_nestable);

	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	ret = lttng_metadata_printf(session,
		"struct {\n");
	if (ret)
		return ret;
	nr_fields = type->u.struct_nestable.nr_fields;
	for (i = 0; i < nr_fields; i++) {
		const struct lttng_event_field *iter_field;

		iter_field = &type->u.struct_nestable.fields[i];
		ret = _lttng_field_statedump(session, iter_field, nesting + 1);
		if (ret)
			return ret;
	}
	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	alignment = type->u.struct_nestable.alignment;
	if (alignment) {
		ret = lttng_metadata_printf(session,
			"} align(%u)",
			alignment);
	} else {
		ret = lttng_metadata_printf(session,
			"}");
	}
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_struct_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;

	ret = _lttng_struct_type_statedump(session,
			&field->type, nesting);
	if (ret)
		return ret;
	return lttng_field_name_statedump(session, field, nesting);
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_variant_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	int ret;
	uint32_t i, nr_choices;

	WARN_ON_ONCE(type->atype != atype_variant_nestable);
	/*
	 * CTF 1.8 does not allow expressing nonzero variant alignment in a nestable way.
	 */
	if (type->u.variant_nestable.alignment != 0)
		return -EINVAL;
	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	ret = lttng_metadata_printf(session,
		"variant <_%s> {\n",
		type->u.variant_nestable.tag_name);
	if (ret)
		return ret;
	nr_choices = type->u.variant_nestable.nr_choices;
	for (i = 0; i < nr_choices; i++) {
		const struct lttng_event_field *iter_field;

		iter_field = &type->u.variant_nestable.choices[i];
		ret = _lttng_field_statedump(session, iter_field, nesting + 1);
		if (ret)
			return ret;
	}
	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	ret = lttng_metadata_printf(session,
		"}");
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_variant_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;

	ret = _lttng_variant_type_statedump(session,
			&field->type, nesting);
	if (ret)
		return ret;
	return lttng_field_name_statedump(session, field, nesting);
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_array_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;
	const struct lttng_type *elem_type;

	WARN_ON_ONCE(field->type.atype != atype_array_nestable);

	if (field->type.u.array_nestable.alignment) {
		ret = print_tabs(session, nesting);
		if (ret)
			return ret;
		ret = lttng_metadata_printf(session,
		"struct { } align(%u) _%s_padding;\n",
				field->type.u.array_nestable.alignment * CHAR_BIT,
				field->name);
		if (ret)
			return ret;
	}
	/*
	 * Nested compound types: Only array of structures and variants are
	 * currently supported.
	 */
	elem_type = field->type.u.array_nestable.elem_type;
	switch (elem_type->atype) {
	case atype_integer:
	case atype_struct_nestable:
	case atype_variant_nestable:
		ret = _lttng_type_statedump(session, elem_type, nesting);
		if (ret)
			return ret;
		break;

	default:
		return -EINVAL;
	}
	ret = lttng_metadata_printf(session,
		" _%s[%u];\n",
		field->name,
		field->type.u.array_nestable.length);
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_sequence_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;
	const char *length_name;
	const struct lttng_type *elem_type;

	WARN_ON_ONCE(field->type.atype != atype_sequence_nestable);

	length_name = field->type.u.sequence_nestable.length_name;

	if (field->type.u.sequence_nestable.alignment) {
		ret = print_tabs(session, nesting);
		if (ret)
			return ret;
		ret = lttng_metadata_printf(session,
		"struct { } align(%u) _%s_padding;\n",
				field->type.u.sequence_nestable.alignment * CHAR_BIT,
				field->name);
		if (ret)
			return ret;
	}

	/*
	 * Nested compound types: Only array of structures and variants are
	 * currently supported.
	 */
	elem_type = field->type.u.sequence_nestable.elem_type;
	switch (elem_type->atype) {
	case atype_integer:
	case atype_struct_nestable:
	case atype_variant_nestable:
		ret = _lttng_type_statedump(session, elem_type, nesting);
		if (ret)
			return ret;
		break;

	default:
		return -EINVAL;
	}
	ret = lttng_metadata_printf(session,
		" _%s[ _%s ];\n",
		field->name,
		field->type.u.sequence_nestable.length_name);
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_enum_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	const struct lttng_enum_desc *enum_desc;
	const struct lttng_type *container_type;
	int ret;
	unsigned int i, nr_entries;

	container_type = type->u.enum_nestable.container_type;
	if (container_type->atype != atype_integer) {
		ret = -EINVAL;
		goto end;
	}
	enum_desc = type->u.enum_nestable.desc;
	nr_entries = enum_desc->nr_entries;

	ret = print_tabs(session, nesting);
	if (ret)
		goto end;
	ret = lttng_metadata_printf(session, "enum : ");
	if (ret)
		goto end;
	ret = _lttng_integer_type_statedump(session, container_type, 0);
	if (ret)
		goto end;
	ret = lttng_metadata_printf(session, " {\n");
	if (ret)
		goto end;
	/* Dump all entries */
	for (i = 0; i < nr_entries; i++) {
		const struct lttng_enum_entry *entry = &enum_desc->entries[i];
		int j, len;

		ret = print_tabs(session, nesting + 1);
		if (ret)
			goto end;
		ret = lttng_metadata_printf(session,
				"\"");
		if (ret)
			goto end;
		len = strlen(entry->string);
		/* Escape the character '"' */
		for (j = 0; j < len; j++) {
			char c = entry->string[j];

			switch (c) {
			case '"':
				ret = lttng_metadata_printf(session,
						"\\\"");
				break;
			case '\\':
				ret = lttng_metadata_printf(session,
						"\\\\");
				break;
			default:
				ret = lttng_metadata_printf(session,
						"%c", c);
				break;
			}
			if (ret)
				goto end;
		}
		ret = lttng_metadata_printf(session, "\"");
		if (ret)
			goto end;

		if (entry->options.is_auto) {
			ret = lttng_metadata_printf(session, ",\n");
			if (ret)
				goto end;
		} else {
			ret = lttng_metadata_printf(session,
					" = ");
			if (ret)
				goto end;
			if (entry->start.signedness)
				ret = lttng_metadata_printf(session,
					"%lld", (long long) entry->start.value);
			else
				ret = lttng_metadata_printf(session,
					"%llu", entry->start.value);
			if (ret)
				goto end;
			if (entry->start.signedness == entry->end.signedness &&
					entry->start.value
						== entry->end.value) {
				ret = lttng_metadata_printf(session,
					",\n");
			} else {
				if (entry->end.signedness) {
					ret = lttng_metadata_printf(session,
						" ... %lld,\n",
						(long long) entry->end.value);
				} else {
					ret = lttng_metadata_printf(session,
						" ... %llu,\n",
						entry->end.value);
				}
			}
			if (ret)
				goto end;
		}
	}
	ret = print_tabs(session, nesting);
	if (ret)
		goto end;
	ret = lttng_metadata_printf(session, "}");
end:
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_enum_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;

	ret = _lttng_enum_type_statedump(session, &field->type, nesting);
	if (ret)
		return ret;
	return lttng_field_name_statedump(session, field, nesting);
}

static
int _lttng_integer_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;

	ret = _lttng_integer_type_statedump(session, &field->type, nesting);
	if (ret)
		return ret;
	return lttng_field_name_statedump(session, field, nesting);
}

static
int _lttng_string_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	int ret;

	WARN_ON_ONCE(type->atype != atype_string);
	/* Default encoding is UTF8 */
	ret = print_tabs(session, nesting);
	if (ret)
		return ret;
	ret = lttng_metadata_printf(session,
		"string%s",
		type->u.string.encoding == lttng_encode_ASCII ?
			" { encoding = ASCII; }" : "");
	return ret;
}

static
int _lttng_string_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret;

	WARN_ON_ONCE(field->type.atype != atype_string);
	ret = _lttng_string_type_statedump(session, &field->type, nesting);
	if (ret)
		return ret;
	return lttng_field_name_statedump(session, field, nesting);
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_type_statedump(struct lttng_session *session,
		const struct lttng_type *type,
		size_t nesting)
{
	int ret = 0;

	switch (type->atype) {
	case atype_integer:
		ret = _lttng_integer_type_statedump(session, type, nesting);
		break;
	case atype_enum_nestable:
		ret = _lttng_enum_type_statedump(session, type, nesting);
		break;
	case atype_string:
		ret = _lttng_string_type_statedump(session, type, nesting);
		break;
	case atype_struct_nestable:
		ret = _lttng_struct_type_statedump(session, type, nesting);
		break;
	case atype_variant_nestable:
		ret = _lttng_variant_type_statedump(session, type, nesting);
		break;

	/* Nested arrays and sequences are not supported yet. */
	case atype_array_nestable:
	case atype_sequence_nestable:
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_field_statedump(struct lttng_session *session,
		const struct lttng_event_field *field,
		size_t nesting)
{
	int ret = 0;

	switch (field->type.atype) {
	case atype_integer:
		ret = _lttng_integer_field_statedump(session, field, nesting);
		break;
	case atype_enum_nestable:
		ret = _lttng_enum_field_statedump(session, field, nesting);
		break;
	case atype_string:
		ret = _lttng_string_field_statedump(session, field, nesting);
		break;
	case atype_struct_nestable:
		ret = _lttng_struct_field_statedump(session, field, nesting);
		break;
	case atype_array_nestable:
		ret = _lttng_array_field_statedump(session, field, nesting);
		break;
	case atype_sequence_nestable:
		ret = _lttng_sequence_field_statedump(session, field, nesting);
		break;
	case atype_variant_nestable:
		ret = _lttng_variant_field_statedump(session, field, nesting);
		break;

	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	return ret;
}

static
int _lttng_context_metadata_statedump(struct lttng_session *session,
				    struct lttng_ctx *ctx)
{
	int ret = 0;
	int i;

	if (!ctx)
		return 0;
	for (i = 0; i < ctx->nr_fields; i++) {
		const struct lttng_ctx_field *field = &ctx->fields[i];

		ret = _lttng_field_statedump(session, &field->event_field, 2);
		if (ret)
			return ret;
	}
	return ret;
}

static
int _lttng_fields_metadata_statedump(struct lttng_session *session,
				   struct lttng_event *event)
{
	const struct lttng_event_desc *desc = event->desc;
	int ret = 0;
	int i;

	for (i = 0; i < desc->nr_fields; i++) {
		const struct lttng_event_field *field = &desc->fields[i];

		ret = _lttng_field_statedump(session, field, 2);
		if (ret)
			return ret;
	}
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 * The entire event metadata is printed as a single atomic metadata
 * transaction.
 */
static
int _lttng_event_metadata_statedump(struct lttng_session *session,
				  struct lttng_channel *chan,
				  struct lttng_event *event)
{
	int ret = 0;

	if (event->metadata_dumped || !LTTNG_READ_ONCE(session->active))
		return 0;
	if (chan->channel_type == METADATA_CHANNEL)
		return 0;

	lttng_metadata_begin(session);

	ret = lttng_metadata_printf(session,
		"event {\n"
		"	name = \"%s\";\n"
		"	id = %u;\n"
		"	stream_id = %u;\n",
		event->desc->name,
		event->id,
		event->chan->id);
	if (ret)
		goto end;

	if (event->ctx) {
		ret = lttng_metadata_printf(session,
			"	context := struct {\n");
		if (ret)
			goto end;
	}
	ret = _lttng_context_metadata_statedump(session, event->ctx);
	if (ret)
		goto end;
	if (event->ctx) {
		ret = lttng_metadata_printf(session,
			"	};\n");
		if (ret)
			goto end;
	}

	ret = lttng_metadata_printf(session,
		"	fields := struct {\n"
		);
	if (ret)
		goto end;

	ret = _lttng_fields_metadata_statedump(session, event);
	if (ret)
		goto end;

	/*
	 * LTTng space reservation can only reserve multiples of the
	 * byte size.
	 */
	ret = lttng_metadata_printf(session,
		"	};\n"
		"};\n\n");
	if (ret)
		goto end;

	event->metadata_dumped = 1;
end:
	lttng_metadata_end(session);
	return ret;

}

/*
 * Must be called with sessions_mutex held.
 * The entire channel metadata is printed as a single atomic metadata
 * transaction.
 */
static
int _lttng_channel_metadata_statedump(struct lttng_session *session,
				    struct lttng_channel *chan)
{
	int ret = 0;

	if (chan->metadata_dumped || !LTTNG_READ_ONCE(session->active))
		return 0;

	if (chan->channel_type == METADATA_CHANNEL)
		return 0;

	lttng_metadata_begin(session);

	WARN_ON_ONCE(!chan->header_type);
	ret = lttng_metadata_printf(session,
		"stream {\n"
		"	id = %u;\n"
		"	event.header := %s;\n"
		"	packet.context := struct packet_context;\n",
		chan->id,
		chan->header_type == 1 ? "struct event_header_compact" :
			"struct event_header_large");
	if (ret)
		goto end;

	if (chan->ctx) {
		ret = lttng_metadata_printf(session,
			"	event.context := struct {\n");
		if (ret)
			goto end;
	}
	ret = _lttng_context_metadata_statedump(session, chan->ctx);
	if (ret)
		goto end;
	if (chan->ctx) {
		ret = lttng_metadata_printf(session,
			"	};\n");
		if (ret)
			goto end;
	}

	ret = lttng_metadata_printf(session,
		"};\n\n");

	chan->metadata_dumped = 1;
end:
	lttng_metadata_end(session);
	return ret;
}

/*
 * Must be called with sessions_mutex held.
 */
static
int _lttng_stream_packet_context_declare(struct lttng_session *session)
{
	return lttng_metadata_printf(session,
		"struct packet_context {\n"
		"	uint64_clock_monotonic_t timestamp_begin;\n"
		"	uint64_clock_monotonic_t timestamp_end;\n"
		"	uint64_t content_size;\n"
		"	uint64_t packet_size;\n"
		"	uint64_t packet_seq_num;\n"
		"	unsigned long events_discarded;\n"
		"	uint32_t cpu_id;\n"
		"};\n\n"
		);
}

/*
 * Compact header:
 * id: range: 0 - 30.
 * id 31 is reserved to indicate an extended header.
 *
 * Large header:
 * id: range: 0 - 65534.
 * id 65535 is reserved to indicate an extended header.
 *
 * Must be called with sessions_mutex held.
 */
static
int _lttng_event_header_declare(struct lttng_session *session)
{
	return lttng_metadata_printf(session,
	"struct event_header_compact {\n"
	"	enum : uint5_t { compact = 0 ... 30, extended = 31 } id;\n"
	"	variant <id> {\n"
	"		struct {\n"
	"			uint27_clock_monotonic_t timestamp;\n"
	"		} compact;\n"
	"		struct {\n"
	"			uint32_t id;\n"
	"			uint64_clock_monotonic_t timestamp;\n"
	"		} extended;\n"
	"	} v;\n"
	"} align(%u);\n"
	"\n"
	"struct event_header_large {\n"
	"	enum : uint16_t { compact = 0 ... 65534, extended = 65535 } id;\n"
	"	variant <id> {\n"
	"		struct {\n"
	"			uint32_clock_monotonic_t timestamp;\n"
	"		} compact;\n"
	"		struct {\n"
	"			uint32_t id;\n"
	"			uint64_clock_monotonic_t timestamp;\n"
	"		} extended;\n"
	"	} v;\n"
	"} align(%u);\n\n",
	lttng_alignof(uint32_t) * CHAR_BIT,
	lttng_alignof(uint16_t) * CHAR_BIT
	);
}

 /*
 * Approximation of NTP time of day to clock monotonic correlation,
 * taken at start of trace.
 * Yes, this is only an approximation. Yes, we can (and will) do better
 * in future versions.
 * This function may return a negative offset. It may happen if the
 * system sets the REALTIME clock to 0 after boot.
 *
 * Use 64bit timespec on kernels that have it, this makes 32bit arch
 * y2038 compliant.
 */
static
int64_t measure_clock_offset(void)
{
	uint64_t monotonic_avg, monotonic[2], realtime;
	uint64_t tcf = trace_clock_freq();
	int64_t offset;
	unsigned long flags;
#ifdef LTTNG_KERNEL_HAS_TIMESPEC64
	struct timespec64 rts = { 0, 0 };
#else
	struct timespec rts = { 0, 0 };
#endif

	/* Disable interrupts to increase correlation precision. */
	local_irq_save(flags);
	monotonic[0] = trace_clock_read64();
#ifdef LTTNG_KERNEL_HAS_TIMESPEC64
	ktime_get_real_ts64(&rts);
#else
	getnstimeofday(&rts);
#endif
	monotonic[1] = trace_clock_read64();
	local_irq_restore(flags);

	monotonic_avg = (monotonic[0] + monotonic[1]) >> 1;
	realtime = (uint64_t) rts.tv_sec * tcf;
	if (tcf == NSEC_PER_SEC) {
		realtime += rts.tv_nsec;
	} else {
		uint64_t n = rts.tv_nsec * tcf;

		do_div(n, NSEC_PER_SEC);
		realtime += n;
	}
	offset = (int64_t) realtime - monotonic_avg;
	return offset;
}

static
int print_escaped_ctf_string(struct lttng_session *session, const char *string)
{
	int ret = 0;
	size_t i;
	char cur;

	i = 0;
	cur = string[i];
	while (cur != '\0') {
		switch (cur) {
		case '\n':
			ret = lttng_metadata_printf(session, "%s", "\\n");
			break;
		case '\\':
		case '"':
			ret = lttng_metadata_printf(session, "%c", '\\');
			if (ret)
				goto error;
			/* We still print the current char */
			/* Fallthrough */
		default:
			ret = lttng_metadata_printf(session, "%c", cur);
			break;
		}

		if (ret)
			goto error;

		cur = string[++i];
	}
error:
	return ret;
}

static
int print_metadata_escaped_field(struct lttng_session *session, const char *field,
		const char *field_value)
{
	int ret;

	ret = lttng_metadata_printf(session, "	%s = \"", field);
	if (ret)
		goto error;

	ret = print_escaped_ctf_string(session, field_value);
	if (ret)
		goto error;

	ret = lttng_metadata_printf(session, "\";\n");

error:
	return ret;
}

/*
 * Output metadata into this session's metadata buffers.
 * Must be called with sessions_mutex held.
 */
static
int _lttng_session_metadata_statedump(struct lttng_session *session)
{
	unsigned char *uuid_c = session->uuid.b;
	unsigned char uuid_s[37], clock_uuid_s[BOOT_ID_LEN];
	const char *product_uuid;
	struct lttng_channel *chan;
	struct lttng_event *event;
	int ret = 0;

	if (!LTTNG_READ_ONCE(session->active))
		return 0;

	lttng_metadata_begin(session);

	if (session->metadata_dumped)
		goto skip_session;

	snprintf(uuid_s, sizeof(uuid_s),
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		uuid_c[0], uuid_c[1], uuid_c[2], uuid_c[3],
		uuid_c[4], uuid_c[5], uuid_c[6], uuid_c[7],
		uuid_c[8], uuid_c[9], uuid_c[10], uuid_c[11],
		uuid_c[12], uuid_c[13], uuid_c[14], uuid_c[15]);

	ret = lttng_metadata_printf(session,
		"typealias integer { size = 8; align = %u; signed = false; } := uint8_t;\n"
		"typealias integer { size = 16; align = %u; signed = false; } := uint16_t;\n"
		"typealias integer { size = 32; align = %u; signed = false; } := uint32_t;\n"
		"typealias integer { size = 64; align = %u; signed = false; } := uint64_t;\n"
		"typealias integer { size = %u; align = %u; signed = false; } := unsigned long;\n"
		"typealias integer { size = 5; align = 1; signed = false; } := uint5_t;\n"
		"typealias integer { size = 27; align = 1; signed = false; } := uint27_t;\n"
		"\n"
		"trace {\n"
		"	major = %u;\n"
		"	minor = %u;\n"
		"	uuid = \"%s\";\n"
		"	byte_order = %s;\n"
		"	packet.header := struct {\n"
		"		uint32_t magic;\n"
		"		uint8_t  uuid[16];\n"
		"		uint32_t stream_id;\n"
		"		uint64_t stream_instance_id;\n"
		"	};\n"
		"};\n\n",
		lttng_alignof(uint8_t) * CHAR_BIT,
		lttng_alignof(uint16_t) * CHAR_BIT,
		lttng_alignof(uint32_t) * CHAR_BIT,
		lttng_alignof(uint64_t) * CHAR_BIT,
		sizeof(unsigned long) * CHAR_BIT,
		lttng_alignof(unsigned long) * CHAR_BIT,
		CTF_SPEC_MAJOR,
		CTF_SPEC_MINOR,
		uuid_s,
#if __BYTE_ORDER == __BIG_ENDIAN
		"be"
#else
		"le"
#endif
		);
	if (ret)
		goto end;

	ret = lttng_metadata_printf(session,
		"env {\n"
		"	hostname = \"%s\";\n"
		"	domain = \"kernel\";\n"
		"	sysname = \"%s\";\n"
		"	kernel_release = \"%s\";\n"
		"	kernel_version = \"%s\";\n"
		"	tracer_name = \"lttng-modules\";\n"
		"	tracer_major = %d;\n"
		"	tracer_minor = %d;\n"
		"	tracer_patchlevel = %d;\n"
		"	trace_buffering_scheme = \"global\";\n",
		current->nsproxy->uts_ns->name.nodename,
		utsname()->sysname,
		utsname()->release,
		utsname()->version,
		LTTNG_MODULES_MAJOR_VERSION,
		LTTNG_MODULES_MINOR_VERSION,
		LTTNG_MODULES_PATCHLEVEL_VERSION
		);
	if (ret)
		goto end;

	ret = print_metadata_escaped_field(session, "trace_name", session->name);
	if (ret)
		goto end;
	ret = print_metadata_escaped_field(session, "trace_creation_datetime",
			session->creation_time);
	if (ret)
		goto end;

	/* Add the product UUID to the 'env' section */
	product_uuid = dmi_get_system_info(DMI_PRODUCT_UUID);
	if (product_uuid) {
		ret = lttng_metadata_printf(session,
				"	product_uuid = \"%s\";\n",
				product_uuid
				);
		if (ret)
			goto end;
	}

	/* Close the 'env' section */
	ret = lttng_metadata_printf(session, "};\n\n");
	if (ret)
		goto end;

	ret = lttng_metadata_printf(session,
		"clock {\n"
		"	name = \"%s\";\n",
		trace_clock_name()
		);
	if (ret)
		goto end;

	if (!trace_clock_uuid(clock_uuid_s)) {
		ret = lttng_metadata_printf(session,
			"	uuid = \"%s\";\n",
			clock_uuid_s
			);
		if (ret)
			goto end;
	}

	ret = lttng_metadata_printf(session,
		"	description = \"%s\";\n"
		"	freq = %llu; /* Frequency, in Hz */\n"
		"	/* clock value offset from Epoch is: offset * (1/freq) */\n"
		"	offset = %lld;\n"
		"};\n\n",
		trace_clock_description(),
		(unsigned long long) trace_clock_freq(),
		(long long) measure_clock_offset()
		);
	if (ret)
		goto end;

	ret = lttng_metadata_printf(session,
		"typealias integer {\n"
		"	size = 27; align = 1; signed = false;\n"
		"	map = clock.%s.value;\n"
		"} := uint27_clock_monotonic_t;\n"
		"\n"
		"typealias integer {\n"
		"	size = 32; align = %u; signed = false;\n"
		"	map = clock.%s.value;\n"
		"} := uint32_clock_monotonic_t;\n"
		"\n"
		"typealias integer {\n"
		"	size = 64; align = %u; signed = false;\n"
		"	map = clock.%s.value;\n"
		"} := uint64_clock_monotonic_t;\n\n",
		trace_clock_name(),
		lttng_alignof(uint32_t) * CHAR_BIT,
		trace_clock_name(),
		lttng_alignof(uint64_t) * CHAR_BIT,
		trace_clock_name()
		);
	if (ret)
		goto end;

	ret = _lttng_stream_packet_context_declare(session);
	if (ret)
		goto end;

	ret = _lttng_event_header_declare(session);
	if (ret)
		goto end;

skip_session:
	list_for_each_entry(chan, &session->chan, list) {
		ret = _lttng_channel_metadata_statedump(session, chan);
		if (ret)
			goto end;
	}

	list_for_each_entry(event, &session->events, list) {
		ret = _lttng_event_metadata_statedump(session, event->chan, event);
		if (ret)
			goto end;
	}
	session->metadata_dumped = 1;
end:
	lttng_metadata_end(session);
	return ret;
}

/**
 * lttng_transport_register - LTT transport registration
 * @transport: transport structure
 *
 * Registers a transport which can be used as output to extract the data out of
 * LTTng. The module calling this registration function must ensure that no
 * trap-inducing code will be executed by the transport functions. E.g.
 * vmalloc_sync_mappings() must be called between a vmalloc and the moment the memory
 * is made visible to the transport function. This registration acts as a
 * vmalloc_sync_mappings. Therefore, only if the module allocates virtual memory
 * after its registration must it synchronize the TLBs.
 */
void lttng_transport_register(struct lttng_transport *transport)
{
	/*
	 * Make sure no page fault can be triggered by the module about to be
	 * registered. We deal with this here so we don't have to call
	 * vmalloc_sync_mappings() in each module's init.
	 */
	wrapper_vmalloc_sync_mappings();

	mutex_lock(&sessions_mutex);
	list_add_tail(&transport->node, &lttng_transport_list);
	mutex_unlock(&sessions_mutex);
}
EXPORT_SYMBOL_GPL(lttng_transport_register);

/**
 * lttng_transport_unregister - LTT transport unregistration
 * @transport: transport structure
 */
void lttng_transport_unregister(struct lttng_transport *transport)
{
	mutex_lock(&sessions_mutex);
	list_del(&transport->node);
	mutex_unlock(&sessions_mutex);
}
EXPORT_SYMBOL_GPL(lttng_transport_unregister);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0))

enum cpuhp_state lttng_hp_prepare;
enum cpuhp_state lttng_hp_online;

static int lttng_hotplug_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct lttng_cpuhp_node *lttng_node;

	lttng_node = container_of(node, struct lttng_cpuhp_node, node);
	switch (lttng_node->component) {
	case LTTNG_RING_BUFFER_FRONTEND:
		return 0;
	case LTTNG_RING_BUFFER_BACKEND:
		return lttng_cpuhp_rb_backend_prepare(cpu, lttng_node);
	case LTTNG_RING_BUFFER_ITER:
		return 0;
	case LTTNG_CONTEXT_PERF_COUNTERS:
		return 0;
	default:
		return -EINVAL;
	}
}

static int lttng_hotplug_dead(unsigned int cpu, struct hlist_node *node)
{
	struct lttng_cpuhp_node *lttng_node;

	lttng_node = container_of(node, struct lttng_cpuhp_node, node);
	switch (lttng_node->component) {
	case LTTNG_RING_BUFFER_FRONTEND:
		return lttng_cpuhp_rb_frontend_dead(cpu, lttng_node);
	case LTTNG_RING_BUFFER_BACKEND:
		return 0;
	case LTTNG_RING_BUFFER_ITER:
		return 0;
	case LTTNG_CONTEXT_PERF_COUNTERS:
		return lttng_cpuhp_perf_counter_dead(cpu, lttng_node);
	default:
		return -EINVAL;
	}
}

static int lttng_hotplug_online(unsigned int cpu, struct hlist_node *node)
{
	struct lttng_cpuhp_node *lttng_node;

	lttng_node = container_of(node, struct lttng_cpuhp_node, node);
	switch (lttng_node->component) {
	case LTTNG_RING_BUFFER_FRONTEND:
		return lttng_cpuhp_rb_frontend_online(cpu, lttng_node);
	case LTTNG_RING_BUFFER_BACKEND:
		return 0;
	case LTTNG_RING_BUFFER_ITER:
		return lttng_cpuhp_rb_iter_online(cpu, lttng_node);
	case LTTNG_CONTEXT_PERF_COUNTERS:
		return lttng_cpuhp_perf_counter_online(cpu, lttng_node);
	default:
		return -EINVAL;
	}
}

static int lttng_hotplug_offline(unsigned int cpu, struct hlist_node *node)
{
	struct lttng_cpuhp_node *lttng_node;

	lttng_node = container_of(node, struct lttng_cpuhp_node, node);
	switch (lttng_node->component) {
	case LTTNG_RING_BUFFER_FRONTEND:
		return lttng_cpuhp_rb_frontend_offline(cpu, lttng_node);
	case LTTNG_RING_BUFFER_BACKEND:
		return 0;
	case LTTNG_RING_BUFFER_ITER:
		return 0;
	case LTTNG_CONTEXT_PERF_COUNTERS:
		return 0;
	default:
		return -EINVAL;
	}
}

static int __init lttng_init_cpu_hotplug(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_BP_PREPARE_DYN, "lttng:prepare",
			lttng_hotplug_prepare,
			lttng_hotplug_dead);
	if (ret < 0) {
		return ret;
	}
	lttng_hp_prepare = ret;
	lttng_rb_set_hp_prepare(ret);

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "lttng:online",
			lttng_hotplug_online,
			lttng_hotplug_offline);
	if (ret < 0) {
		cpuhp_remove_multi_state(lttng_hp_prepare);
		lttng_hp_prepare = 0;
		return ret;
	}
	lttng_hp_online = ret;
	lttng_rb_set_hp_online(ret);

	return 0;
}

static void __exit lttng_exit_cpu_hotplug(void)
{
	lttng_rb_set_hp_online(0);
	cpuhp_remove_multi_state(lttng_hp_online);
	lttng_rb_set_hp_prepare(0);
	cpuhp_remove_multi_state(lttng_hp_prepare);
}

#else /* #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)) */
static int lttng_init_cpu_hotplug(void)
{
	return 0;
}
static void lttng_exit_cpu_hotplug(void)
{
}
#endif /* #else #if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)) */


static int __init lttng_events_init(void)
{
	int ret;

	ret = wrapper_lttng_fixup_sig(THIS_MODULE);
	if (ret)
		return ret;
	ret = wrapper_get_pfnblock_flags_mask_init();
	if (ret)
		return ret;
	ret = wrapper_get_pageblock_flags_mask_init();
	if (ret)
		return ret;
	ret = lttng_probes_init();
	if (ret)
		return ret;
	ret = lttng_context_init();
	if (ret)
		return ret;
	ret = lttng_tracepoint_init();
	if (ret)
		goto error_tp;
	event_cache = KMEM_CACHE(lttng_event, 0);
	if (!event_cache) {
		ret = -ENOMEM;
		goto error_kmem_event;
	}
	event_notifier_cache = KMEM_CACHE(lttng_event_notifier, 0);
	if (!event_notifier_cache) {
		ret = -ENOMEM;
		goto error_kmem_event_notifier;
	}
	ret = lttng_abi_init();
	if (ret)
		goto error_abi;
	ret = lttng_logger_init();
	if (ret)
		goto error_logger;
	ret = lttng_init_cpu_hotplug();
	if (ret)
		goto error_hotplug;
	printk(KERN_NOTICE "LTTng: Loaded modules v%s.%s.%s%s (%s)%s%s\n",
		__stringify(LTTNG_MODULES_MAJOR_VERSION),
		__stringify(LTTNG_MODULES_MINOR_VERSION),
		__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION),
		LTTNG_MODULES_EXTRAVERSION,
		LTTNG_VERSION_NAME,
#ifdef LTTNG_EXTRA_VERSION_GIT
		LTTNG_EXTRA_VERSION_GIT[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_GIT,
#else
		"",
#endif
#ifdef LTTNG_EXTRA_VERSION_NAME
		LTTNG_EXTRA_VERSION_NAME[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_NAME);
#else
		"");
#endif
	return 0;

error_hotplug:
	lttng_logger_exit();
error_logger:
	lttng_abi_exit();
error_abi:
	kmem_cache_destroy(event_notifier_cache);
error_kmem_event_notifier:
	kmem_cache_destroy(event_cache);
error_kmem_event:
	lttng_tracepoint_exit();
error_tp:
	lttng_context_exit();
	printk(KERN_NOTICE "LTTng: Failed to load modules v%s.%s.%s%s (%s)%s%s\n",
		__stringify(LTTNG_MODULES_MAJOR_VERSION),
		__stringify(LTTNG_MODULES_MINOR_VERSION),
		__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION),
		LTTNG_MODULES_EXTRAVERSION,
		LTTNG_VERSION_NAME,
#ifdef LTTNG_EXTRA_VERSION_GIT
		LTTNG_EXTRA_VERSION_GIT[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_GIT,
#else
		"",
#endif
#ifdef LTTNG_EXTRA_VERSION_NAME
		LTTNG_EXTRA_VERSION_NAME[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_NAME);
#else
		"");
#endif
	return ret;
}

module_init(lttng_events_init);

static void __exit lttng_events_exit(void)
{
	struct lttng_session *session, *tmpsession;

	lttng_exit_cpu_hotplug();
	lttng_logger_exit();
	lttng_abi_exit();
	list_for_each_entry_safe(session, tmpsession, &sessions, list)
		lttng_session_destroy(session);
	kmem_cache_destroy(event_cache);
	kmem_cache_destroy(event_notifier_cache);
	lttng_tracepoint_exit();
	lttng_context_exit();
	printk(KERN_NOTICE "LTTng: Unloaded modules v%s.%s.%s%s (%s)%s%s\n",
		__stringify(LTTNG_MODULES_MAJOR_VERSION),
		__stringify(LTTNG_MODULES_MINOR_VERSION),
		__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION),
		LTTNG_MODULES_EXTRAVERSION,
		LTTNG_VERSION_NAME,
#ifdef LTTNG_EXTRA_VERSION_GIT
		LTTNG_EXTRA_VERSION_GIT[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_GIT,
#else
		"",
#endif
#ifdef LTTNG_EXTRA_VERSION_NAME
		LTTNG_EXTRA_VERSION_NAME[0] == '\0' ? "" : " - " LTTNG_EXTRA_VERSION_NAME);
#else
		"");
#endif
}

module_exit(lttng_events_exit);

#include <generated/patches.h>
#ifdef LTTNG_EXTRA_VERSION_GIT
MODULE_INFO(extra_version_git, LTTNG_EXTRA_VERSION_GIT);
#endif
#ifdef LTTNG_EXTRA_VERSION_NAME
MODULE_INFO(extra_version_name, LTTNG_EXTRA_VERSION_NAME);
#endif
MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers <mathieu.desnoyers@efficios.com>");
MODULE_DESCRIPTION("LTTng tracer");
MODULE_VERSION(__stringify(LTTNG_MODULES_MAJOR_VERSION) "."
	__stringify(LTTNG_MODULES_MINOR_VERSION) "."
	__stringify(LTTNG_MODULES_PATCHLEVEL_VERSION)
	LTTNG_MODULES_EXTRAVERSION);