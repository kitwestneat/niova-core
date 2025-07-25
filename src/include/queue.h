/*	$OpenBSD: queue.h,v 1.32 2007/04/30 18:42:34 pedro Exp $	*/
/*	$NetBSD: queue.h,v 1.11 1996/05/16 05:17:14 mycroft Exp $	*/

/*
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)queue.h	8.5 (Berkeley) 8/20/94
 */

#ifndef _SYS_QUEUE_H_
#define _SYS_QUEUE_H_

/*
 * This file defines five types of data structures: singly-linked lists,
 * lists, simple queues, tail queues, and circular queues.
 *
 *
 * A singly-linked list is headed by a single forward pointer. The elements
 * are singly linked for minimum space and pointer manipulation overhead at
 * the expense of O(n) removal for arbitrary elements. New elements can be
 * added to the list after an existing element or at the head of the list.
 * Elements being removed from the head of the list should use the explicit
 * macro for this purpose for optimum efficiency. A singly-linked list may
 * only be traversed in the forward direction.  Singly-linked lists are ideal
 * for applications with large datasets and few or no removals or for
 * implementing a LIFO queue.
 *
 * A list is headed by a single forward pointer (or an array of forward
 * pointers for a hash table header). The elements are doubly linked
 * so that an arbitrary element can be removed without a need to
 * traverse the list. New elements can be added to the list before
 * or after an existing element or at the head of the list. A list
 * may only be traversed in the forward direction.
 *
 * A simple queue is headed by a pair of pointers, one the head of the
 * list and the other to the tail of the list. The elements are singly
 * linked to save space, so elements can only be removed from the
 * head of the list. New elements can be added to the list before or after
 * an existing element, at the head of the list, or at the end of the
 * list. A simple queue may only be traversed in the forward direction.
 *
 * A tail queue is headed by a pair of pointers, one to the head of the
 * list and the other to the tail of the list. The elements are doubly
 * linked so that an arbitrary element can be removed without a need to
 * traverse the list. New elements can be added to the list before or
 * after an existing element, at the head of the list, or at the end of
 * the list. A tail queue may be traversed in either direction.
 *
 * A circle queue is headed by a pair of pointers, one to the head of the
 * list and the other to the tail of the list. The elements are doubly
 * linked so that an arbitrary element can be removed without a need to
 * traverse the list. New elements can be added to the list before or after
 * an existing element, at the head of the list, or at the end of the list.
 * A circle queue may be traversed in either direction, but has a more
 * complex end of list detection.
 *
 * For details on the use of these macros, see the queue(3) manual page.
 */

#if defined(QUEUE_MACRO_DEBUG) || (defined(_KERNEL) && defined(DIAGNOSTIC))
#define _Q_INVALIDATE(a) (a) = ((void *)-1)
#else
#define _Q_INVALIDATE(a)
#endif

#ifdef QUEUE_MACRO_DEBUG
/* Store the last 2 places the queue element or head was altered */
struct qm_trace {
    char *lastfile;
    int   lastline;
    char *prevfile;
    int   prevline;
};

#define TRACEBUF        struct qm_trace trace;
#define TRASHIT(x)      do {(x) = (void *)-1;} while (0)
#define QMD_SAVELINK(name, link)        void **name = (void *)&(link)

#define QMD_TRACE_HEAD(head) do {                        \
        (head)->trace.prevline = (head)->trace.lastline; \
        (head)->trace.prevfile = (head)->trace.lastfile; \
        (head)->trace.lastline = __LINE__;               \
        (head)->trace.lastfile = __FILE__;               \
} while (0)

#define QMD_TRACE_ELEM(elem) do {                        \
        (elem)->trace.prevline = (elem)->trace.lastline; \
        (elem)->trace.prevfile = (elem)->trace.lastfile; \
        (elem)->trace.lastline = __LINE__;               \
        (elem)->trace.lastfile = __FILE__;               \
} while (0)

#else
#define QMD_TRACE_ELEM(elem)
#define QMD_TRACE_HEAD(head)
#define QMD_SAVELINK(name, link)
#define TRACEBUF
#define TRASHIT(x)
#endif /* QUEUE_MACRO_DEBUG */

