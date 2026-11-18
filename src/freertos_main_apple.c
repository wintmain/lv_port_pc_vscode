/**
 * @file    freertos_main_apple.c
 * @brief   Apple entry point for the FreeRTOS build.
 * @license MIT License
 *
 * On Apple, src/freertos_main.c compiles empty (it targets non-Apple). This
 * file owns main() and the kernel hooks for every Apple build. Two models:
 *
 *  - LV_USE_OS == LV_OS_FREERTOS : decoupled — every LVGL call runs inside a
 *    FreeRTOS task; SDL stays on the main thread (Cocoa requires that), so
 *    LVGL's FreeRTOS mutexes are always touched from a valid task.
 *  - otherwise (PTHREAD / NONE)   : main-thread — LVGL + SDL run on the main
 *    thread; the FreeRTOS scheduler runs on a background pthread.
 */

#include "lvgl/lvgl.h"
#include "FreeRTOS.h"
#include "task.h"

#if defined(__APPLE__)

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  FreeRTOS kernel hooks (shared by both models)                     */
/* ------------------------------------------------------------------ */
void vApplicationMallocFailedHook(void)
{
    printf("Malloc failed! Available heap: %ld bytes\n", (long)xPortGetFreeHeapSize());
    for (;;) { }
}
void vApplicationIdleHook(void) { }
void vApplicationTickHook(void) { }
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("Stack overflow in task %s\n", pcTaskName);
    for (;;) { }
}

/* ------------------------------------------------------------------ */
/*  Background pthread that runs the scheduler (shared)               */
/* ------------------------------------------------------------------ */
static void *scheduler_thread(void *arg)
{
    (void)arg;
    vTaskStartScheduler();
    return NULL;
}

/* ================================================================== */
/*  Model A: LV_USE_OS != LV_OS_FREERTOS  (PTHREAD / NONE)            */
/*  Main thread -> lv_init + sdl_hal_init + lv_timer_handler loop     */
/*  Bg pthread  -> vTaskStartScheduler                                */
/* ================================================================== */
#if LV_USE_OS != LV_OS_FREERTOS

#include "hal/hal.h"

static void another_task(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        printf("Second Task is running :)\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void create_hello_world_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) { printf("Error: Failed to create screen object\n"); return; }
    lv_obj_t *label = lv_label_create(screen);
    if (label == NULL) { printf("Error: Failed to create label object\n"); return; }
    lv_label_set_text(label, "Hello, World!");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_scr_load(screen);
}

int main(int argc, char **argv)
{
    LV_UNUSED(argc);
    LV_UNUSED(argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    lv_init();

    if (xTaskCreate(another_task, "Another Task", 1024, NULL, 1, NULL) != pdPASS) {
        printf("Error creating another task\n");
    }

    pthread_t sched;
    if (pthread_create(&sched, NULL, scheduler_thread, NULL) != 0) {
        printf("Error creating scheduler thread\n");
        return 1;
    }

    sdl_hal_init(320, 480);
    create_hello_world_screen();

    while (true) {
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY) {
            sleep_time_ms = LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }
    return 0;
}

/* ================================================================== */
/*  Model B: LV_USE_OS == LV_OS_FREERTOS  (decoupled)                 */
/*  FreeRTOS task : all LVGL calls  --framebuffer-->  main thread: SDL */
/* ================================================================== */
#else /* LV_USE_OS == LV_OS_FREERTOS */

#include <SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXP_W     480
#define EXP_H     320
#define EXP_BPP   4   /* XRGB8888 -> matches SDL_PIXELFORMAT_XRGB8888 */

/* Shared framebuffer: written by the LVGL task's flush callback, blitted to
 * SDL by the main thread. Protected by s_fb_mtx. */
static uint32_t *s_fb;
static pthread_mutex_t s_fb_mtx;
static pthread_cond_t  s_fb_cond;
static bool s_frame_ready;

/* Quit flag + pointer state: written by the main thread (SDL pump), read by
 * the LVGL task (indev read). Protected by s_state_mtx. */
static pthread_mutex_t s_state_mtx;
static bool s_quit;
static int  s_mouse_x;
static int  s_mouse_y;
static bool s_mouse_down;

static uint32_t exp_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void exp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area; /* FULL render mode: whole screen flushed at once. */
    pthread_mutex_lock(&s_fb_mtx);
    memcpy(s_fb, px_map, (size_t)EXP_W * EXP_H * EXP_BPP);
    s_frame_ready = true;
    pthread_cond_signal(&s_fb_cond);
    pthread_mutex_unlock(&s_fb_mtx);
    lv_display_flush_ready(disp);
}

static void exp_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    pthread_mutex_lock(&s_state_mtx);
    data->point.x = s_mouse_x;
    data->point.y = s_mouse_y;
    data->state = s_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    pthread_mutex_unlock(&s_state_mtx);
}

static void on_btn_clicked(lv_event_t *e)
{
    (void)e;
    static int clicks = 0;
    clicks++;
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text_fmt(label, "Clicked %d time(s)  (on task '%s')",
                          clicks, pcTaskGetTaskName(NULL));
}

