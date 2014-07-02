/* Copyright 2013-2014 IBM Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#define BITS_PER_LONG (sizeof(long) * 8)
/* Don't include this, it's PPC-specific */
#define __CPU_H
static unsigned int cpu_max_pir = 1;
struct cpu_thread {
	unsigned int			chip_id;
};

#include <stdlib.h>
#include <string.h>

/* Use these before we override definitions below. */
static void *__malloc(size_t size, const char *location __attribute__((unused)))
{
	return malloc(size);
}

static void *__realloc(void *ptr, size_t size, const char *location __attribute__((unused)))
{
	return realloc(ptr, size);
}

static inline void __free(void *p, const char *location __attribute__((unused)))
{
	return free(p);
}

static void *__zalloc(size_t size, const char *location __attribute__((unused)))
{
	void *ptr = malloc(size);
	memset(ptr, 0, size);
	return ptr;
}

#include <skiboot.h>

#define is_rodata(p) true

#include "../mem_region.c"
#include "../device.c"

#include <assert.h>
#include <stdio.h>

struct dt_node *dt_root;

void lock(struct lock *l)
{
	l->lock_val++;
}

void unlock(struct lock *l)
{
	l->lock_val--;
}

#define TEST_HEAP_ORDER 12
#define TEST_HEAP_SIZE (1ULL << TEST_HEAP_ORDER)

static bool heap_empty(void)
{
	const struct alloc_hdr *h = region_start(&skiboot_heap);
	return h->num_longs == skiboot_heap.len / sizeof(long);
}

