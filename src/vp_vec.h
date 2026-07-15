#ifndef _VP_VEC_H
#define _VP_VEC_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#define VP_VEC_DEF_CAP 128
#define vp_for(elem, vec) for (size_t __i = 0; __i < (vec)->length && (elem = vp_vec_get((vec), __i)); ++__i)

struct vp_vec {
	size_t length;
	size_t capacity;
	void **data;
};

int vp_vec_alloc(struct vp_vec *vec, size_t capacity);
void vp_vec_free(struct vp_vec *vec);
int vp_vec_append(struct vp_vec *vec, const void *item);
void *vp_vec_pop(struct vp_vec *vec);
int vp_vec_insert(struct vp_vec *vec,
		  const void *item,
		  size_t index);
void *vp_vec_remove(struct vp_vec *vec, size_t index);
void *vp_vec_get(const struct vp_vec *vec, size_t index);
void vp_vec_set(struct vp_vec *vec,
		size_t index,
		const void *item);
void vp_vec_sort(struct vp_vec *vec,
		 int (*compar) (const void *, const void *));
void vp_vec_sort_r(struct vp_vec *vec,
		   int (*compar)(const void *, const void *, void *),
		   void *arg);
int vp_vec_exists(const struct vp_vec *vec, const void *elem);
int vp_vec_index(const struct vp_vec *vec, const void *elem);

#endif /* _VP_VEC_H */

#ifdef VP_VEC_IMPLEMENTATION
#include <string.h>

/* void data_realloc(void **data, size_t size) */
/* { */
/* } */

int vp_vec_alloc(struct vp_vec *vec, size_t capacity)
{
	void **data;

	assert(vec != NULL && "The given vector is NULL");
	if (capacity == 0) {
		capacity = VP_VEC_DEF_CAP;
	}
	data = malloc(capacity * sizeof(void *));

	if (data == NULL) {
		return -1;
	}

	vec->capacity = capacity;
	vec->length = 0;
	vec->data = data;

	return 0;
}

void vp_vec_free(struct vp_vec *vec)
{
	if (vec->data != NULL) {
		free(vec->data);
	}
	memset(vec, 0, sizeof(struct vp_vec));
}

int vp_vec_append(struct vp_vec *vec, const void *item)
{
	size_t new_cap;
	assert(vec != NULL && "The given vector is NULL");

	if (!vec->capacity ||
		vec->length >= vec->capacity) {
		new_cap = vec->capacity * 2;
		if (new_cap == 0) {
			new_cap = VP_VEC_DEF_CAP;
		}
		//vec->data = realloc(vec->data, new_cap * sizeof(void *));
		void **tmp = realloc(vec->data, new_cap * sizeof(void *));
		if (!tmp) {
			perror("vp_vec_append");
			exit(1);
		} else {
			vec->data = tmp;
		}
		//if (errno == ENOMEM) {
		//	perror("vp_vec_append");
		//	exit(1);
		//}
		vec->capacity = new_cap;
	}

	vec->data[vec->length] = (void *) item;
	vec->length += 1;

	return 0;
}

void *vp_vec_pop(struct vp_vec *vec)
{
	void *item;

	assert(vec != NULL && "The given vector is NULL");
	if (vec->length == 0) {
		return NULL;
	}
	vec->length -= 1;
	item = vec->data[vec->length];

	return item;
}

int vp_vec_insert(struct vp_vec *vec,
		   const void *item,
		   size_t index)
{
	size_t i, new_cap;

	assert(vec != NULL && "The given vector is NULL");
	if (!vec->capacity || vec->length >= vec->capacity) {
		new_cap = vec->capacity * 2;
		if (new_cap == 0) {
			new_cap = VP_VEC_DEF_CAP;
		}
		//vec->data = realloc(vec->data, new_cap * sizeof(void *));
		void **tmp = realloc(vec->data, new_cap * sizeof(void *));
		if (!tmp) {
		//if (errno == ENOMEM) {
			perror("vp_vec_insert");
			exit(1);
		}
		vec->data = tmp;
		vec->capacity = new_cap;
	}
	if (index > vec->length) {
		return -2;
	}
	for (i = vec->length; i > index; i--) {
		vec->data[i] = vec->data[i - 1];
	}
	vec->data[index] = (void *) item;
	vec->length += 1;

	return 0;
}

void *vp_vec_remove(struct vp_vec *vec, size_t index)
{
	void *item;
	size_t i;

	assert(vec != NULL && "The given vector is NULL");
	if (vec->length == 0) {
		return NULL;
	}
	if (index > vec->length - 1) {
		return NULL;
	}

	item = vec->data[index];
	vec->length -= 1;
	for (i = index; i < vec->length; i++) {
		vec->data[i] = vec->data[i+1];
	}
	return item;
}

void *vp_vec_get(const struct vp_vec *vec, size_t index)
{
	assert(vec != NULL && "The given vector is NULL");
	if (index >= vec->length) {
		return NULL;
	}
	return vec->data[index];
}

void vp_vec_set(struct vp_vec *vec,
		 size_t index,
		 const void *item)
{
	assert(vec != NULL && "The given vector is NULL");
	if (index > vec->length - 1) {
		return;
	}
	vec->data[index] = (void *) item;
}

struct compar_aux {
	int (*compar) (const void *, const void *);
};

struct compar_aux_r {
	int (*compar) (const void *, const void *, void *);
	void *arg;
};

int cons_compar(const void *a,
		const void *b,
		void *cmp)
{
	return ((struct compar_aux *) cmp)->compar(* (const void **)a, *(const void **) b);
}

static inline int
cons_compar_r(const void *a,
	      const void *b,
	      void *cmp)
{
	return ((struct compar_aux_r *) cmp)->compar(*(const void **)a,
						     *(const void **)b,
						     ((struct compar_aux_r *) cmp)->arg);
}

int vp_vec_exists(const struct vp_vec *vec, const void *elem)
{
	for (size_t i = 0; i < vec->length; i++) {
		if (vec->data[i] == elem) {
			return 1;
		}
	}
	return 0;
}

int vp_vec_index(const struct vp_vec *vec, const void *elem)
{
	for (size_t i = 0; i < vec->length; i++) {
		if (vec->data[i] == elem) {
			return i;
		}
	}
	return -1;
}

/*
void vp_vec_sort(struct vp_vec *vec,
		 int (*compar) (const void *, const void *))
{
	struct compar_aux cmp;

	cmp.compar = compar;
	qsort_r(vec->data, vec->length, sizeof(void *), cons_compar, &cmp);
}

void vp_vec_sort_r(struct vp_vec *vec,
		   int (*compar)(const void *, const void *, void *),
		   void *arg)
{
	struct compar_aux_r cmp;

	cmp.compar = compar;
	cmp.arg = arg;
	qsort_r(vec->data, vec->length, sizeof(void *), cons_compar_r, &cmp);
}
*/

#endif /* VP_VEC_IMPLEMENTATION */
