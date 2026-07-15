#ifndef _VP_LIST_H
#define _VP_LIST_H
#include <stdlib.h>

#define VP_LIST_DEF_CAP 128
//#define vp_list_for (elem, list) for ()
//#define vp_list_node_for (elem, list)

#define VP_LIST_NODE_FREE 0
#define VP_LIST_NODE_USED 1

struct vp_list_node {
    char status;
    struct vp_list_node *next;
    struct vp_list_node *prev;
    const void *item;
};

struct vp_list {
    size_t length;
    size_t capacity;
    struct vp_list_node *data;
    struct vp_list_node *next_free;
    struct vp_list_node *first;
    struct vp_list_node *last;
};

int vp_list_alloc(struct vp_list *list, size_t capacity);
void vp_list_free(struct vp_list *list);
int vp_list_append(struct vp_list *list, const void *item);
void *vp_list_pop(struct vp_list *list);
int vp_list_insert(struct vp_list *list,
    const void *item,
    size_t index);
struct vp_list_node *
vp_list_insert_before(struct vp_list *list,
    const void *item,
    struct vp_list_node *elem);
struct vp_list_node *
vp_list_insert_after(struct vp_list *list,
    const void *item,
    struct vp_list_node *elem);
void *vp_list_remove(struct vp_list *list, size_t index);
void *vp_list_remove_at(struct vp_list *list, struct vp_list_node *elem);

#endif /* _VP_LIST_H */

#ifdef VP_LIST_IMPLEMENTATION
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>

static struct vp_list_node *
vp_list_get_node(struct vp_list *list, size_t index)
{
    struct vp_list_node *node;
    size_t i;

    if (index >= list->length) {
        return NULL;
    }

    if (index < list->length / 2) {
        node = list->first;
        for (i = 0; i < index; ++i) {
            node = node->next;
        }
    } else {
        node = list->last;
        for (i = list->length-1; i > index; --i) {
            node = node->prev;
        }
    }

    return node;
}

static int
vp_list_realloc(struct vp_list *list)
{
    struct vp_list_node *old_data;
    struct vp_list_node *new_data;
    ptrdiff_t delta;

    size_t old_cap;
    size_t new_cap;
    size_t i;

    if (!list) {
        return -1;
    }

    old_cap = list->capacity;
    new_cap = old_cap ? old_cap * 2 : VP_LIST_DEF_CAP;
    old_data = list->data;

    new_data = realloc(list->data, new_cap * sizeof(struct vp_list_node));
    if (!new_data) {
        return -1;
    }

    list->data = new_data;
    /*
     * If realloc moved the block,
     * every internal pointer must be fixed.
     */

    if (new_data != old_data) {
        delta = (char *) new_data - (char *) old_data;
        for (i = 0; i < old_cap; ++i) {
            if (new_data[i].next) {
                new_data[i].next = (struct vp_list_node *) ((char *)new_data[i].next + delta);
            }
            if (new_data[i].prev) {
                new_data[i].prev = (struct vp_list_node *) ((char *)new_data[i].prev + delta);
            }
        }
        if (list->first) {
            list->first = (struct vp_list_node *) ((char *)list->first + delta);
        }
        if (list->last) {
            list->last = (struct vp_list_node *) ((char *)list->last + delta);
        }
        if (list->next_free) {
            list->next_free = (struct vp_list_node *) ((char *)list->next_free + delta);
        }
    }

    /*
     * Initialize newly-added nodes.
     */

    for (i = old_cap; i < new_cap - 1; ++i) {
        new_data[i].status = VP_LIST_NODE_FREE;
        new_data[i].item = NULL;
        new_data[i].prev = NULL;
        new_data[i].next = &new_data[i + 1];
    }
    new_data[new_cap - 1].status = VP_LIST_NODE_FREE;
    new_data[new_cap - 1].item = NULL;
    new_data[new_cap - 1].prev = NULL;
    new_data[new_cap - 1].next = list->next_free;

    list->next_free = &new_data[old_cap];
    list->capacity = new_cap;
    return 0;
}

int
vp_list_alloc(struct vp_list *list, size_t capacity)
{
    size_t i;
    if (!list) {
        return -1;
    }
    memset(list, 0, sizeof(struct vp_list));
    if (capacity == 0) {
        capacity = VP_LIST_DEF_CAP;
    }
    list->data = (struct vp_list_node *) calloc(capacity, sizeof(struct vp_list_node));
    if (!list->data) {
        return -1;
    }
    list->capacity = capacity;

    for (i = 0; i < capacity-1; ++i) {
        list->data[i].status = VP_LIST_NODE_FREE;
        list->data[i].next = &list->data[i+1];
    }
    list->data[capacity-1].status = VP_LIST_NODE_FREE;
    list->data[capacity-1].next = NULL;
    list->next_free = &list->data[0];
    return 0;
}

void
vp_list_free(struct vp_list *list)
{
    if (!list) {
        return;
    }
    free(list->data);
    memset(list, 0, sizeof(*list));
}