/*
 * Singly-linked List definitions.
 */
#define SLIST_HEAD(name, type)                      \
struct name {                                       \
        struct type *slh_first; /* first element */ \
}

#define SLIST_HEAD_INITIALIZER(head) \
        { NULL }

#define SLIST_ENTRY(type)                          \
struct {                                           \
        struct type *sle_next;  /* next element */ \
}

#define SLIST_ENTRY_INIT(entry) \
    { (entry)->sle_next = NULL; }

#define SLIST_ENTRY_DETACHED(entry) \
    ((entry)->sle_next == NULL)

/*
 * Singly-linked List access methods.
 */
#define SLIST_FIRST(head)       ((head)->slh_first)
#define SLIST_END(head)         NULL
#define SLIST_EMPTY(head)       (SLIST_FIRST(head) == SLIST_END(head))
#define SLIST_NEXT(elm, field)  ((elm)->field.sle_next)

#define SLIST_FOREACH(var, head, field) \
        for((var) = SLIST_FIRST(head);  \
            (var) != SLIST_END(head);   \
            (var) = SLIST_NEXT(var, field))

#define SLIST_FOREACH_PREVPTR(var, varp, head, field) \
        for ((varp) = &SLIST_FIRST((head));           \
            ((var) = *(varp)) != SLIST_END(head);     \
            (varp) = &SLIST_NEXT((var), field))

#define SLIST_FOREACH_SAFE(var, head, field, tvar)         \
        for ((var) = SLIST_FIRST((head));                    \
            (var) && ((tvar) = SLIST_NEXT((var), field), 1); \
            (var) = (tvar))

/*
 * Singly-linked List functions.
 */
#define SLIST_INIT(head) {                   \
        SLIST_FIRST(head) = SLIST_END(head); \
}

#define SLIST_INSERT_AFTER(slistelm, elm, field) do {       \
        (elm)->field.sle_next = (slistelm)->field.sle_next; \
        (slistelm)->field.sle_next = (elm);                 \
} while (0)

#define SLIST_INSERT_HEAD(head, elm, field) do {   \
        (elm)->field.sle_next = (head)->slh_first; \
        (head)->slh_first = (elm);                 \
} while (0)

#define SLIST_REMOVE_NEXT(head, elm, field) do {                       \
        (elm)->field.sle_next = (elm)->field.sle_next->field.sle_next; \
} while (0)

#define SLIST_REMOVE_HEAD(head, field) do {                    \
        (head)->slh_first = (head)->slh_first->field.sle_next; \
} while (0)

#define SLIST_REMOVE(head, elm, type, field) do {           \
        if ((head)->slh_first == (elm)) {                   \
                SLIST_REMOVE_HEAD((head), field);           \
        } else {                                            \
                struct type *curelm = (head)->slh_first;    \
                                                            \
                while (curelm->field.sle_next != (elm))     \
                        curelm = curelm->field.sle_next;    \
                curelm->field.sle_next =                    \
                    curelm->field.sle_next->field.sle_next; \
                _Q_INVALIDATE((elm)->field.sle_next);       \
        }                                                   \
} while (0)

#define SLIST_SWAP(head1, head2, type) do {				\
	struct type *swap_first = SLIST_FIRST(head1);		\
	SLIST_FIRST(head1) = SLIST_FIRST(head2);			\
	SLIST_FIRST(head2) = swap_first;				\
} while (0)

/*
 * Singly-linked Tail queue declarations.
 */
#define STAILQ_HEAD(name, type)                                 \
struct name {                                                   \
        struct type *stqh_first;/* first element */             \
        struct type **stqh_last;/* addr of last next element */ \
}

#define STAILQ_HEAD_INITIALIZER(head) \
        { NULL, &(head).stqh_first }

#define STAILQ_ENTRY(type)                         \
struct {                                           \
        struct type *stqe_next; /* next element */ \
}

#define STAILQ_NEXT_ENTRY_DETACHED(entry) \
    ((entry)->stqe_next == NULL)

