/*
 * The "slice" memory allocator.  It is an allocator for large numbers
 * of small fixed-size objects.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "slice.hxx"
#include "mmap.h"

#include <inline/list.h>

#include <new>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static constexpr unsigned ALLOCATED = -1;
static constexpr unsigned END_OF_LIST = -2;

#ifndef NDEBUG
static constexpr unsigned MARK = -3;
#endif

struct slice_slot {
    unsigned next;

    constexpr bool IsAllocated() const {
        return next == ALLOCATED;
    }
};

struct slice_area {
    struct list_head siblings;

    unsigned allocated_count;

    unsigned free_head;

    struct slice_slot slices[1];

private:
    slice_area(struct slice_pool &pool);

    ~slice_area() {
        assert(allocated_count == 0);
    }

public:
    static struct slice_area *New(struct slice_pool &pool);
    void Delete(struct slice_pool &pool);

    bool IsEmpty() const {
        return allocated_count == 0;
    }

    bool IsFull(const struct slice_pool &pool) const;

    gcc_pure
    void *GetPage(const struct slice_pool &pool, unsigned page);

    gcc_pure
    void *GetSlice(const struct slice_pool &pool, unsigned slice);

    /**
     * Calculates the allocation slot index from an allocated pointer.
     * This is used to locate the #slice_slot for a pointer passed to a
     * public function.
     */
    gcc_pure
    unsigned IndexOf(const struct slice_pool &pool, const void *_p);

    /**
     * Find the first free slot index, starting at the specified position.
     */
    gcc_pure
    unsigned FindFree(const struct slice_pool &pool, unsigned start) const;

    /**
     * Find the first allocated slot index, starting at the specified
     * position.
     */
    gcc_pure
    unsigned FindAllocated(const struct slice_pool &pool,
                           unsigned start) const;

    /**
     * Punch a hole in the memory map in the specified slot index range.
     * This means notifying the kernel that we will no longer need the
     * contents, which allows the kernel to drop the allocated pages and
     * reuse it for other processes.
     */
    void PunchSliceRange(struct slice_pool &pool,
                         unsigned start, gcc_unused unsigned end);

    void Compress(struct slice_pool &pool);

    void *Alloc(struct slice_pool &pool);
    void Free(struct slice_pool &pool, void *p);
};

constexpr
static inline size_t
align_size(size_t size)
{
    return ((size - 1) | 0x1f) + 1;
}

gcc_const
static inline size_t
align_page_size(size_t size)
{
    return ((size - 1) | (mmap_page_size() - 1)) + 1;
}

static constexpr unsigned
divide_round_up(unsigned a, unsigned b)
{
    return (a + b - 1) / b;
}

struct slice_pool {
    size_t slice_size;

    /**
     * Number of slices that fit on one MMU page (4 kB).
     */
    unsigned slices_per_page;

    unsigned pages_per_slice;

    unsigned pages_per_area;

    unsigned slices_per_area;

    /**
     * Number of pages for the area header.
     */
    unsigned header_pages;

    size_t area_size;

    struct list_head areas;

    slice_pool(size_t _slice_size, unsigned _slices_per_area);
    ~slice_pool();

    void Compress();

    gcc_pure
    struct slice_area *FindNonFullArea();

    struct slice_area *GetArea();
};

/*
 * slice_area methods
 *
 */

slice_area::slice_area(struct slice_pool &pool)
    :allocated_count(0), free_head(0)
{
    /* build the "free" list */
    for (unsigned i = 0; i < pool.slices_per_area - 1; ++i)
        slices[i].next = i + 1;

    slices[pool.slices_per_area - 1].next = END_OF_LIST;
}

struct slice_area *
slice_area::New(struct slice_pool &pool)
{
    void *p = mmap_alloc_anonymous(pool.area_size);
    if (p == (void *)-1) {
        fputs("Out of adress space\n", stderr);
        abort();
    }

    return ::new(p) slice_area(pool);
}