int
vp_list_append(struct vp_list *list, const void *item)
{
    struct vp_list_node *node;

    if (!list) {
        return -1;
    }
    if (!list->next_free) {
        if (vp_list_realloc(list) != 0) {
            return -1;
        }
    }

    node = list->next_free;
    list->next_free = node->next;

    node->status = VP_LIST_NODE_USED;
    node->item = item;
    node->next = NULL;
    node->prev = list->last;

    if (list->last) {
        list->last->next = node;
    } else {
        list->first = node;
    }
    list->last = node;
    list->length += 1;
    return 0;
}

void *
vp_list_pop(struct vp_list *list)
{
    struct vp_list_node *node;
    void *item;

    if (!list || list->length == 0) {
        return NULL;
    }

    node = list->last;
    item = (void *) node->item;

    list->last = node->prev;
    if (list->last) {
        list->last->next = NULL;
    } else {
        list->first = NULL;
    }

    node->status = VP_LIST_NODE_FREE;
    node->item = NULL;
    node->prev = NULL;
    node->next = list->next_free;

    list->next_free = node;
    list->length -= 1;

    return item;
}

int
vp_list_insert(struct vp_list *list, const void *item, size_t index)
{
    struct vp_list_node *new_node;
    struct vp_list_node *curr;

    if (!list)
        return -1;

    if (!list->next_free) {
        if (vp_list_realloc(list) != 0) {
            return -1;
        }
    }

    if (index > list->length)
        return -1;

    if (index == list->length)
        return vp_list_append(list, item);

    curr = vp_list_get_node(list, index);
    if (!curr)
        return -1;

    new_node = list->next_free;
    list->next_free = new_node->next;

    new_node->status = VP_LIST_NODE_USED;
    new_node->item = item;

    new_node->prev = curr->prev;
    new_node->next = curr;

    if (curr->prev) {
        curr->prev->next = new_node;
    } else {
        list->first = new_node;
    }
    curr->prev = new_node;
    list->length++;
    return 0;
}

struct vp_list_node *vp_list_insert_before(struct vp_list *list,
    const void *item,
    struct vp_list_node *elem)
{
    struct vp_list_node *new_node;
    
    if (!list) {
        return NULL;
    }
    if (!list->next_free) {
        size_t idx = elem - list->data;
        if (vp_list_realloc(list) != 0) {
            return NULL;
        }
        elem = &list->data[idx];
    }
    new_node = list->next_free;
    list->next_free = new_node->next;

    new_node->status = VP_LIST_NODE_USED;
    new_node->item = item;
    new_node->prev = elem->prev;
    new_node->next = elem;

    if (elem->prev) {
        elem->prev->next = new_node;
    } else {
        list->first = new_node;
    }
    elem->prev = new_node;
    list->length += 1;
    return elem;
}

struct vp_list_node *vp_list_insert_after(struct vp_list *list,
    const void *item,
    struct vp_list_node *elem)
{
    struct vp_list_node *new_node;
    
    if (!list || !elem) {
        return NULL;
    }
    if (!list->next_free) {
        return NULL;
    }
    if (!list->next_free) {
        size_t idx = elem - list->data;
        if (vp_list_realloc(list) != 0) {
            return NULL;
        }
        elem = &list->data[idx];
    }
    new_node = list->next_free;
    list->next_free = new_node->next;

    new_node->status = VP_LIST_NODE_USED;
    new_node->item = item;
    
    new_node->prev = elem;
    new_node->next = elem->next;

    if (elem->next) {
        elem->next->prev = new_node;
    } else {
        list->last = new_node;
    }
    elem->next = new_node;
    list->length += 1;
    return elem;
}

void *vp_list_remove(struct vp_list *list, size_t index)
{
    struct vp_list_node *node;
    void *item;

    if (!list) {
        return NULL;
    }

    node = vp_list_get_node(list, index);
    if (!node) {
        return NULL;
    }

    item = (void *) node->item;
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        list->first = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        list->last = node->prev;
    }
    node->status = VP_LIST_NODE_FREE;
    node->item = NULL;
    node->prev = NULL;

    node->next = list->next_free;
    list->next_free = node;
    list->length--;
    return item;
}

void *vp_list_remove_at(struct vp_list *list, struct vp_list_node *elem)
{
    void *item;

    if (!list || !elem) {
        return NULL;
    }
    if (elem->status != VP_LIST_NODE_USED) {
        return NULL;
    }
    assert(elem->prev == NULL || elem->prev->next == elem);
    assert(elem->next == NULL || elem->next->prev == elem);
    item = (void *) elem->item;
    if (elem->prev) {
        elem->prev->next = elem->next;
    } else {
        list->first = elem->next;
    }
    if (elem->next) {
       elem->next->prev = elem->prev;
    } else {
        list->last = elem->prev;
    }
    elem->status = VP_LIST_NODE_FREE;
    elem->item = NULL;
    elem->prev = NULL;

    elem->next = list->next_free;
    list->next_free = elem;
    list->length -= 1;
    return item;
}

#endif /* VP_LIST_H_IMPLEMENTATION */