int main(void)
{
	char *test_heap;
	void *p, *ptrs[100];
	size_t i;
	struct mem_region *r;

	/* Use malloc for the heap, so valgrind can find issues. */
	test_heap = __malloc(TEST_HEAP_SIZE, __location__);
	skiboot_heap.start = (unsigned long)test_heap;
	skiboot_heap.len = TEST_HEAP_SIZE;

	/* Allocations of various sizes. */
	for (i = 0; i < TEST_HEAP_ORDER; i++) {
		p = mem_alloc(&skiboot_heap, 1ULL << i, 1, "here");
		assert(p);
		assert(mem_check(&skiboot_heap));
		assert(!strcmp(((struct alloc_hdr *)p)[-1].location, "here"));
		assert(p > (void *)test_heap);
		assert(p + (1ULL << i) <= (void *)test_heap + TEST_HEAP_SIZE);
		assert(mem_size(&skiboot_heap, p) >= 1ULL << i);
		mem_free(&skiboot_heap, p, "freed");
		assert(heap_empty());
		assert(mem_check(&skiboot_heap));
		assert(!strcmp(((struct alloc_hdr *)p)[-1].location, "freed"));
	}
	p = mem_alloc(&skiboot_heap, 1ULL << i, 1, "here");
	assert(!p);
	mem_free(&skiboot_heap, p, "freed");
	assert(heap_empty());
	assert(mem_check(&skiboot_heap));

	/* Allocations of various alignments: use small alloc first. */
	ptrs[0] = mem_alloc(&skiboot_heap, 1, 1, "small");
	for (i = 0; ; i++) {
		p = mem_alloc(&skiboot_heap, 1, 1ULL << i, "here");
		assert(mem_check(&skiboot_heap));
		/* We will eventually fail... */
		if (!p) {
			assert(i >= TEST_HEAP_ORDER);
			break;
		}
		assert(p);
		assert((long)p % (1ULL << i) == 0);
		assert(p > (void *)test_heap);
		assert(p + 1 <= (void *)test_heap + TEST_HEAP_SIZE);
		mem_free(&skiboot_heap, p, "freed");
		assert(mem_check(&skiboot_heap));
	}
	mem_free(&skiboot_heap, ptrs[0], "small freed");
	assert(heap_empty());
	assert(mem_check(&skiboot_heap));

	/* Many little allocations, freed in reverse order. */
	for (i = 0; i < 100; i++) {
		ptrs[i] = mem_alloc(&skiboot_heap, sizeof(long), 1, "here");
		assert(ptrs[i]);
		assert(ptrs[i] > (void *)test_heap);
		assert(ptrs[i] + sizeof(long)
		       <= (void *)test_heap + TEST_HEAP_SIZE);
		assert(mem_check(&skiboot_heap));
	}
	for (i = 0; i < 100; i++)
		mem_free(&skiboot_heap, ptrs[100 - 1 - i], "freed");

	assert(heap_empty());
	assert(mem_check(&skiboot_heap));

	/* Check the prev_free gets updated properly. */
	ptrs[0] = mem_alloc(&skiboot_heap, sizeof(long), 1, "ptrs[0]");
	ptrs[1] = mem_alloc(&skiboot_heap, sizeof(long), 1, "ptrs[1]");
	assert(ptrs[1] > ptrs[0]);
	mem_free(&skiboot_heap, ptrs[0], "ptrs[0] free");
	assert(mem_check(&skiboot_heap));
	ptrs[0] = mem_alloc(&skiboot_heap, sizeof(long), 1, "ptrs[0] again");
	assert(mem_check(&skiboot_heap));
	mem_free(&skiboot_heap, ptrs[1], "ptrs[1] free");
	mem_free(&skiboot_heap, ptrs[0], "ptrs[0] free");
	assert(mem_check(&skiboot_heap));
	assert(heap_empty());

#if 0
	printf("Heap map:\n");
	for (i = 0; i < TEST_HEAP_SIZE / sizeof(long); i++) {
		printf("%u", test_bit(skiboot_heap.bitmap, i));
		if (i % 64 == 63)
			printf("\n");
		else if (i % 8 == 7)
			printf(" ");
	}
#endif

	/* Simple enlargement, then free */
	p = mem_alloc(&skiboot_heap, 1, 1, "one byte");
	assert(p);
	assert(mem_resize(&skiboot_heap, p, 100, "hundred bytes"));
	assert(mem_size(&skiboot_heap, p) >= 100);
	assert(mem_check(&skiboot_heap));
	assert(!strcmp(((struct alloc_hdr *)p)[-1].location, "hundred bytes"));
	mem_free(&skiboot_heap, p, "freed");

	/* Simple shrink, then free */
	p = mem_alloc(&skiboot_heap, 100, 1, "100 bytes");
	assert(p);
	assert(mem_resize(&skiboot_heap, p, 1, "1 byte"));
	assert(mem_size(&skiboot_heap, p) < 100);
	assert(mem_check(&skiboot_heap));
	assert(!strcmp(((struct alloc_hdr *)p)[-1].location, "1 byte"));
	mem_free(&skiboot_heap, p, "freed");

	/* Lots of resizing (enlarge). */
	p = mem_alloc(&skiboot_heap, 1, 1, "one byte");
	assert(p);
	for (i = 1; i <= TEST_HEAP_SIZE - sizeof(struct alloc_hdr); i++) {
		assert(mem_resize(&skiboot_heap, p, i, "enlarge"));
		assert(mem_size(&skiboot_heap, p) >= i);
		assert(mem_check(&skiboot_heap));
	}

	/* Can't make it larger though. */
	assert(!mem_resize(&skiboot_heap, p, i, "enlarge"));

	for (i = TEST_HEAP_SIZE - sizeof(struct alloc_hdr); i > 0; i--) {
		assert(mem_resize(&skiboot_heap, p, i, "shrink"));
		assert(mem_check(&skiboot_heap));
	}

	mem_free(&skiboot_heap, p, "freed");
	assert(mem_check(&skiboot_heap));

	/* Test splitting of a region. */
	r = new_region("base", (unsigned long)test_heap,
		       TEST_HEAP_SIZE, NULL, REGION_SKIBOOT_HEAP);
	assert(add_region(r));
	r = new_region("splitter", (unsigned long)test_heap + TEST_HEAP_SIZE/4,
		       TEST_HEAP_SIZE/2, NULL, REGION_RESERVED);
	assert(add_region(r));
	/* Now we should have *three* regions. */
	i = 0;
	list_for_each(&regions, r, list) {
		if (region_start(r) == test_heap) {
			assert(r->len == TEST_HEAP_SIZE/4);
			assert(strcmp(r->name, "base") == 0);
			assert(r->type == REGION_SKIBOOT_HEAP);
		} else if (region_start(r) == test_heap + TEST_HEAP_SIZE / 4) {
			assert(r->len == TEST_HEAP_SIZE/2);
			assert(strcmp(r->name, "splitter") == 0);
			assert(r->type == REGION_RESERVED);
			assert(!r->free_list.n.next);
		} else if (region_start(r) == test_heap + TEST_HEAP_SIZE/4*3) {
			assert(r->len == TEST_HEAP_SIZE/4);
			assert(strcmp(r->name, "base") == 0);
			assert(r->type == REGION_SKIBOOT_HEAP);
		} else
			abort();
		assert(mem_check(r));
		i++;
	}
	assert(i == 3);
	while ((r = list_pop(&regions, struct mem_region, list)) != NULL) {
		list_del(&r->list);
		mem_free(&skiboot_heap, r, __location__);
	}
	assert(mem_region_lock.lock_val == 0);
	__free(test_heap, "");
	return 0;
}