inline bool
slice_area::IsFull(gcc_unused const struct slice_pool &pool) const
{
    assert(free_head < pool.slices_per_area ||
           free_head == END_OF_LIST);

    return free_head == END_OF_LIST;
}

void
slice_area::Delete(struct slice_pool &pool)
{
    assert(allocated_count == 0);

#ifndef NDEBUG
    for (unsigned i = 0; i < pool.slices_per_area; ++i)
        assert(slices[i].next < pool.slices_per_area ||
               slices[i].next == END_OF_LIST);

    unsigned i = free_head;
    while (i != END_OF_LIST) {
        assert(i < pool.slices_per_area);

        unsigned next = slices[i].next;
        slices[i].next = MARK;
        i = next;
    }
#endif

    this->~slice_area();
    mmap_free(this, pool.area_size);
}

inline void *
slice_area::GetPage(const struct slice_pool &pool, unsigned page)
{
    assert(page <= pool.pages_per_area);

    return (uint8_t *)this + (pool.header_pages + page) * mmap_page_size();
}

inline void *
slice_area::GetSlice(const struct slice_pool &pool, unsigned slice)
{
    assert(slice < pool.slices_per_area);
    assert(slices[slice].IsAllocated());

    unsigned page = (slice / pool.slices_per_page) * pool.pages_per_slice;
    slice %= pool.slices_per_page;

    return (uint8_t *)GetPage(pool, page) + slice * pool.slice_size;
}

inline unsigned
slice_area::IndexOf(const struct slice_pool &pool, const void *_p)
{
    const uint8_t *p = (const uint8_t *)_p;
    assert(p >= (uint8_t *)GetPage(pool, 0));
    assert(p < (uint8_t *)GetPage(pool, pool.pages_per_area));

    size_t offset = p - (const uint8_t *)this;
    const unsigned page = offset / mmap_page_size() - pool.header_pages;
    offset %= mmap_page_size();
    assert(offset % pool.slice_size == 0);

    return page * pool.slices_per_page / pool.pages_per_slice
        + offset / pool.slice_size;
}

unsigned
slice_area::FindFree(const struct slice_pool &pool, unsigned start) const
{
    assert(start <= pool.slices_per_page);

    const unsigned end = pool.slices_per_page;

    unsigned i;
    for (i = start; i != end; ++i)
        if (!slices[i].IsAllocated())
            break;

    return i;
}

/**
 * Find the first allocated slot index, starting at the specified
 * position.
 */
gcc_pure
unsigned
slice_area::FindAllocated(const struct slice_pool &pool, unsigned start) const
{
    assert(start <= pool.slices_per_page);

    const unsigned end = pool.slices_per_page;

    unsigned i;
    for (i = start; i != end; ++i)
        if (slices[i].IsAllocated())
            break;

    return i;
}

void
slice_area::PunchSliceRange(struct slice_pool &pool,
                            unsigned start, gcc_unused unsigned end)
{
    assert(start <= end);

    unsigned start_page = divide_round_up(start, pool.slices_per_page)
        * pool.pages_per_slice;
    unsigned end_page = (start / pool.slices_per_page)
        * pool.pages_per_slice;
    assert(start_page <= end_page);
    if (start_page == end_page)
        return;

    uint8_t *start_pointer = (uint8_t *)GetPage(pool, start_page);
    uint8_t *end_pointer = (uint8_t *)GetPage(pool, end_page);

    mmap_discard_pages(start_pointer, end_pointer - start_pointer);
}

void
slice_area::Compress(struct slice_pool &pool)
{
    unsigned position = 0;

    while (true) {
        unsigned first_free = FindFree(pool, position);
        if (first_free == pool.slices_per_page)
            break;

        unsigned first_allocated = FindAllocated(pool, first_free + 1);
        PunchSliceRange(pool, first_free, first_allocated);

        position = first_allocated;
    }
}

/*
 * slice_pool methods
 *
 */