#define STAILQ_ENTRY_INIT(entry)                \
    { (entry)->stqe_next = NULL; }
/*
 * Singly-linked Tail queue functions.
 */

/**
 * STAILQ_CONCAT() places the contents of head2 at the end of head1.
 */
#define STAILQ_CONCAT(head1, head2) do {                   \
        if (!STAILQ_EMPTY((head2))) {                      \
                *(head1)->stqh_last = (head2)->stqh_first; \
                (head1)->stqh_last = (head2)->stqh_last;   \
                STAILQ_INIT((head2));                      \
        }                                                  \
} while (0)

/**
 * STAILQ_CONCAT_TO_HEAD() places the contents of 'src' to the head of 'dest'.
 */
#define STAILQ_CONCAT_TO_HEAD(src, dest) do {                 \
        if (!STAILQ_EMPTY((src))) {                           \
            if (STAILQ_EMPTY((dest))) {                       \
                *(dest) = *(src);                             \
            } else {                                          \
                *(src)->stqh_last = (dest)->stqh_first;       \
                (dest)->stqh_first = (src)->stqh_first;       \
            }                                                 \
            STAILQ_INIT((src));                               \
        }                                                     \
} while (0)

#define STAILQ_EMPTY(head)      ((head)->stqh_first == NULL)

#define STAILQ_FIRST(head)      ((head)->stqh_first)

#define STAILQ_FOREACH(var, head, field)  \
        for((var) = STAILQ_FIRST((head)); \
           (var);                         \
           (var) = STAILQ_NEXT((var), field))

#define STAILQ_FOREACH_SAFE(var, head, field, tvar)           \
        for ((var) = STAILQ_FIRST((head));                    \
            (var) && ((tvar) = STAILQ_NEXT((var), field), 1); \
            (var) = (tvar))

#define STAILQ_INIT(head) do {                     \
        STAILQ_FIRST((head)) = NULL;               \
        (head)->stqh_last = &STAILQ_FIRST((head)); \
} while (0)

#define STAILQ_INSERT_AFTER(head, tqelm, elm, field) do {                      \
        if ((STAILQ_NEXT((elm), field) = STAILQ_NEXT((tqelm), field)) == NULL) \
                (head)->stqh_last = &STAILQ_NEXT((elm), field);                \
        STAILQ_NEXT((tqelm), field) = (elm);                                   \
} while (0)

#define STAILQ_INSERT_HEAD(head, elm, field) do {                       \
        if ((STAILQ_NEXT((elm), field) = STAILQ_FIRST((head))) == NULL) \
                (head)->stqh_last = &STAILQ_NEXT((elm), field);         \
        STAILQ_FIRST((head)) = (elm);                                   \
} while (0)

#define STAILQ_INSERT_TAIL(head, elm, field) do {       \
        STAILQ_NEXT((elm), field) = NULL;               \
        *(head)->stqh_last = (elm);                     \
        (head)->stqh_last = &STAILQ_NEXT((elm), field); \
} while (0)

#define STAILQ_LAST(head, type, field)   \
        (STAILQ_EMPTY((head)) ?          \
                NULL :                   \
                ((struct type *)(void *) \
                ((char *)((head)->stqh_last) - offsetof(struct type, field))))

#define STAILQ_NEXT(elm, field) ((elm)->field.stqe_next)

#define STAILQ_REMOVE(head, elm, type, field) do {           \
        QMD_SAVELINK(oldnext, (elm)->field.stqe_next);       \
        if (STAILQ_FIRST((head)) == (elm)) {                 \
                STAILQ_REMOVE_HEAD((head), field);           \
        }                                                    \
        else {                                               \
                struct type *curelm = STAILQ_FIRST((head));  \
                while (STAILQ_NEXT(curelm, field) != (elm))  \
                        curelm = STAILQ_NEXT(curelm, field); \
                STAILQ_REMOVE_AFTER(head, curelm, field);    \
        }                                                    \
        TRASHIT(*oldnext);                                   \
} while (0)

