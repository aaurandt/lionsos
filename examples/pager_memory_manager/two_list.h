#ifndef FRAME_TABLE_H
#define FRAME_TABLE_H

#include "types.h"
#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <os/sddf.h>
#include <sddf/blk/queue.h> 
#include <sddf/blk/storage_info.h>
#include <sddf/blk/config.h>
#include <sddf/util/printf.h>

static void remove_from_list(struct list *l, tl_frame_t *node);
static void push_head(struct list *l, tl_frame_t *node);
static inline void promote_page(tl_frame_t *frame, int pd_idx); // Added prototype for forward reference
static void refill_inactive(int pd_idx, int num);

static struct list activelist[MAX_PDS];
static struct list inactivelist[MAX_PDS];
static struct list freelist[MAX_PDS];
tl_frame_t frame_table[MAX_PDS][HEAP_PAGES]; // this is a static list of frames
int free_frames_idx[MAX_PDS] = {-1};

/**
 * initialise the free frames
 */
static inline void init_frame(frame_pd_id *current_frame, uintptr_t frame_map_addr) {
    int pd_idx = current_frame->pd_idx;
    ++free_frames_idx[pd_idx];
    frame_table[pd_idx][free_frames_idx[pd_idx]] = (tl_frame_t){ .data = (char *)(FRAME_DATA + (((((uintptr_t)current_frame) - frame_map_addr) / sizeof(frame_pd_id)) * 4096)), .cap = current_frame->frame_cap, .page = NULL, .next = NULL, .prev = NULL, .active = false, .dirty = false };
    push_head(&freelist[pd_idx], &frame_table[pd_idx][free_frames_idx[pd_idx]]);
}

/**
 * Used to calculate the offset of the frame to get the address of the frame within the pager.
 */
static inline uintptr_t get_frame_offset(uintptr_t frame_addr, int pd_idx) {
    return (frame_addr - (uintptr_t)frame_table[pd_idx]) / sizeof(tl_frame_t);
}

static inline void promote_page(tl_frame_t *frame, int pd_idx) {
    // sddf_printf("promote\n");
    remove_from_list(&inactivelist[pd_idx], frame);
    push_head(&activelist[pd_idx], frame);
    frame->active = true;
}

/**
 * returns the prev frame.
 */
static inline tl_frame_t *demote_page(tl_frame_t *frame, int pd_idx) {
    // sddf_printf("demote\n");
    tl_frame_t *prev = frame->prev;
    remove_from_list(&activelist[pd_idx], frame);
    push_head(&inactivelist[pd_idx], frame);
    frame->active = false;
    return prev;
}

static inline void remove_from_list(struct list *l, tl_frame_t *node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        l->head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        l->tail = node->prev;
    }

    node->next = NULL;
    node->prev = NULL;
    l->size--;
}

static inline void push_head(struct list *l, tl_frame_t *node) {
    node->prev = NULL;
    node->next = l->head;
    if (l->head != NULL) {
        l->head->prev = node;
    } else {
        l->tail = node;
    }
    l->head = node;
    l->size++;
}

/**
 * WARNING: inappropriate num may cause infinite looping.
 */
static void refill_inactive(int pd_idx, int num) {
    // sddf_printf("refilling inactive");
    while (num) {
        tl_frame_t *ptr = activelist[pd_idx].tail;
        if (ptr == NULL) return; // prevent infinite loops.
        while (ptr != NULL) {
            if (ptr->referenced) {
                // move to head
                ptr->referenced = false;
                tl_frame_t *prev = ptr->prev;
                microkit_arm_page_unmap(ptr->cap);
                remove_from_list(&activelist[pd_idx], ptr);
                push_head(&activelist[pd_idx], ptr);
                ptr = prev;
            } else {
                // move to inactive
                ptr = demote_page(ptr, pd_idx);
                if (!--num) return;
            }
        }
    }
}


// this is only called during eviction, so frame should be cleared.
static tl_frame_t *get_frame(uint32_t pd_idx) {
    // sddf_printf("getting frame\n");
    // If we have free frames, use one
    if (freelist[pd_idx].head != NULL) {
        tl_frame_t *frame = freelist[pd_idx].head;
        remove_from_list(&freelist[pd_idx], frame);
        push_head(&activelist[pd_idx], frame);
        frame->active = true;
        return frame;
    }

    // Otherwise, evict from inactive/active
    // how many should I keep in the inactivelist?
    if (inactivelist[pd_idx].size < 1) {
        refill_inactive(pd_idx, 100);
    }

    tl_frame_t *ef = inactivelist[pd_idx].tail;
    promote_page(ef, pd_idx);
    return ef;
}

// freeing frames is not implemented yet
// /**
//  * Frees a frame: removes it from the active/inactive list 
//  * and adds it back to the free frames pool.
//  */
// static void free_frame(tl_frame_t *frame) {
//     if (frame == NULL) return;
    
//     // 1. Unmap hardware
//     microkit_arm_page_unmap(frame->cap);
//     // 2. Remove from active/inactive list
//     if (frame->active) {
//         remove_from_list(&activelist[frame->pd_idx], frame);
//     } else {
//         remove_from_list(&inactivelist[frame->pd_idx], frame);
//     }

//     // 3. Reset metadata
//     frame->page = NULL;
//     frame->active = false;
//     frame->dirty = false;
//     frame->referenced = false;
    
//     // 4. Add to free list
//     push_head(&freelist[frame->pd_idx], frame);
//     memset(get_frame_data(frame), 0, 4096);
// }

#endif