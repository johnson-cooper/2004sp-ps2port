#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "linklist.h"

static bool linklist_ensure_sentinel(LinkList *list) {
    if (!list) {
        return false;
    }
    if (list->sentinel) {
        return true;
    }

    list->sentinel = calloc(1, sizeof(Linkable));
    if (!list->sentinel) {
        return false;
    }
    list->sentinel->next = list->sentinel;
    list->sentinel->prev = list->sentinel;
    return true;
}

LinkList *linklist_new(void) {
    // Keep an empty list genuinely cheap. The client creates one ground-item list
    // for every tile on all four levels (43,264 lists), while almost every one is
    // empty. Allocate only the tiny list/cursor object here; materialise its
    // sentinel the first time a node is actually inserted.
    return calloc(1, sizeof(LinkList));
}

void linklist_free(LinkList *list) {
    if (!list) {
        return;
    }
    free(list->sentinel);
    free(list);
}

void linklist_add_tail(LinkList *list, Linkable *node) {
    if (!node || !linklist_ensure_sentinel(list)) {
        return;
    }
    if (node->prev) {
        linkable_unlink(node);
    }

    node->prev = list->sentinel->prev;
    node->next = list->sentinel;
    node->prev->next = node;
    node->next->prev = node;
}

void linklist_add_head(LinkList *list, Linkable *node) {
    if (!node || !linklist_ensure_sentinel(list)) {
        return;
    }
    if (node->prev) {
        linkable_unlink(node);
    }

    node->prev = list->sentinel;
    node->next = list->sentinel->next;
    node->prev->next = node;
    node->next->prev = node;
}

Linkable *linklist_remove_head(LinkList *list) {
    if (!list || !list->sentinel) {
        return NULL;
    }
    Linkable *node = list->sentinel->next;
    if (node == list->sentinel) {
        return NULL;
    }
    linkable_unlink(node);
    return node;
}

Linkable *linklist_head(LinkList *list) {
    if (!list || !list->sentinel) {
        if (list) {
            list->cursor = NULL;
        }
        return NULL;
    }
    Linkable *node = list->sentinel->next;
    if (node == list->sentinel) {
        list->cursor = NULL;
        return NULL;
    }
    list->cursor = node->next;
    return node;
}

Linkable *linklist_tail(LinkList *list) {
    if (!list || !list->sentinel) {
        if (list) {
            list->cursor = NULL;
        }
        return NULL;
    }
    Linkable *node = list->sentinel->prev;
    if (node == list->sentinel) {
        list->cursor = NULL;
        return NULL;
    }
    list->cursor = node->prev;
    return node;
}

Linkable *linklist_next(LinkList *list) {
    if (!list || !list->sentinel || !list->cursor) {
        return NULL;
    }
    Linkable *node = list->cursor;
    if (node == list->sentinel) {
        list->cursor = NULL;
        return NULL;
    }
    list->cursor = node->next;
    return node;
}

Linkable *linklist_prev(LinkList *list) {
    if (!list || !list->sentinel || !list->cursor) {
        return NULL;
    }
    Linkable *node = list->cursor;
    if (node == list->sentinel) {
        list->cursor = NULL;
        return NULL;
    }
    list->cursor = node->prev;
    return node;
}

void linklist_clear(LinkList *list) {
    if (!list || !list->sentinel) {
        return;
    }
    while (true) {
        Linkable *node = list->sentinel->next;
        if (node == list->sentinel) {
            return;
        }
        linkable_unlink(node);
        free(node);
    }
}
