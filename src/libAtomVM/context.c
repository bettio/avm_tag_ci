/*
 * This file is part of AtomVM.
 *
 * Copyright 2017 Davide Bettio <davide@uninstall.it>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
 */

#include "context.h"

#include <fenv.h>
#include <math.h>

#include "defaultatoms.h"
#include "dictionary.h"
#include "erl_nif.h"
#include "erl_nif_priv.h"
#include "globalcontext.h"
#include "list.h"
#include "mailbox.h"
#include "smp.h"
#include "synclist.h"
#include "sys.h"
#include "term.h"
#include "utils.h"

#ifdef HAVE_PLATFORM_ATOMIC_H
#include "platform_atomic.h"
#else
#if defined(HAVE_ATOMIC)
#include <stdatomic.h>
#define ATOMIC_COMPARE_EXCHANGE_WEAK_INT atomic_compare_exchange_weak
#endif
#endif

#define IMPL_EXECUTE_LOOP
#include "opcodesswitch.h"
#undef IMPL_EXECUTE_LOOP

#define DEFAULT_STACK_SIZE 8
#define BYTES_PER_TERM (TERM_BITS / 8)

static struct Monitor *context_monitors_handle_terminate(Context *ctx);
static void destroy_extended_registers(Context *ctx, unsigned int live);

Context *context_new(GlobalContext *glb)
{
    Context *ctx = malloc(sizeof(Context));
    if (IS_NULL_PTR(ctx)) {
        fprintf(stderr, "Failed to allocate memory: %s:%i.\n", __FILE__, __LINE__);
        return NULL;
    }
    ctx->cp = 0;

    if (UNLIKELY(memory_init_heap(&ctx->heap, DEFAULT_STACK_SIZE) != MEMORY_GC_OK)) {
        fprintf(stderr, "Failed to allocate memory: %s:%i.\n", __FILE__, __LINE__);
        free(ctx);
        return NULL;
    }
    ctx->e = ctx->heap.heap_end;

    context_clean_registers(ctx, 0);
    list_init(&ctx->extended_x_regs);

    ctx->fr = NULL;

    ctx->min_heap_size = 0;
    ctx->max_heap_size = 0;
    ctx->heap_growth_strategy = BoundedFreeHeapGrowth;
    ctx->has_min_heap_size = 0;
    ctx->has_max_heap_size = 0;

    mailbox_init(&ctx->mailbox);

    list_init(&ctx->dictionary);

    ctx->native_handler = NULL;

    ctx->saved_module = NULL;
    ctx->saved_ip = NULL;
    ctx->restore_trap_handler = NULL;

    ctx->leader = 0;

    timer_list_item_init(&ctx->timer_list_head, 0);

    list_init(&ctx->monitors_head);

    ctx->trap_exit = false;
#ifdef ENABLE_ADVANCED_TRACE
    ctx->trace_calls = 0;
    ctx->trace_call_args = 0;
    ctx->trace_returns = 0;
    ctx->trace_send = 0;
    ctx->trace_receive = 0;
#endif

    ctx->flags = NoFlags;
    ctx->platform_data = NULL;

    ctx->group_leader = term_from_local_process_id(INVALID_PROCESS_ID);

    ctx->bs = term_invalid_term();
    ctx->bs_offset = 0;

    ctx->exit_reason = NORMAL_ATOM;

    globalcontext_init_process(glb, ctx);

    return ctx;
}

