/**
 * @file lv_async.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_async.h"
#include "lv_mem.h"
#include "lv_timer.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

typedef struct _lv_async_info_t {
    lv_async_cb_t cb;
    void * user_data;
} lv_async_info_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void lv_async_timer_cb(lv_timer_t * timer);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_res_t lv_async_call(lv_async_cb_t async_xcb, void * user_data)
{
    /*Allocate an info structure*/
    lv_async_info_t * info = lv_mem_alloc(sizeof(lv_async_info_t));

    if(info == NULL)
        return LV_RES_INV;

    /* Set callback and user_data BEFORE creating the timer to avoid race where
       the timer could be executed before the fields are initialized. */
    info->cb = async_xcb;
    info->user_data = user_data;

    /*Create a new timer*/
    lv_timer_t * timer = lv_timer_create(lv_async_timer_cb, 0, info);

    if(timer == NULL) {
        lv_mem_free(info);
        return LV_RES_INV;
    }

    lv_timer_set_repeat_count(timer, 1);
    return LV_RES_OK;
}

lv_res_t lv_async_call_cancel(lv_async_cb_t async_xcb, void * user_data)
{
    lv_timer_t * timer = lv_timer_get_next(NULL);
    lv_res_t res = LV_RES_INV;

    while(timer != NULL) {
        /*Find the next timer node*/
        lv_timer_t * timer_next = lv_timer_get_next(timer);

        /*Find async timer callback*/
        if(timer->timer_cb == lv_async_timer_cb) {
            lv_async_info_t * info = (lv_async_info_t *)timer->user_data;
            if(info == NULL) {
                /* defensive: skip corrupt entry */
                timer = timer_next;
                continue;
            }

            /*Match user function callback and user data*/
            if(info->cb == async_xcb && info->user_data == user_data) {
                lv_timer_del(timer);
                lv_mem_free(info);
                res = LV_RES_OK;
            }
        }

        timer = timer_next;
    }

    return res;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_async_timer_cb(lv_timer_t * timer)
{
    if (timer == NULL) return;

    lv_async_info_t * info = (lv_async_info_t *)timer->user_data;

    /* Defensive checks: ensure info and callback are valid before calling.
       If either is NULL, skip the call but free info if present to avoid leak.
       (If info == NULL we cannot free; that's a sign of memory corruption). */
    if (info != NULL) {
        if (info->cb != NULL) {
            /* Safe to call user callback */
            info->cb(info->user_data);
        } else {
            /* info->cb is NULL — skip calling. Consider logging this situation
               during debugging: it indicates something unexpected in the app. */
        }
        lv_mem_free(info);
    } else {
        /* Extremely unlikely: timer->user_data == NULL. This indicates memory
           corruption or a bug somewhere that overwrote the pointer.
           Best to skip to avoid panic. */
    }
}