#define STAILQ_REMOVE_HEAD(head, field) do {                    \
        if ((STAILQ_FIRST((head)) =                             \
             STAILQ_NEXT(STAILQ_FIRST((head)), field)) == NULL) \
                (head)->stqh_last = &STAILQ_FIRST((head));      \
} while (0)

#define STAILQ_REMOVE_AFTER(head, elm, field) do {                 \
        if ((STAILQ_NEXT(elm, field) =                             \
             STAILQ_NEXT(STAILQ_NEXT(elm, field), field)) == NULL) \
                (head)->stqh_last = &STAILQ_NEXT((elm), field);    \
} while (0)

#define STAILQ_SWAP(head1, head2, type) do {               \
        struct type *swap_first = STAILQ_FIRST(head1);     \
        struct type **swap_last = (head1)->stqh_last;      \
        STAILQ_FIRST(head1) = STAILQ_FIRST(head2);         \
        (head1)->stqh_last = (head2)->stqh_last;           \
        STAILQ_FIRST(head2) = swap_first;                  \
        (head2)->stqh_last = swap_last;                    \
        if (STAILQ_EMPTY(head1))                           \
                (head1)->stqh_last = &STAILQ_FIRST(head1); \
        if (STAILQ_EMPTY(head2))                           \
                (head2)->stqh_last = &STAILQ_FIRST(head2); \
} while (0)


/*
 * List definitions.
 */
#define LIST_HEAD(name, type)                       \
struct name {                                       \
        struct type *lh_first;  /* first element */ \
}

#define LIST_HEAD_INITIALIZER(head) \
        { NULL }

#define LIST_ENTRY(type)                                               \
struct {                                                               \
        struct type *le_next;   /* next element */                     \
        struct type **le_prev;  /* address of previous next element */ \
}

#define LIST_ENTRY_DETACHED(entry) \
    ((entry)->le_next == NULL && (entry)->le_prev == NULL)

#define LIST_ENTRY_INIT(entry) \
    { (entry)->le_next = NULL; (entry)->le_prev = NULL; }

/*
 * List access methods
 */
#define LIST_FIRST(head)                ((head)->lh_first)
#define LIST_END(head)                  NULL
#define LIST_EMPTY(head)                (LIST_FIRST(head) == LIST_END(head))
#define LIST_NEXT(elm, field)           ((elm)->field.le_next)

#define LIST_FOREACH(var, head, field) \
        for((var) = LIST_FIRST(head);  \
            (var)!= LIST_END(head);    \
            (var) = LIST_NEXT(var, field))

#define	LIST_FOREACH_SAFE(var, head, field, tvar)                       \
        for ((var) = LIST_FIRST(head);                                  \
             (var) && ((tvar) = LIST_NEXT(var, field), 1);              \
             (var) = (tvar))

#define LIST_ENTRY_IS_MEMBER(head, elm, field)        \
({                                                    \
    bool found = false;                               \
    typeof (elm) tmp;                                 \
    LIST_FOREACH(tmp, head, field)                    \
    {                                                 \
        if ((elm) == tmp)                             \
        {                                             \
            found = true;                             \
            break;                                    \
        }                                             \
    }                                                 \
    found;                                            \
})

/*
 * List functions.
 */
#define LIST_INIT(head) do {               \
        LIST_FIRST(head) = LIST_END(head); \
} while (0)

#define LIST_INSERT_AFTER(listelm, elm, field) do {                    \
        if (((elm)->field.le_next = (listelm)->field.le_next) != NULL) \
                (listelm)->field.le_next->field.le_prev =              \
                    &(elm)->field.le_next;                             \
        (listelm)->field.le_next = (elm);                              \
        (elm)->field.le_prev = &(listelm)->field.le_next;              \
} while (0)

#define LIST_INSERT_BEFORE(listelm, elm, field) do {      \
        (elm)->field.le_prev = (listelm)->field.le_prev;  \
        (elm)->field.le_next = (listelm);                 \
        *(listelm)->field.le_prev = (elm);                \
        (listelm)->field.le_prev = &(elm)->field.le_next; \
} while (0)