void context_destroy(Context *ctx)
{
    // Another process can get an access to our mailbox until this point.
    struct ListHead *processes_table_list = synclist_wrlock(&ctx->global->processes_table);
    UNUSED(processes_table_list);

    list_remove(&ctx->processes_table_head);

    // Ensure process is not registered
    globalcontext_maybe_unregister_process_id(ctx->global, ctx->process_id);

    // Process any link/unlink/monitor/demonitor signal that arrived recently
    // Also process ProcessInfoRequestSignal so caller isn't trapped waiting
    MailboxMessage *signal_message = mailbox_process_outer_list(&ctx->mailbox);
    while (signal_message) {
        if (signal_message->type == ProcessInfoRequestSignal) {
            struct BuiltInAtomRequestSignal *request_signal
                = CONTAINER_OF(signal_message, struct BuiltInAtomRequestSignal, base);
            context_process_process_info_request_signal(ctx, request_signal);
        } else if (signal_message->type == MonitorSignal) {
            struct MonitorPointerSignal *monitor_signal
                = CONTAINER_OF(signal_message, struct MonitorPointerSignal, base);
            context_add_monitor(ctx, monitor_signal->monitor);
        } else if (signal_message->type == UnlinkSignal) {
            struct ImmediateSignal *immediate_signal
                = CONTAINER_OF(signal_message, struct ImmediateSignal, base);
            context_unlink(ctx, immediate_signal->immediate);
        } else if (signal_message->type == DemonitorSignal) {
            struct RefSignal *ref_signal
                = CONTAINER_OF(signal_message, struct RefSignal, base);
            context_demonitor(ctx, ref_signal->ref_ticks);
        }
        MailboxMessage *next = signal_message->next;
        mailbox_message_dispose(signal_message, &ctx->heap);
        signal_message = next;
    }

    // When monitor message is sent, process is no longer in the table
    // and is no longer registered either.
    struct Monitor *resource_monitors = context_monitors_handle_terminate(ctx);

    synclist_unlock(&ctx->global->processes_table);

    // Eventually call resource monitors handlers after the processes table was unlocked
    // The monitors were removed from the list of monitors.
    if (resource_monitors) {
        ErlNifEnv env;
        erl_nif_env_partial_init_from_globalcontext(&env, ctx->global);

        struct ListHead monitors;
        list_prepend(&resource_monitors->monitor_list_head, &monitors);

        struct ListHead *item;
        struct ListHead *tmp;
        MUTABLE_LIST_FOR_EACH (item, tmp, &monitors) {
            struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
            void *resource = term_to_term_ptr(monitor->monitor_obj);
            struct RefcBinary *refc = refc_binary_from_data(resource);
            resource_type_fire_monitor(refc->resource_type, &env, resource, ctx->process_id, monitor->ref_ticks);
            free(monitor);
        }
    }

    // Any other process released our mailbox, so we can clear it.
    mailbox_destroy(&ctx->mailbox, &ctx->heap);

    destroy_extended_registers(ctx, 0);
    free(ctx->fr);

    memory_destroy_heap(&ctx->heap, ctx->global);

    dictionary_destroy(&ctx->dictionary);

    if (ctx->timer_list_head.head.next != &ctx->timer_list_head.head) {
        scheduler_cancel_timeout(ctx);
    }

    // Platform data is freed here to allow drivers to use the
    // globalcontext_get_process_lock lock to protect this pointer
    // Typically, another thread or an interrupt would call
    // globalcontext_get_process_lock before accessing platform_data.
    // Here, the context can no longer be acquired with
    // globalcontext_get_process_lock, so it's safe to free the pointer.
    free(ctx->platform_data);

    free(ctx);
}

void context_process_kill_signal(Context *ctx, struct TermSignal *signal)
{
    // exit_reason is one of the roots when garbage collecting
    ctx->exit_reason = signal->signal_term;
    context_update_flags(ctx, ~NoFlags, Killed);
}

void context_process_process_info_request_signal(Context *ctx, struct BuiltInAtomRequestSignal *signal)
{
    Context *target = globalcontext_get_process_lock(ctx->global, signal->sender_pid);
    if (target) {
        size_t term_size;
        if (context_get_process_info(ctx, NULL, &term_size, signal->atom, NULL)) {
            Heap heap;
            if (UNLIKELY(memory_init_heap(&heap, term_size) != MEMORY_GC_OK)) {
                mailbox_send_immediate_signal(target, TrapExceptionSignal, OUT_OF_MEMORY_ATOM);
            } else {
                term ret;
                if (context_get_process_info(ctx, &ret, NULL, signal->atom, &heap)) {
                    mailbox_send_term_signal(target, TrapAnswerSignal, ret);
                } else {
                    mailbox_send_immediate_signal(target, TrapExceptionSignal, ret);
                }
                memory_destroy_heap(&heap, ctx->global);
            }
        } else {
            mailbox_send_immediate_signal(target, TrapExceptionSignal, BADARG_ATOM);
        }
        globalcontext_get_process_unlock(ctx->global, target);
    } // else: sender died
}

