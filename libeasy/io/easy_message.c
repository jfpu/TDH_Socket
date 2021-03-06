/*
* Copyright(C) 2011-2012 Alibaba Group Holding Limited
* 
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*/


#include "easy_io.h"
#include "easy_message.h"
#include "easy_connection.h"
#include "easy_request.h"
#include "easy_baseth_pool.h"

easy_message_t *easy_message_create(easy_connection_t *c)
{
    easy_pool_t         *pool;
    easy_message_t      *m;
    easy_buf_t          *input;
    int                 size;

    if ((pool = easy_pool_create(c->default_message_len)) == NULL)
        return NULL;

    // 新建一个message
    m = (easy_message_t *)easy_pool_calloc(pool, sizeof(easy_message_t));
    size = pool->end - pool->last - sizeof(easy_buf_t);
    size = ((size < 0) ? EASY_POOL_ALIGNMENT : size);
    input = easy_buf_create(pool, size);

    if (m == NULL || input == NULL) {
        easy_pool_destroy(pool);
        return NULL;
    }

    pool->ref = 1;
    m->pool = pool;
    m->c = c;
    m->next_read_len = size;
    m->input = input;
    m->type = EASY_TYPE_MESSAGE;
    m->request_list_count = 0;
    easy_list_init(&m->request_list);
    easy_list_init(&m->request_done_list);
    easy_list_add_tail(&m->message_list_node, &c->message_list);

    return m;
}

/**
 * destroy掉easy_message_t对象
 *
 * @param m - easy_message_t对象
 */
void easy_message_destroy(easy_message_t *m, int del)
{
    easy_request_t  *r, *n;

    // delete from message_list
    if (del) {
        m->status = EASY_MESG_DESTROY;
        easy_list_del(&m->message_list_node);
    }

    if (easy_atomic_add_return(&m->pool->ref, -1) == 0) {
        // server done
        easy_list_for_each_entry_safe(r, n, &m->request_list, request_list_node) {
            easy_list_del(&r->request_list_node);
            easy_request_server_done(r);
        }
        easy_list_for_each_entry_safe(r, n, &m->request_done_list, request_list_node) {
            easy_list_del(&r->request_list_node);
            easy_request_server_done(r);
        }

        easy_list_del(&m->message_list_node);

        if (m->input) easy_buf_destroy(m->input);

        easy_pool_destroy(m->pool);
    }
}

/**
 * 用于回调
 */
void easy_message_cleanup(easy_buf_t *b, void *args)
{
    easy_message_t *m = (easy_message_t *) args;
    easy_message_destroy(m, 0);
}

/**
 * 新建一个session_t
 */
easy_session_t *easy_session_create(int size)
{
    easy_pool_t     *pool;
    easy_session_t  *s;

    // 新建一个pool
    size += sizeof(easy_session_t);

    if ((pool = easy_pool_create(size)) == NULL)
        return NULL;

    // 新建一个message
    if ((s = (easy_session_t *)easy_pool_alloc(pool, size)) == NULL) {
        easy_pool_destroy(pool);
        return NULL;
    }

    memset(s, 0, sizeof(easy_session_t));
    s->pool = pool;
    s->r.ms = (easy_message_session_t *)s;
    s->type = EASY_TYPE_SESSION;
    easy_list_init(&s->session_list_node);

    return s;
}

/**
 * destroy掉easy_session_t对象
 *
 * @param s - easy_session_t对象
 */
void easy_session_destroy(void *data)
{
    easy_message_t          *m;
    easy_session_t          *s;

    s = (easy_session_t *) data;

    if (s->cleanup)
        (s->cleanup)(&s->r, NULL);

    // 如果存在
    if (s->async && (m = (easy_message_t *)s->r.request_list_node.next)) {
        s->r.request_list_node.next = NULL;
        easy_message_destroy(m, 0);
    }

    easy_pool_destroy(s->pool);
}

int easy_session_process(easy_session_t *s, int stop)
{
    if (stop) {
        ev_timer_stop(s->c->loop, &s->timeout_watcher);
        easy_list_del(&s->session_list_node);
        easy_request_client_done(&s->r);
        s->c->pool->ref --;
    }

    // timeout, 把output里的clear掉
    if (s->r.ipacket == NULL && s->nextb) {
        easy_buf_t *b, *b2;
        easy_list_for_each_entry_safe(b, b2, s->nextb, node) {
            if (b->args != s->pool) break;

            easy_list_del(&b->node);
        }
        easy_list_del(s->nextb);
    }

    if (s->process) {
        return (s->process)(&s->r);
    } else {
        easy_error_log("session process is null, s = %p\n", s);
        easy_session_destroy(s);
        return EASY_ERROR;
    }
}