#define LIST_INSERT_HEAD(head, elm, field) do {                          \
        if (((elm)->field.le_next = (head)->lh_first) != NULL)           \
                (head)->lh_first->field.le_prev = &(elm)->field.le_next; \
        (head)->lh_first = (elm);                                        \
        (elm)->field.le_prev = &(head)->lh_first;                        \
} while (0)

#define LIST_REMOVE(elm, field) do {                  \
        if ((elm)->field.le_next != NULL)             \
                (elm)->field.le_next->field.le_prev = \
                    (elm)->field.le_prev;             \
        *(elm)->field.le_prev = (elm)->field.le_next; \
        _Q_INVALIDATE((elm)->field.le_prev);          \
        _Q_INVALIDATE((elm)->field.le_next);          \
} while (0)

#define LIST_REPLACE(elm, elm2, field) do {                         \
        if (((elm2)->field.le_next = (elm)->field.le_next) != NULL) \
                (elm2)->field.le_next->field.le_prev =              \
                    &(elm2)->field.le_next;                         \
        (elm2)->field.le_prev = (elm)->field.le_prev;               \
        *(elm2)->field.le_prev = (elm2);                            \
        _Q_INVALIDATE((elm)->field.le_prev);                        \
        _Q_INVALIDATE((elm)->field.le_next);                        \
} while (0)

/*
 * Simple queue definitions.
 */
#define SIMPLEQ_HEAD(name, type)                                \
struct name {                                                   \
        struct type *sqh_first; /* first element */             \
        struct type **sqh_last; /* addr of last next element */ \
}

#define SIMPLEQ_HEAD_INITIALIZER(head) \
        { NULL, &(head).sqh_first }

#define SIMPLEQ_ENTRY(type)                        \
struct {                                           \
        struct type *sqe_next;  /* next element */ \
}

/*
 * Simple queue access methods.
 */
#define SIMPLEQ_FIRST(head)         ((head)->sqh_first)
#define SIMPLEQ_END(head)           NULL
#define SIMPLEQ_EMPTY(head)         (SIMPLEQ_FIRST(head) == SIMPLEQ_END(head))
#define SIMPLEQ_NEXT(elm, field)    ((elm)->field.sqe_next)

#define SIMPLEQ_FOREACH(var, head, field) \
        for((var) = SIMPLEQ_FIRST(head);  \
            (var) != SIMPLEQ_END(head);   \
            (var) = SIMPLEQ_NEXT(var, field))

/*
 * Simple queue functions.
 */
#define SIMPLEQ_INIT(head) do {                \
        (head)->sqh_first = NULL;              \
        (head)->sqh_last = &(head)->sqh_first; \
} while (0)

#define SIMPLEQ_INSERT_HEAD(head, elm, field) do {               \
        if (((elm)->field.sqe_next = (head)->sqh_first) == NULL) \
                (head)->sqh_last = &(elm)->field.sqe_next;       \
        (head)->sqh_first = (elm);                               \
} while (0)

#define SIMPLEQ_INSERT_TAIL(head, elm, field) do { \
        (elm)->field.sqe_next = NULL;              \
        *(head)->sqh_last = (elm);                 \
        (head)->sqh_last = &(elm)->field.sqe_next; \
} while (0)

#define SIMPLEQ_INSERT_AFTER(head, listelm, elm, field) do {             \
        if (((elm)->field.sqe_next = (listelm)->field.sqe_next) == NULL) \
                (head)->sqh_last = &(elm)->field.sqe_next;               \
        (listelm)->field.sqe_next = (elm);                               \
} while (0)

#define SIMPLEQ_REMOVE_HEAD(head, field) do {                                \
        if (((head)->sqh_first = (head)->sqh_first->field.sqe_next) == NULL) \
                (head)->sqh_last = &(head)->sqh_first;                       \
} while (0)

/*
 * Tail queue definitions.
 */
#define TAILQ_HEAD(name, type)                                  \
struct name {                                                   \
        struct type *tqh_first; /* first element */             \
        struct type **tqh_last; /* addr of last next element */ \
}

#define TAILQ_HEAD_INITIALIZER(head) \
        { NULL, &(head).tqh_first }

