/*

  Copyright (c) 2015 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include <stddef.h>
#include <stdio.h>

#include "cr.h"
#include "libmill.h"
#include "poller.h"
#include "stack.h"
#include "utils.h"

/* Size of the buffer for temporary storage of values received from channels.
   It should be properly aligned and never change if there are any stacks
   allocated at the moment. */
size_t mill_valbuf_size = 128;

/* Valbuf for tha main coroutine. */
char mill_main_valbuf[128];

volatile int mill_unoptimisable1 = 1;
volatile void *mill_unoptimisable2 = NULL;

struct mill_cr mill_main = {0};

struct mill_cr *mill_running = &mill_main;

/* Queue of coroutines scheduled for execution. */
static struct mill_slist mill_ready = {0};

static void *mill_getvalbuf(struct mill_cr *cr, size_t size) {
    /* Small valbufs don't require dynamic allocation. Also note that main
       coroutine doesn't have a stack allocated on the heap like other
       coroutines, so we have to handle valbuf in a special way. */
    if(mill_fast(cr != &mill_main)) {
        if(mill_fast(size <= mill_valbuf_size))
            return (void*)(((char*)cr) - mill_valbuf_size);
    }
    else {
        if(mill_fast(size <= sizeof(mill_main_valbuf)))
            return (void*)mill_main_valbuf;
    }
    /* Large valbufs are simply allocated on heap. */
    if(mill_fast(cr->valbuf && cr->valbuf_sz <= size))
        return cr->valbuf;
    void *ptr = realloc(cr->valbuf, size);
    if(!ptr)
        return NULL;
    cr->valbuf = ptr;
    cr->valbuf_sz = size;
    return cr->valbuf;
}

int goprepare(int count, size_t stack_size, size_t val_size) {
    /* If needed, make val size slightly bigger to align properly. */
    mill_valbuf_size = (val_size + 15) & ~((size_t)0xf);
    /* Preallocate the valbuf for the main coroutine. */
    if(!mill_getvalbuf(&mill_main, mill_valbuf_size))
        return 0;
    /* Allocate the stacks. */
    return mill_preparestacks(count, stack_size + mill_valbuf_size +
        sizeof(struct mill_cr));
}

int mill_suspend(void) {
    /* Even if process never gets idle, we have to process external events
       once in a while. The external signal may very well be a deadline or
       a user-issued command that cancels the CPU intensive operation. */
    static int counter = 0;
    if(counter >= 103) {
        mill_wait(0);
        counter = 0;
    }
    /* Store the context of the current coroutine, if any. */
    if(mill_running && mill_setjmp(&mill_running->ctx))
        return mill_running->result;
    while(1) {
        /* If there's a coroutine ready to be executed go for it. */
        if(!mill_slist_empty(&mill_ready)) {
            ++counter;
            struct mill_slist_item *it = mill_slist_pop(&mill_ready);
            mill_running = mill_cont(it, struct mill_cr, u_ready.item);
            mill_jmp(&mill_running->ctx);
        }
        /*  Otherwise, we are going to wait for sleeping coroutines
            and for external events. */
        mill_wait(1);
        mill_assert(!mill_slist_empty(&mill_ready));
        counter = 0;
    }
}

void mill_resume(struct mill_cr *cr, int result) {
    cr->result = result;
    cr->state = MILL_READY;
    mill_slist_push_back(&mill_ready, &cr->u_ready.item);
}

/* The intial part of go(). Starts the new coroutine.
   Returns the pointer to the top of its stack. */
void *mill_go_prologue(const char *created) {
    /* Allocate and initialise new stack. */
    struct mill_cr *cr = ((struct mill_cr*)mill_allocstack()) - 1;
    cr->valbuf = NULL;
    cr->valbuf_sz = 0;
    cr->cls = NULL;
    /* Suspend the parent coroutine and make the new one running. */
    if(mill_setjmp(&mill_running->ctx))
        return NULL;
    mill_resume(mill_running, 0);    
    mill_running = cr;
    /* Return pointer to the top of the stack. There's valbuf interposed
       between the mill_cr structure and the stack itself. */
    return (void*)(((char*)cr) - mill_valbuf_size);
}

/* The final part of go(). Cleans up after the coroutine is finished. */
void mill_go_epilogue(void) {
    if(mill_running->valbuf)
        free(mill_running->valbuf);
    mill_freestack(mill_running + 1);
    mill_running = NULL;
    /* Given that there's no running coroutine at this point
       this call will never return. */
    mill_suspend();
}

void mill_yield(const char *current) {
    /* This looks fishy, but yes, we can resume the coroutine even before
       suspending it. */
    mill_resume(mill_running, 0);
    mill_suspend();
}

void *mill_valbuf(struct mill_cr *cr, size_t size) {
    void *ptr = mill_getvalbuf(cr, size);
    if(!ptr)
        mill_panic("not enough memory to receive from channel");
    return ptr;
}

void *cls(void) {
    return mill_running->cls;
}

void setcls(void *val) {
    mill_running->cls = val;
}