bool context_process_signal_trap_answer(Context *ctx, struct TermSignal *signal)
{
    context_update_flags(ctx, ~Trap, NoFlags);
    ctx->x[0] = signal->signal_term;
    return true;
}

void context_process_flush_monitor_signal(Context *ctx, uint64_t ref_ticks, bool info)
{
    context_update_flags(ctx, ~Trap, NoFlags);
    bool result = true;
    mailbox_reset(&ctx->mailbox);
    term msg;
    while (mailbox_peek(ctx, &msg)) {
        if (term_is_tuple(msg)
            && term_get_tuple_arity(msg) == 5
            && term_get_tuple_element(msg, 0) == DOWN_ATOM
            && term_is_reference(term_get_tuple_element(msg, 1))
            && term_to_ref_ticks(term_get_tuple_element(msg, 1)) == ref_ticks) {
            mailbox_remove_message(&ctx->mailbox, &ctx->heap);
            // If option info is combined with option flush, false is returned if a flush was needed, otherwise true.
            result = !info;
        } else {
            mailbox_next(&ctx->mailbox);
        }
    }
    mailbox_reset(&ctx->mailbox);
    ctx->x[0] = result ? TRUE_ATOM : FALSE_ATOM;
}

void context_update_flags(Context *ctx, int mask, int value) CLANG_THREAD_SANITIZE_SAFE
{
#ifndef AVM_NO_SMP
    enum ContextFlags expected = ctx->flags;
    enum ContextFlags desired;
    do {
        desired = (expected & mask) | value;
    } while (!ATOMIC_COMPARE_EXCHANGE_WEAK_INT(&ctx->flags, &expected, desired));
#else
    ctx->flags = (ctx->flags & mask) | value;
#endif
}

size_t context_message_queue_len(Context *ctx)
{
    return mailbox_len(&ctx->mailbox);
}

size_t context_size(Context *ctx)
{
    size_t messages_size = mailbox_size(&ctx->mailbox);

    // TODO include ctx->platform_data
    return sizeof(Context)
        + messages_size
        + memory_heap_memory_size(&ctx->heap) * BYTES_PER_TERM;
}