#define TAILQ_ENTRY(type)                                              \
struct {                                                               \
        struct type *tqe_next;  /* next element */                     \
        struct type **tqe_prev; /* address of previous next element */ \
}

/*
 * tail queue access methods
 */
#define TAILQ_FIRST(head)               ((head)->tqh_first)
#define TAILQ_END(head)                 NULL
#define TAILQ_NEXT(elm, field)          ((elm)->field.tqe_next)
#define TAILQ_LAST(head, headname) \
        (*(((struct headname *)((head)->tqh_last))->tqh_last))
/* XXX */
#define TAILQ_PREV(elm, headname, field) \
        (*(((struct headname *)((elm)->field.tqe_prev))->tqh_last))
#define TAILQ_EMPTY(head) \
        (TAILQ_FIRST(head) == TAILQ_END(head))

#define TAILQ_FOREACH(var, head, field) \
        for((var) = TAILQ_FIRST(head);  \
            (var) != TAILQ_END(head);   \
            (var) = TAILQ_NEXT(var, field))

#define TAILQ_FOREACH_REVERSE(var, head, headname, field) \
        for((var) = TAILQ_LAST(head, headname);           \
            (var) != TAILQ_END(head);                     \
            (var) = TAILQ_PREV(var, headname, field))

/*
 * Tail queue functions.
 */
#define TAILQ_INIT(head) do {                  \
        (head)->tqh_first = NULL;              \
        (head)->tqh_last = &(head)->tqh_first; \
} while (0)

#define TAILQ_INSERT_HEAD(head, elm, field) do {                 \
        if (((elm)->field.tqe_next = (head)->tqh_first) != NULL) \
                (head)->tqh_first->field.tqe_prev =              \
                    &(elm)->field.tqe_next;                      \
        else                                                     \
                (head)->tqh_last = &(elm)->field.tqe_next;       \
        (head)->tqh_first = (elm);                               \
        (elm)->field.tqe_prev = &(head)->tqh_first;              \
} while (0)

#define TAILQ_INSERT_TAIL(head, elm, field) do {   \
        (elm)->field.tqe_next = NULL;              \
        (elm)->field.tqe_prev = (head)->tqh_last;  \
        *(head)->tqh_last = (elm);                 \
        (head)->tqh_last = &(elm)->field.tqe_next; \
} while (0)

#define TAILQ_INSERT_AFTER(head, listelm, elm, field) do {               \
        if (((elm)->field.tqe_next = (listelm)->field.tqe_next) != NULL) \
                (elm)->field.tqe_next->field.tqe_prev =                  \
                    &(elm)->field.tqe_next;                              \
        else                                                             \
                (head)->tqh_last = &(elm)->field.tqe_next;               \
        (listelm)->field.tqe_next = (elm);                               \
        (elm)->field.tqe_prev = &(listelm)->field.tqe_next;              \
} while (0)

#define TAILQ_INSERT_BEFORE(listelm, elm, field) do {       \
        (elm)->field.tqe_prev = (listelm)->field.tqe_prev;  \
        (elm)->field.tqe_next = (listelm);                  \
        *(listelm)->field.tqe_prev = (elm);                 \
        (listelm)->field.tqe_prev = &(elm)->field.tqe_next; \
} while (0)

#define TAILQ_REMOVE(head, elm, field) do {               \
        if (((elm)->field.tqe_next) != NULL)              \
                (elm)->field.tqe_next->field.tqe_prev =   \
                    (elm)->field.tqe_prev;                \
        else                                              \
                (head)->tqh_last = (elm)->field.tqe_prev; \
        *(elm)->field.tqe_prev = (elm)->field.tqe_next;   \
        _Q_INVALIDATE((elm)->field.tqe_prev);             \
        _Q_INVALIDATE((elm)->field.tqe_next);             \
} while (0)

