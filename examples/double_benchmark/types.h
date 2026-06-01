#ifndef TYPES_H
#define TYPES_H

#include <microkit.h>
#include <stdbool.h>
#include <stdint.h>

#include <sddf/util/printf.h>

// number of protection domains supported by pager
#define MAX_PDS 2
// number of unmapped frames
#define HEAP_PAGES 500000
// number of stack pages
#define STACK_PAGES 2
// where the heap starts
#define BRK_START 0x8000000000
#define BRK_END 0x9000000000
#define STACK_TOP 0x10000000000
#define STACK_BOTTOM 0x0FFFFFFE000

#define PAGE_BITS 12ULL
#define PAGE_SIZE 4096ULL
#define PAGE_MASK 0xFFFFFFFFFFFFF000ULL

#define ROUND_DOWN_TO_4K(x)      ((uintptr_t)(x) & PAGE_MASK)
#define INDEX_INTO_MMAP_ARRAY(x) ((ROUND_DOWN_TO_4K(x) - (uintptr_t)BRK_START) >> PAGE_BITS)
#define INDEX_INTO_HEAP(x) ((x - BRK_START) >> PAGE_BITS)
#define INDEX_INTO_STACK(x) ((x - STACK_BOTTOM) >> PAGE_BITS)
// where frame data is mapped into for pager metadata
#define FRAME_DATA 0x8000000000

enum benchmark_trigger {
    START,
    STOP
};

extern uintptr_t frame_map_addr;
uintptr_t frame_map_addr = 0;

typedef unsigned long long Cap;

typedef struct microkit_data {
    Cap frame_cap;
    uintptr_t pd_idx;
} frame_pd_id;

struct frame;

typedef struct pe {
    struct frame *frame_addr;
    int pagefile_offset;
} pe;

typedef struct FrameInfo {
    seL4_CPtr cap;
    pe *page;
    uint32_t next;
    uint32_t prev;
    uint32_t pd_idx;
} FrameInfo;

enum paging_state {
    PAGE_OUT,
    PAGE_IN,
};

struct page_request_info {
    uint32_t pd_idx;
    uintptr_t fault_addr;
    struct frame *frame;
    enum paging_state state;
    bool is_write;
};

/**
 * if page is not null that means it is mapped
 */
typedef struct frame {
    struct frame *next;
    struct frame *prev;
    seL4_CPtr cap;
    pe *page;
    bool referenced;
    bool dirty;
    bool active;
    char *data;
} tl_frame_t;

struct list {
    tl_frame_t *head;
    tl_frame_t *tail;
    int size;
};

struct fault_info {
    uintptr_t pc;
    bool write;
};

#endif