inline
slice_pool::slice_pool(size_t _slice_size, unsigned _slices_per_area)
{
    assert(_slice_size > 0);
    assert(_slices_per_area > 0);

    if (_slice_size <= mmap_page_size() / 2) {
        slice_size = align_size(_slice_size);

        slices_per_page = mmap_page_size() / slice_size;
        pages_per_slice = 1;

        pages_per_area = divide_round_up(_slices_per_area,
                                         slices_per_page);
    } else {
        slice_size = align_page_size(_slice_size);

        slices_per_page = 1;
        pages_per_slice = slice_size / mmap_page_size();

        pages_per_area = _slices_per_area * pages_per_slice;
    }

    slices_per_area = (pages_per_area / pages_per_slice) * slices_per_page;

    const struct slice_area *area = nullptr;
    const size_t header_size = sizeof(*area)
        + sizeof(area->slices[0]) * (slices_per_area - 1);
    header_pages = divide_round_up(header_size, mmap_page_size());

    area_size = mmap_page_size() * (header_pages + pages_per_area);

    list_init(&areas);
}

inline
slice_pool::~slice_pool()
{
    while (!list_empty(&areas)) {
        struct slice_area *area = (struct slice_area *)areas.next;

        /* must be empty at this point, or it's a memory leak */
        assert(area->allocated_count == 0);

        list_remove(&area->siblings);
        area->Delete(*this);
    }
}

struct slice_pool *
slice_pool_new(size_t slice_size, unsigned slices_per_area)
{
    return new slice_pool(slice_size, slices_per_area);
}

void
slice_pool_free(struct slice_pool *pool)
{
    delete pool;
}

size_t
slice_pool_get_slice_size(const struct slice_pool *pool)
{
    assert(pool != nullptr);

    return pool->slice_size;
}

inline void
slice_pool::Compress()
{
    for (struct slice_area *area = (struct slice_area *)areas.next,
             *next = (struct slice_area *)area->siblings.next;
         &area->siblings != &areas;
         area = next, next = (struct slice_area *)area->siblings.next) {
        if (area->IsEmpty()) {
            list_remove(&area->siblings);
            area->Delete(*this);
        } else
            area->Compress(*this);
    }
}

void
slice_pool_compress(struct slice_pool *pool)
{
    pool->Compress();
}

gcc_pure
inline struct slice_area *
slice_pool::FindNonFullArea()
{
    for (struct slice_area *area = (struct slice_area *)areas.next;
         &area->siblings != &areas;
         area = (struct slice_area *)area->siblings.next)
        if (!area->IsFull(*this))
            return area;

    return nullptr;
}

inline struct slice_area *
slice_pool::GetArea()
{
    struct slice_area *area = FindNonFullArea();
    if (area == nullptr) {
        area = slice_area::New(*this);
        list_add(&area->siblings, &areas);
    }

    return area;
}

struct slice_area *
slice_pool_get_area(struct slice_pool *pool)
{
    return pool->GetArea();
}

inline void *
slice_area::Alloc(struct slice_pool &pool)
{
    assert(!IsFull(pool));

    const unsigned i = free_head;
    struct slice_slot *const slot = &slices[i];

    ++allocated_count;
    free_head = slot->next;
    slot->next = ALLOCATED;

    return GetSlice(pool, i);
}

void *
slice_alloc(struct slice_pool *pool, struct slice_area *area)
{
    assert(pool != nullptr);
    assert(area != nullptr);

    return area->Alloc(*pool);
}

void
slice_area::Free(struct slice_pool &pool, void *p)
{
    unsigned i = IndexOf(pool, p);
    assert(slices[i].IsAllocated());

    slices[i].next = free_head;
    free_head = i;

    assert(allocated_count > 0);
    --allocated_count;
}

void
slice_free(struct slice_pool *pool, struct slice_area *area, void *p)
{
    assert(pool != nullptr);
    assert(area != nullptr);

    area->Free(*pool, p);
}