#define TAILQ_REPLACE(head, elm, elm2, field) do {                    \
        if (((elm2)->field.tqe_next = (elm)->field.tqe_next) != NULL) \
                (elm2)->field.tqe_next->field.tqe_prev =              \
                    &(elm2)->field.tqe_next;                          \
        else                                                          \
                (head)->tqh_last = &(elm2)->field.tqe_next;           \
        (elm2)->field.tqe_prev = (elm)->field.tqe_prev;               \
        *(elm2)->field.tqe_prev = (elm2);                             \
        _Q_INVALIDATE((elm)->field.tqe_prev);                         \
        _Q_INVALIDATE((elm)->field.tqe_next);                         \
} while (0)

/*
 * Circular queue definitions.
 */
#define CIRCLEQ_HEAD(name, type)                            \
struct name {                                               \
        struct type *cqh_first;         /* first element */ \
        struct type *cqh_last;          /* last element */  \
}

#define CIRCLEQ_HEAD_INITIALIZER(head) \
        { CIRCLEQ_END(&head), CIRCLEQ_END(&head) }

#define CIRCLEQ_ENTRY(type)                                    \
struct {                                                       \
        struct type *cqe_next;          /* next element */     \
        struct type *cqe_prev;          /* previous element */ \
}

/*
 * Circular queue access methods
 */
#define CIRCLEQ_FIRST(head)             ((head)->cqh_first)
#define CIRCLEQ_LAST(head)              ((head)->cqh_last)
#define CIRCLEQ_END(head)               ((void *)(head))
#define CIRCLEQ_NEXT(elm, field)        ((elm)->field.cqe_next)
#define CIRCLEQ_PREV(elm, field)        ((elm)->field.cqe_prev)
#define CIRCLEQ_EMPTY(head) \
        (CIRCLEQ_FIRST(head) == CIRCLEQ_END(head))

#define CIRCLEQ_ENTRY_DETACHED(entry) \
    ((entry)->cqe_next == NULL && (entry)->cqe_prev == NULL)

#define CIRCLEQ_ENTRY_INIT(entry) \
    { (entry)->cqe_next = NULL; (entry)->cqe_prev = NULL; }

/*
 * Circular queue functions.
 */
#define CIRCLEQ_FOREACH(var, head, field) \
    for((var) = CIRCLEQ_FIRST(head);      \
        (var) != CIRCLEQ_END(head);       \
        (var) = CIRCLEQ_NEXT(var, field))

#define CIRCLEQ_FOREACH_REVERSE(var, head, field) \
    for((var) = CIRCLEQ_LAST(head);               \
        (var) != CIRCLEQ_END(head);               \
        (var) = CIRCLEQ_PREV(var, field))

#define CIRCLEQ_FOREACH_SAFE(var, head, field, tvar) \
    for ((var) = CIRCLEQ_FIRST(head);                \
         (var) != CIRCLEQ_END(head) &&               \
             ((tvar) = CIRCLEQ_NEXT(var, field), 1); \
         (var) = (tvar))

#define CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, field, tvar) \
    for ((var) = CIRCLEQ_LAST(head);                         \
         (var) != CIRCLEQ_END(head) &&                       \
             ((tvar) = CIRCLEQ_PREV(var, field), 1);         \
         (var) = (tvar))

#define CIRCLEQ_INIT(head) do {                \
        (head)->cqh_first = CIRCLEQ_END(head); \
        (head)->cqh_last = CIRCLEQ_END(head);  \
} while (0)

#define CIRCLEQ_INSERT_AFTER(head, listelm, elm, field) do {       \
        (elm)->field.cqe_next = (listelm)->field.cqe_next;         \
        (elm)->field.cqe_prev = (listelm);                         \
        if ((listelm)->field.cqe_next == CIRCLEQ_END(head))        \
                (head)->cqh_last = (elm);                          \
        else                                                       \
                (listelm)->field.cqe_next->field.cqe_prev = (elm); \
        (listelm)->field.cqe_next = (elm);                         \
} while (0)

#define CIRCLEQ_INSERT_BEFORE(head, listelm, elm, field) do {      \
        (elm)->field.cqe_next = (listelm);                         \
        (elm)->field.cqe_prev = (listelm)->field.cqe_prev;         \
        if ((listelm)->field.cqe_prev == CIRCLEQ_END(head))        \
                (head)->cqh_first = (elm);                         \
        else                                                       \
                (listelm)->field.cqe_prev->field.cqe_next = (elm); \
        (listelm)->field.cqe_prev = (elm);                         \
} while (0)