bool context_get_process_info(Context *ctx, term *out, size_t *term_size, term atom_key, Heap *heap)
{
    size_t ret_size;
    switch (atom_key) {
        case HEAP_SIZE_ATOM:
        case TOTAL_HEAP_SIZE_ATOM:
        case STACK_SIZE_ATOM:
        case MESSAGE_QUEUE_LEN_ATOM:
        case MEMORY_ATOM:
            ret_size = TUPLE_SIZE(2);
            break;
        case LINKS_ATOM: {
            struct ListHead *item;
            size_t links_count = 0;
            LIST_FOR_EACH (item, &ctx->monitors_head) {
                struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
                if (monitor->ref_ticks == 0) {
                    links_count++;
                }
            }
            ret_size = TUPLE_SIZE(2) + CONS_SIZE * links_count;
            break;
        }
        default:
            if (out != NULL) {
                *out = BADARG_ATOM;
            }
            return false;
    }
    if (term_size != NULL) {
        *term_size = ret_size;
    }
    if (out == NULL) {
        return true;
    }

    term ret = term_alloc_tuple(2, heap);
    switch (atom_key) {
        // heap_size size in words of the heap of the process
        case HEAP_SIZE_ATOM: {
            term_put_tuple_element(ret, 0, HEAP_SIZE_ATOM);
            unsigned long value = memory_heap_youngest_size(&ctx->heap);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // total_heap_size size in words of the heap of the process, including fragments
        case TOTAL_HEAP_SIZE_ATOM: {
            term_put_tuple_element(ret, 0, TOTAL_HEAP_SIZE_ATOM);
            unsigned long value = memory_heap_memory_size(&ctx->heap);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // stack_size stack size, in words, of the process
        case STACK_SIZE_ATOM: {
            term_put_tuple_element(ret, 0, STACK_SIZE_ATOM);
            unsigned long value = context_stack_size(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // message_queue_len number of messages currently in the message queue of the process
        case MESSAGE_QUEUE_LEN_ATOM: {
            term_put_tuple_element(ret, 0, MESSAGE_QUEUE_LEN_ATOM);
            unsigned long value = context_message_queue_len(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // memory size in bytes of the process. This includes call stack, heap, and internal structures.
        case MEMORY_ATOM: {
            term_put_tuple_element(ret, 0, MEMORY_ATOM);
            unsigned long value = context_size(ctx);
            term_put_tuple_element(ret, 1, term_from_int32(value));
            break;
        }

        // pids of linked processes
        case LINKS_ATOM: {
            term_put_tuple_element(ret, 0, LINKS_ATOM);
            term list = term_nil();
            struct ListHead *item;
            LIST_FOR_EACH (item, &ctx->monitors_head) {
                struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
                // Links are struct Monitor entries with ref_ticks equal to 0
                if (monitor->ref_ticks == 0) {
                    list = term_list_prepend(monitor->monitor_obj, list, heap);
                }
            }
            term_put_tuple_element(ret, 1, list);
            break;
        }

        default:
            UNREACHABLE();
    }
    *out = ret;
    return true;
}

static struct Monitor *context_monitors_handle_terminate(Context *ctx)
{
    GlobalContext *glb = ctx->global;
    struct ListHead *item;
    struct ListHead *tmp;
    struct Monitor *result = NULL;
    MUTABLE_LIST_FOR_EACH (item, tmp, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if (monitor->ref_ticks && ((monitor->monitor_obj & 0x3) == CONTEXT_MONITOR_RESOURCE_TAG)) {
            // remove it from the list we are iterating on
            if (result == NULL) {
                list_init(&monitor->monitor_list_head);
                result = monitor;
            } else {
                list_append(&result->monitor_list_head, &monitor->monitor_list_head);
            }
        } else if (monitor->ref_ticks == 0 || ((monitor->monitor_obj & 0x3) == CONTEXT_MONITOR_MONITORED_PID_TAG)) {
            // term_to_local_process_id and monitor->monitor_obj >> 4 are identical here.
            int local_process_id = term_to_local_process_id(monitor->monitor_obj);
            Context *target = globalcontext_get_process_nolock(glb, local_process_id);
            if (IS_NULL_PTR(target)) {
                // TODO: assess whether this can happen
                free(monitor);
                continue;
            }

            if (monitor->ref_ticks == 0) {
                term exited_pid = term_from_local_process_id(ctx->process_id);
                mailbox_send_immediate_signal(target, UnlinkSignal, exited_pid);
                if (target->trap_exit) {
                    if (UNLIKELY(memory_ensure_free(ctx, TUPLE_SIZE(3)) != MEMORY_GC_OK)) {
                        // TODO: handle out of memory here
                        fprintf(stderr, "Cannot handle out of memory.\n");
                        globalcontext_get_process_unlock(glb, target);
                        AVM_ABORT();
                    }
                    // Prepare the message on ctx's heap which will be freed afterwards.
                    term info_tuple = term_alloc_tuple(3, &ctx->heap);
                    term_put_tuple_element(info_tuple, 0, EXIT_ATOM);
                    term_put_tuple_element(info_tuple, 1, exited_pid);
                    term_put_tuple_element(info_tuple, 2, ctx->exit_reason);
                    mailbox_send(target, info_tuple);
                } else if (ctx->exit_reason != NORMAL_ATOM) {
                    mailbox_send_term_signal(target, KillSignal, ctx->exit_reason);
                }
            } else {
                mailbox_send_ref_signal(target, DemonitorSignal, monitor->ref_ticks);
                int required_terms = REF_SIZE + TUPLE_SIZE(5);
                if (UNLIKELY(memory_ensure_free(ctx, required_terms) != MEMORY_GC_OK)) {
                    // TODO: handle out of memory here
                    fprintf(stderr, "Cannot handle out of memory.\n");
                    globalcontext_get_process_unlock(glb, target);
                    AVM_ABORT();
                }

                // Prepare the message on ctx's heap which will be freed afterwards.
                term ref = term_from_ref_ticks(monitor->ref_ticks, &ctx->heap);

                term info_tuple = term_alloc_tuple(5, &ctx->heap);
                term_put_tuple_element(info_tuple, 0, DOWN_ATOM);
                term_put_tuple_element(info_tuple, 1, ref);
                if (ctx->native_handler != NULL) {
                    term_put_tuple_element(info_tuple, 2, PORT_ATOM);
                } else {
                    term_put_tuple_element(info_tuple, 2, PROCESS_ATOM);
                }
                term_put_tuple_element(info_tuple, 3, term_from_local_process_id(ctx->process_id));
                term_put_tuple_element(info_tuple, 4, ctx->exit_reason);

                mailbox_send(target, info_tuple);
            }
            free(monitor);
        } else {
            // We are the monitoring process.
            int local_process_id = monitor->monitor_obj >> 4;
            Context *target = globalcontext_get_process_nolock(glb, local_process_id);
            mailbox_send_ref_signal(target, DemonitorSignal, monitor->ref_ticks);
            free(monitor);
        }
    }
    return result;
}

struct Monitor *monitor_link_new(term link_pid)
{
    struct Monitor *monitor = malloc(sizeof(struct Monitor));
    if (IS_NULL_PTR(monitor)) {
        return NULL;
    }
    monitor->monitor_obj = link_pid;
    monitor->ref_ticks = 0;

    return monitor;
}

struct Monitor *monitor_new(term monitor_pid, uint64_t ref_ticks, bool is_monitoring)
{
    struct Monitor *monitor = malloc(sizeof(struct Monitor));
    if (IS_NULL_PTR(monitor)) {
        return NULL;
    }
    int32_t local_process_id = term_to_local_process_id(monitor_pid);
    if (is_monitoring) {
        monitor->monitor_obj = (local_process_id << 4) | CONTEXT_MONITOR_MONITORING_PID_TAG;
    } else {
        monitor->monitor_obj = (local_process_id << 4) | CONTEXT_MONITOR_MONITORED_PID_TAG;
    }
    monitor->ref_ticks = ref_ticks;

    return monitor;
}

struct Monitor *monitor_resource_monitor_new(void *resource, uint64_t ref_ticks)
{
    struct Monitor *monitor = malloc(sizeof(struct Monitor));
    if (IS_NULL_PTR(monitor)) {
        return NULL;
    }
    monitor->monitor_obj = ((term) resource) | CONTEXT_MONITOR_RESOURCE_TAG;
    monitor->ref_ticks = ref_ticks;

    return monitor;
}

void context_add_monitor(Context *ctx, struct Monitor *new_monitor)
{
    struct ListHead *item;
    LIST_FOR_EACH (item, &ctx->monitors_head) {
        struct Monitor *existing = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if ((existing->monitor_obj == new_monitor->monitor_obj) && (existing->ref_ticks == new_monitor->ref_ticks)) {
            free(new_monitor);
            return;
        }
    }
    list_append(&ctx->monitors_head, &new_monitor->monitor_list_head);
}

void context_unlink(Context *ctx, term link_pid)
{
    struct ListHead *item;
    LIST_FOR_EACH (item, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if ((monitor->monitor_obj == link_pid) && (monitor->ref_ticks == 0)) {
            list_remove(&monitor->monitor_list_head);
            free(monitor);
            return;
        }
    }
}

void context_demonitor(Context *ctx, uint64_t ref_ticks)
{
    struct ListHead *item;
    LIST_FOR_EACH (item, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if (monitor->ref_ticks == ref_ticks) {
            list_remove(&monitor->monitor_list_head);
            free(monitor);
            return;
        }
    }
}

term context_get_monitor_pid(Context *ctx, uint64_t ref_ticks, bool *is_monitoring)
{
    struct ListHead *item;
    LIST_FOR_EACH (item, &ctx->monitors_head) {
        struct Monitor *monitor = GET_LIST_ENTRY(item, struct Monitor, monitor_list_head);
        if (monitor->ref_ticks == ref_ticks) {
            if ((monitor->monitor_obj & 0x3) == CONTEXT_MONITOR_MONITORED_PID_TAG) {
                *is_monitoring = false;
                return term_from_local_process_id((uint32_t) (monitor->monitor_obj >> 4));
            } else if ((monitor->monitor_obj & 0x3) == CONTEXT_MONITOR_MONITORING_PID_TAG) {
                *is_monitoring = true;
                return term_from_local_process_id((uint32_t) (monitor->monitor_obj >> 4));
            } else {
                return term_invalid_term();
            }
        }
    }
    return term_invalid_term();
}