static void create_demo(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL internal OS = FreeRTOS\n(LVGL runs inside a FreeRTOS task)");
    lv_obj_set_style_text_color(title, lv_color_hex(0x80e0ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 200, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click me");
    lv_obj_center(btn_label);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text(info, "Clicked 0 time(s)");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_event_cb(btn, on_btn_clicked, LV_EVENT_CLICKED, info);
    lv_scr_load(scr);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    printf("[exp] lvgl_task started on FreeRTOS task '%s'\n", pcTaskGetTaskName(NULL));

    lv_init();
    lv_tick_set_cb(exp_tick_ms);

    static uint32_t draw_buf[EXP_W * EXP_H];
    lv_display_t *disp = lv_display_create(EXP_W, EXP_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, exp_flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, exp_pointer_read);

    create_demo();

#if configUSE_STATS_FORMATTING_FUNCTIONS
    char tasklist[1024];
    vTaskList(tasklist);
    printf("[exp] FreeRTOS task list after LVGL init:\n%s\n", tasklist);
#else
    printf("[exp] (define configUSE_STATS_FORMATTING_FUNCTIONS=1 to dump the task list)\n");
#endif

    while (true) {
        pthread_mutex_lock(&s_state_mtx);
        bool quit = s_quit;
        pthread_mutex_unlock(&s_state_mtx);
        if (quit) break;
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("[exp] lvgl_task exiting\n");
    vTaskDelete(NULL);
}

static void another_task(void *arg)
{
    (void)arg;
    while (true) {
        printf("[exp] another_task alive on '%s'  (heap free=%ld)\n",
               pcTaskGetTaskName(NULL), (long)xPortGetFreeHeapSize());
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[exp] === LV_USE_OS == LV_OS_FREERTOS ===\n");
    printf("[exp] Main (SDL) thread is a plain pthread, NOT a FreeRTOS task.\n");
    printf("[exp] All LVGL calls run inside the 'LVGL' FreeRTOS task.\n");

    s_fb = (uint32_t *)malloc((size_t)EXP_W * EXP_H * EXP_BPP);
    if (!s_fb) { fprintf(stderr, "[exp] malloc framebuffer failed\n"); return 1; }
    memset(s_fb, 0, (size_t)EXP_W * EXP_H * EXP_BPP);
    pthread_mutex_init(&s_fb_mtx, NULL);
    pthread_cond_init(&s_fb_cond, NULL);
    pthread_mutex_init(&s_state_mtx, NULL);
    s_frame_ready = false;
    s_quit = false;
    s_mouse_x = EXP_W / 2;
    s_mouse_y = EXP_H / 2;
    s_mouse_down = false;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[exp] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("LVGL + FreeRTOS (LVGL internals = FreeRTOS)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       EXP_W, EXP_H, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, EXP_W, EXP_H);
    if (!win || !ren || !tex) {
        fprintf(stderr, "[exp] SDL create failed: %s\n", SDL_GetError());
        return 1;
    }

    if (xTaskCreate(lvgl_task, "LVGL", 8192, NULL, 1, NULL) != pdPASS) {
        fprintf(stderr, "[exp] xTaskCreate(lvgl_task) failed\n");
        return 1;
    }
    if (xTaskCreate(another_task, "Second", 1024, NULL, 1, NULL) != pdPASS) {
        fprintf(stderr, "[exp] xTaskCreate(another_task) failed\n");
        return 1;
    }

    pthread_t sched;
    if (pthread_create(&sched, NULL, scheduler_thread, NULL) != 0) {
        fprintf(stderr, "[exp] pthread_create(scheduler) failed\n");
        return 1;
    }

    SDL_Event ev;
    while (!s_quit) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    pthread_mutex_lock(&s_state_mtx); s_quit = true; pthread_mutex_unlock(&s_state_mtx);
                    break;
                case SDL_MOUSEMOTION:
                    pthread_mutex_lock(&s_state_mtx);
                    s_mouse_x = ev.motion.x; s_mouse_y = ev.motion.y;
                    pthread_mutex_unlock(&s_state_mtx);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        pthread_mutex_lock(&s_state_mtx);
                        s_mouse_down = true; s_mouse_x = ev.button.x; s_mouse_y = ev.button.y;
                        pthread_mutex_unlock(&s_state_mtx);
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        pthread_mutex_lock(&s_state_mtx); s_mouse_down = false;
                        pthread_mutex_unlock(&s_state_mtx);
                    }
                    break;
                default: break;
            }
        }
        pthread_mutex_lock(&s_fb_mtx);
        while (!s_frame_ready && !s_quit) pthread_cond_wait(&s_fb_cond, &s_fb_mtx);
        if (s_frame_ready) {
            SDL_UpdateTexture(tex, NULL, s_fb, EXP_W * EXP_BPP);
            s_frame_ready = false;
        }
        pthread_mutex_unlock(&s_fb_mtx);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    SDL_Delay(100);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("[exp] done\n");
    return 0;
}

#endif /* LV_USE_OS == LV_OS_FREERTOS */

#endif /* defined(__APPLE__) */