#define CIRCLEQ_INSERT_HEAD(head, elm, field) do {         \
        (elm)->field.cqe_next = (head)->cqh_first;         \
        (elm)->field.cqe_prev = CIRCLEQ_END(head);         \
        if ((head)->cqh_last == CIRCLEQ_END(head))         \
                (head)->cqh_last = (elm);                  \
        else                                               \
                (head)->cqh_first->field.cqe_prev = (elm); \
        (head)->cqh_first = (elm);                         \
} while (0)

#define CIRCLEQ_INSERT_TAIL(head, elm, field) do {        \
        (elm)->field.cqe_next = CIRCLEQ_END(head);        \
        (elm)->field.cqe_prev = (head)->cqh_last;         \
        if ((head)->cqh_first == CIRCLEQ_END(head))       \
                (head)->cqh_first = (elm);                \
        else                                              \
                (head)->cqh_last->field.cqe_next = (elm); \
        (head)->cqh_last = (elm);                         \
} while (0)

#define CIRCLEQ_REMOVE(head, elm, field) do {              \
        if ((elm)->field.cqe_next == CIRCLEQ_END(head))    \
                (head)->cqh_last = (elm)->field.cqe_prev;  \
        else                                               \
                (elm)->field.cqe_next->field.cqe_prev =    \
                    (elm)->field.cqe_prev;                 \
        if ((elm)->field.cqe_prev == CIRCLEQ_END(head))    \
                (head)->cqh_first = (elm)->field.cqe_next; \
        else                                               \
                (elm)->field.cqe_prev->field.cqe_next =    \
                    (elm)->field.cqe_next;                 \
        _Q_INVALIDATE((elm)->field.cqe_prev);              \
        _Q_INVALIDATE((elm)->field.cqe_next);              \
} while (0)

#define CIRCLEQ_REPLACE(head, elm, elm2, field) do {             \
        if (((elm2)->field.cqe_next = (elm)->field.cqe_next) ==  \
            CIRCLEQ_END(head))                                   \
                (head).cqh_last = (elm2);                        \
        else                                                     \
                (elm2)->field.cqe_next->field.cqe_prev = (elm2); \
        if (((elm2)->field.cqe_prev = (elm)->field.cqe_prev) ==  \
            CIRCLEQ_END(head))                                   \
                (head).cqh_first = (elm2);                       \
        else                                                     \
                (elm2)->field.cqe_prev->field.cqe_next = (elm2); \
        _Q_INVALIDATE((elm)->field.cqe_prev);                    \
        _Q_INVALIDATE((elm)->field.cqe_next);                    \
} while (0)

#define CIRCLEQ_SPLICE_TAIL(src_head, dst_head, field) do {               \
        if (CIRCLEQ_EMPTY((src_head)))                                    \
            break;  /* nothing to do */                                   \
                                                                          \
        (src_head)->cqh_first->field.cqe_prev = (dst_head)->cqh_last;     \
        (src_head)->cqh_last->field.cqe_next = (void *)(dst_head);        \
                                                                          \
        if (CIRCLEQ_EMPTY((dst_head)))                                    \
            (dst_head)->cqh_first = (src_head)->cqh_first;                \
        else                                                              \
            (dst_head)->cqh_last->field.cqe_next = (src_head)->cqh_first; \
                                                                          \
        (dst_head)->cqh_last  = (src_head)->cqh_last;                     \
                                                                          \
        CIRCLEQ_INIT((src_head));                                         \
    } while (0)

#define CIRCLEQ_ENTRY_IS_MEMBER(head, elm, field)     \
({                                                    \
    bool found = false;                               \
    typeof (elm) tmp;                                 \
    CIRCLEQ_FOREACH(tmp, head, field)                 \
    {                                                 \
        if ((elm) == tmp)                             \
        {                                             \
            found = true;                             \
            break;                                    \
        }                                             \
    }                                                 \
    found;                                            \
})

#endif  /* !_SYS_QUEUE_H_ */
