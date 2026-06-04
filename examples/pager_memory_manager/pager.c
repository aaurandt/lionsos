#include <microkit.h>
#include <stdint.h>
#include <stdlib.h>
#include "types.h"
#include "pagefile.h"
#include "two_list.h"
#include <os/sddf.h>
#include <sddf/blk/queue.h> 
#include <sddf/blk/storage_info.h>
#include <sddf/blk/config.h>
#include <sddf/util/printf.h>
#include <string.h>
#include "pager.h"
#include <sddf/benchmark/config.h>
#include <sddf/benchmark/bench.h>

uint64_t unmapped_frames_addr;
uint64_t num_frames;
__attribute__((__section__(".blk_client_config"))) blk_client_config_t blk_config;
static blk_queue_handle_t blk_queue;
__attribute__((__section__(".benchmark_client_config"))) benchmark_client_config_t benchmark_config;
benchmark_client_config_t *bench;
frame_pd_id *frames;
// so that we don't process the same fault multiple times.
struct fault_info current_faults[MAX_PDS];
// this is where the heaps are all located.
char *heaps = (char *)FRAME_DATA;
pe stack_table[MAX_PDS][STACK_PAGES];
pe heap_table[MAX_PDS][HEAP_PAGES];
unsigned int vspaces[MAX_PDS];


// assumes that the fault addr is already rounded down.
static inline pe *retrieve_page(uintptr_t fault_addr, uint32_t pd_idx) {
    // check whether the fault is in the stack or heap
    if (fault_addr > BRK_END) {
        return &stack_table[pd_idx][INDEX_INTO_STACK(fault_addr)];
    } else {
        return &heap_table[pd_idx][INDEX_INTO_HEAP(fault_addr)];
    }   
}


void init(void)
{
    // initialise the page tables
    memset(stack_table, 0, MAX_PDS * STACK_PAGES * sizeof(pe));
    memset(heap_table, 0, MAX_PDS * HEAP_PAGES * sizeof(pe));
    frames = (frame_pd_id *) unmapped_frames_addr;
    frame_map_addr = (uintptr_t)frames;
    for (int i = 0; i < MAX_PDS; ++i) {
        for (int j = 0; j < STACK_PAGES; ++j) {
            stack_table[i][j].pagefile_offset = -1;
        }
        for (int j = 0; j < HEAP_PAGES; ++j) {
            heap_table[i][j].pagefile_offset = -1;
        }
        free_frames_idx[i] = -1;
        current_faults[i] = (struct fault_info){ .write = false, .pc = -1};
    } 

    
    assert(blk_config_check_magic(&blk_config));
    blk_queue_init(&blk_queue, blk_config.virt.req_queue.vaddr, blk_config.virt.resp_queue.vaddr,
                   blk_config.virt.num_buffers);

    // initialises the frame caps.
    int frame_indicies[MAX_PDS] = {0};
    for (int i = 0; i < num_frames; ++i) {
        uint32_t next = i + 1, prev = i - 1;
        frame_pd_id *cur_frame = &frames[i];
        init_frame(cur_frame, frame_map_addr);
    }
}

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply_msginfo)
{   
    uintptr_t fault_addr = ROUND_DOWN_TO_4K(microkit_mr_get(1));
    uint64_t fsr = microkit_mr_get(3);
    bool is_write = (fsr >> 6) & 1;
    uint32_t pd_idx = child;
    pe *page = retrieve_page(fault_addr, pd_idx);
    tl_frame_t *old_frame = page->frame_addr;
    current_faults[child].pc = microkit_mr_get(0);
    current_faults[child].write = is_write;
    if (old_frame) {
        old_frame->referenced = true;
        int err;
        if (is_write) { 
            old_frame->dirty = true;
            if (page->pagefile_offset != -1) mark_pagefile_slot_free(page);
            err = microkit_arm_page_map_rw(old_frame->cap, vspaces[pd_idx], fault_addr);
        } else {
            err = microkit_arm_page_map_ro(old_frame->cap, vspaces[pd_idx], fault_addr);
        }
        if (err) {
            sddf_printf("there is an error %p\n", fault_addr);
        }
        return seL4_True;
    }

    microkit_pd_stop(child);
    tl_frame_t *frame = get_frame(pd_idx);
    if (frame->dirty) {
        // do a page out
        page_out(frame, pd_idx, fault_addr);
        return seL4_True;
    } else if (frame->page) {
        if (frame->page->pagefile_offset == -1) {
            page_out(frame, pd_idx, fault_addr);
            return seL4_True;
        } else {
            microkit_arm_page_unmap(frame->cap);
            frame->page->frame_addr = NULL;
        }
        
    }
    after_page_out(frame, pd_idx, fault_addr);
    return seL4_True;
}

