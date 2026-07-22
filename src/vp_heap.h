#ifndef _VP_HEAP_H
#define _VP_HEAP_H
#include <stdlib.h>
#include <assert.h>

#define VP_HEAP_DEF_CAP 128

struct vp_heap {
    size_t length;
    size_t capacity;
    int (*cmp)(const void *a, const void *b);
    void **data;
};

int vp_heap_alloc(struct vp_heap *h, size_t capacity,
    int (*cmp)(const void *a, const void *b));
void vp_heap_free(struct vp_heap *h);
void vp_heap_push(struct vp_heap *h, void *item);
void *vp_heap_pop(struct vp_heap *h);
void *vp_heap_peek(const struct vp_heap *h);

#endif
#ifdef VP_HEAP_IMPLEMENTATION
#include <string.h>

int vp_heap_alloc(struct vp_heap *h, size_t capacity,
    int (*cmp)(const void *a, const void *b))
{
    void **data;
    assert(h != NULL && "The given heap is NULL");
	if (capacity == 0) {
		capacity = VP_HEAP_DEF_CAP;
	}
	data = malloc(capacity * sizeof(void *));
    if (data == NULL) {
		return -1;
	}
    h->capacity = capacity;
	h->length = 0;
	h->data = data;
    h->cmp = cmp;

	return 0;
}

void vp_heap_free(struct vp_heap *h)
{
    if (h->data != NULL) {
        free(h->data);
    }
    memset(h, 0, sizeof(struct vp_heap));
}

static void vp_heap_swap(struct vp_heap *h, long long a, long long b)
{
    void *aux = h->data[a];
    h->data[a] = h->data[b];
    h->data[b] = aux;
}

void vp_heap_push(struct vp_heap *h, void *item)
{
    size_t new_cap;
	assert(h != NULL && "The given heap is NULL");

	if (!h->capacity ||
		h->length >= h->capacity) {
		new_cap = h->capacity * 2;
		if (new_cap == 0) {
			new_cap = VP_HEAP_DEF_CAP;
		}
		void **tmp = realloc(h->data, new_cap * sizeof(void *));
		if (!tmp) {
			perror("vp_heap_append");
			exit(1);
		} else {
			h->data = tmp;
		}
		h->capacity = new_cap;
	}
    h->data[h->length] = (void *) item;
    long long i = (long long) h->length;
    h->length += 1;
    while (i > 0 && h->cmp(h->data[(i-1)/2], h->data[i]) > 0) {
        vp_heap_swap(h, i, (i-1)/2);
        i = (i-1)/2;
    }
}

void *vp_heap_pop(struct vp_heap *h)
{
    void *item, *last;

	assert(h != NULL && "The given heap is NULL");
	if (h->length == 0) {
		return NULL;
	}
	h->length -= 1;
	item = h->data[0];
    last = h->data[h->length];
    if (h->length > 0) {
        h->data[0] = last;

        for (size_t i = 0;;) {
            size_t left  = 2 * i + 1;
            size_t right = 2 * i + 2;
            size_t min   = i;

            if (left  < h->length && h->cmp(h->data[left] , h->data[min]) < 0) min = left;
            if (right < h->length && h->cmp(h->data[right], h->data[min]) < 0) min = right;
            if (min == i)
                break;

            vp_heap_swap(h, i, min);
            i = min;
      }
    }

	return item;
}

void *vp_heap_peek(const struct vp_heap *h)
{
    if (h->length == 0) {
        return NULL;
    }
    return h->data[0];
}

#endif