/**
 * This gets called once the write/read operation is complete.
 */
void notified(microkit_channel ch)
{
    blk_resp_status_t status = -1;
    uint16_t count = -1;
    uint32_t id = -1;

    while (blk_dequeue_resp(&blk_queue, &status, &count, &id) != -1) {
        assert(status == BLK_RESP_OK);
        assert(count == 1);

        // queue the next thing depending on what was done.
        if (page_continuations[id].state == PAGE_OUT) {
            // unmap the frame.
            microkit_arm_page_unmap(page_continuations[id].frame->cap);
            after_page_out( page_continuations[id].frame, page_continuations[id].pd_idx, page_continuations[id].fault_addr);
        } else {
            after_page_in(page_continuations[id].frame, page_continuations[id].pd_idx, page_continuations[id].fault_addr, true, id);
        }
    }
}



void after_page_in(tl_frame_t *frame, uint32_t pd_idx, uintptr_t fault_addr, bool paged_in, int request_id) {
    // map the page to the frame
    frame->page = retrieve_page(fault_addr, pd_idx);
    frame->page->frame_addr = frame;
    if (paged_in) {
        memcpy(frame->data,
       (char *)blk_config.data.vaddr + request_id * 4096, 4096);
    } else {
        memset(frame->data, 0, 4096);
    }
    int err = 0;
    if (current_faults[pd_idx].write) {
        err = microkit_arm_page_map_rw(frame->cap, vspaces[pd_idx], fault_addr);
        if (frame->page->pagefile_offset != -1) mark_pagefile_slot_free(frame->page);
        frame->dirty = true;
    } else {
        err = microkit_arm_page_map_ro(frame->cap, vspaces[pd_idx], fault_addr);
        frame->dirty = false;
    }
    if (err) {
        sddf_printf("page map failed! %d\n", err);
    }
    
    microkit_pd_restart(pd_idx, current_faults[pd_idx].pc);
}

void page_in(tl_frame_t *frame, uint32_t pd_idx, uintptr_t fault_addr) {
    // sddf_printf("paging in %p\n", fault_addr);
    // get the slot
    int slot = retrieve_page(fault_addr, pd_idx)->pagefile_offset;
    // queue the read
    int request_id = get_request_id();
    page_continuations[request_id] = (struct page_request_info){ .frame = frame, .pd_idx = pd_idx, .fault_addr = fault_addr, .state = PAGE_IN }; // TODO: fill this out with relevant info.
    blk_enqueue_req(&blk_queue, BLK_REQ_READ, request_id * 4096, slot, 1, request_id);
    sddf_notify(blk_config.virt.id);
}

void after_page_out(tl_frame_t *frame, uint32_t pd_idx, uintptr_t fault_addr) {
    if (retrieve_page(fault_addr, pd_idx)->pagefile_offset != -1) {
        page_in(frame, pd_idx, fault_addr);
    } else {
        after_page_in(frame, pd_idx, fault_addr, false, 0);
    }
}

void page_out(tl_frame_t *frame, uint32_t pd_idx, uintptr_t fault_addr) {
    int slot = get_pagefile_slot();
    pe* page = frame->page;
    page->pagefile_offset = slot;
    int request_id = get_request_id();
    page_continuations[request_id] = (struct page_request_info){ .frame = frame, .pd_idx = pd_idx, .fault_addr = fault_addr, .state = PAGE_OUT };
    char *data_dest = (char *) blk_config.data.vaddr + request_id * 4096;
    memcpy(data_dest, frame->data, 4096);
    blk_enqueue_req(&blk_queue, BLK_REQ_WRITE, request_id * 4096, slot, 1, request_id);
    sddf_notify(blk_config.virt.id);
    page->frame_addr = NULL;
}

/**
 * This gets called to start / stop a benchmark running.
 */
seL4_MessageInfo_t protected(microkit_channel ch, microkit_msginfo msginfo)
{
    enum benchmark_trigger cmd = microkit_mr_get(0);
    if (cmd == START) {
        sddf_notify(benchmark_config.start_ch);
    } else if (cmd == STOP) {
        sddf_notify(benchmark_config.stop_ch);
    }

}



