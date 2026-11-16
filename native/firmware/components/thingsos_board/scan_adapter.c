/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Derived from MindReset/read_pico_firmware@ed1e9331077451a0391769cd6c191568f7901807
 * components/epdiy/src/output_lcd/render_lcd.c. Upstream epdiy LGPL-3.0 applies.
 * Local extension: select an immutable 1 KiB LUT per physical row during a
 * synchronous scan. All scan timing, queues and error paths remain upstream.
 * No source from kiikoread is used. See scan_adapter.md.
 * Metalio (ESP32-S31): no S3 DCache preload; output bytes are converted to
 * panel pixel order by epd_output_reverse_pixels() after masking. See
 * native/metalio/components/epdiy/FORK-METALIO.md.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_cpu.h"

#include "output_common/render_method.h"

#ifdef RENDER_METHOD_LCD

#include <esp_idf_version.h>
#include <esp_log.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#if __has_include(<rom/cache.h>)
#include <rom/cache.h>
#elif __has_include(<esp32s3/rom/cache.h>)
#include <esp32s3/rom/cache.h>
#endif
#endif

#include "epd_internals.h"
#include "output_common/line_queue.h"
#include "output_common/lut.h"
#include "output_common/render_context.h"
#include "epd_board.h"
#include "epdiy.h"
#include "output_lcd/lcd_driver.h"
#include "output_lcd/render_lcd.h"

static const uint8_t* const* thingsos_row_tables;
void thingsos_scan_row_tables(const uint8_t* const* tables) {
    /* Set/clear only between complete synchronous scans. */
    thingsos_row_tables=tables;
}

/* Metalio E0470A14-AF: the outermost pixels flash black on full refreshes
   unless they are held. After the first clean frame the last output stage
   forces a border of this many physical pixels to 00 (hold, no drive) on every
   scan, as the vendor 2.0.70-debug firmware does. Multiple of 4 (one byte). */
static int border_pixels;
void thingsos_scan_border_hold(int pixels) {
    border_pixels=pixels>0?pixels&~3:0;
}
static inline void IRAM_ATTR border_hold(uint8_t* buf,int row,int width,int height) {
    const int bytes=border_pixels/4,line=width/4;
    if(!bytes)return;
    if(row<border_pixels || row>=height-border_pixels){memset(buf,0,line);return;}
    memset(buf,0,bytes);memset(buf+line-bytes,0,bytes);
}

/* Each scan starts once the first prefill lines are queued. Two producers on
   both cores claim lines alternately, so the line that triggers the scan does
   not prove that every earlier line has been committed; the ISR would then
   underrun (EPD_DRAW_EMPTY_LINE_QUEUE). Track commits per line and let the
   trigger wait, bounded, for the earlier ones (vendor 2.0.66-debug finding). */
#define READY_LINES 1024
/* One phase scan takes about 12 ms; 2 s means the scan is stuck. */
#define SCAN_TIMEOUT_MS 2000
static _Atomic uint8_t line_ready[READY_LINES];
static inline void IRAM_ATTR mark_ready(int line) {
    if(line>=0 && line<READY_LINES)atomic_store_explicit(&line_ready[line],1,memory_order_release);
}
/* Underrun diagnostics in the shape the vendor 2.0.7x build logs them
   (phase, prefill, first missing row, reason, consumed/total). One report per
   phase keeps a failing scan from flooding the console. */
static int s_reported_phase = -1;
static int s_prefill_lines; /* defined below */
/* Line-path cycle accounting: lut (pixel conversion), mask, reverse. Printed
   once per frame so the feeder budget can be compared with one scan line. */
static uint32_t s_cycles_lut, s_cycles_mask, s_cycles_reverse, s_measured_lines;
static int s_measure_frames;
static void report_underrun(RenderContext_t* ctx, const char* reason, int row) {
    int phase = ctx->current_frame;
    if (phase == s_reported_phase) return;
    s_reported_phase = phase;
    ESP_LOGE("epd_feed", "phase=%d prefill=%d first_missing_row=%d reason=%s consumed=%d/%d",
             phase, s_prefill_lines, row, reason, ctx->lines_consumed, ctx->lines_total);
}
/* The whole prefill prefix must be committed before the scan starts: claiming
   row N does not prove the other core committed the rows before it (vendor
   2.0.66-debug finding). Bounded at 2 s; on timeout the frame is reported as an
   underrun instead of starting with holes. */
static bool wait_prefix(RenderContext_t* ctx,int first,int line) {
    const int64_t deadline=esp_timer_get_time()+2000000;
    for(int i=first;i<line && i<READY_LINES;++i) {
        while(!atomic_load_explicit(&line_ready[i],memory_order_acquire)) {
            if(ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                report_underrun(ctx,"prefix-abort",i);
                return false;
            }
            if(esp_timer_get_time()>deadline){
                ctx->error|=EPD_DRAW_EMPTY_LINE_QUEUE;
                report_underrun(ctx,"prefix-timeout",i);
                return false;
            }
            esp_rom_delay_us(1);
        }
    }
    return true;
}

// declare vector optimized line mask application.
void epd_apply_line_mask_VE(uint8_t* line, const uint8_t* mask, int mask_len);

__attribute__((optimize("O3"))) static bool IRAM_ATTR
retrieve_line_isr(RenderContext_t* ctx, uint8_t* buf) {
    if (ctx->lines_consumed >= ctx->lines_total) {
        return false;
    }
    int thread = ctx->line_threads[ctx->lines_consumed];
    if (thread >= NUM_RENDER_THREADS) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
        ctx->lines_consumed += 1;
        return pdFALSE;
    }

    LineQueue_t* lq = &ctx->line_queues[thread];

    BaseType_t awoken = pdFALSE;

    if (lq_read(lq, buf) != 0) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        memset(buf, 0x00, ctx->display_width / 4);
    }

    if (ctx->lines_consumed >= ctx->display_height) {
        memset(buf, 0x00, ctx->display_width / 4);
    }
    ctx->lines_consumed += 1;
    return awoken;
}

/// start the next frame in the current update cycle
static void IRAM_ATTR handle_lcd_frame_done(RenderContext_t* ctx) {
    epd_lcd_frame_done_cb(NULL, NULL);
    epd_lcd_line_source_cb(NULL, NULL);

    // 扫描先于供数结束时解除生产者等待，禁止残留行进入下一相位。
    // Release producers if scan ends before consumption; never carry stale lines into the next phase.
    if (ctx->lines_consumed < ctx->lines_total) {
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
    }

    BaseType_t task_awoken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->frame_done, &task_awoken);

    portYIELD_FROM_ISR();
}

void lcd_do_update(RenderContext_t* ctx) {
    epd_set_mode(1);

    for (uint8_t k = 0; k < ctx->cycle_frames; k++) {
        epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
        prepare_context_for_next_frame(ctx);
        for (int i = 0; i < READY_LINES; i++) atomic_store_explicit(&line_ready[i], 0, memory_order_relaxed);
        atomic_thread_fence(memory_order_release);

        // start both feeder tasks
        xTaskNotifyGive(ctx->feed_tasks[!xPortGetCoreID()]);
        xTaskNotifyGive(ctx->feed_tasks[xPortGetCoreID()]);

        // transmission is started in renderer threads, now wait util it's done.
        // A scan that never starts or never ends is an underrun: stop the LCD
        // and let the caller power the panel down instead of hanging with the
        // rails on (Metalio vendor firmware uses the same 2 s bound).
        if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(SCAN_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE("epd_lcd", "LCD frame %u/%u timeout", (unsigned)k + 1, (unsigned)ctx->cycle_frames);
            ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            epd_lcd_line_source_cb(NULL, NULL);
            epd_lcd_abort_frame();
            xSemaphoreTake(ctx->frame_done, 0);
        }

        for (int i = 0; i < NUM_RENDER_THREADS; i++) {
            if (xSemaphoreTake(ctx->feed_done_smphr[i], pdMS_TO_TICKS(SCAN_TIMEOUT_MS)) != pdTRUE) {
                ESP_LOGE("epd_lcd", "feed %d timeout at frame %u", i, (unsigned)k);
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
            }
        }

        // DMA 回调和生产者均已停止后再清队列，由调用方执行欠载恢复。
        // Reset only after DMA callbacks and producers stop; let the caller recover from underrun.
        if (ctx->error) {
            for (int i = 0; i < NUM_RENDER_THREADS; i++) {
                lq_reset(&ctx->line_queues[i]);
            }
            break;
        }

        ctx->current_frame++;

        // make the watchdog happy.
        vTaskDelay(0);
    }

    epd_lcd_line_source_cb(NULL, NULL);
    epd_lcd_frame_done_cb(NULL, NULL);

    epd_set_mode(0);
}

__attribute__((optimize("O3"))) static bool IRAM_ATTR
push_pixels_isr(RenderContext_t* ctx, uint8_t* buf) {
    // Output no-op outside of drawn area
    if (ctx->lines_consumed < ctx->area.y) {
        memset(buf, 0, ctx->display_width / 4);
    } else if (ctx->lines_consumed >= ctx->area.y + ctx->area.height) {
        memset(buf, 0, ctx->display_width / 4);
    } else {
        memcpy(buf, ctx->static_line_buffer, ctx->display_width / 4);
        border_hold(buf, ctx->lines_consumed, ctx->display_width, ctx->display_height);
    }
    ctx->lines_consumed += 1;
    return pdFALSE;
}

/**
 * Populate the line mask for use in epd_push_pixels.
 */
static bool push_pixels_populate_line(RenderContext_t* ctx, int color) {
    // Select fill pattern by draw color
    int fill_byte = 0;
    switch (color) {
        case 0:
            fill_byte = DARK_BYTE;
            break;
        case 1:
            fill_byte = CLEAR_BYTE;
            break;
        default:
            fill_byte = 0x00;
    }

    // Compute a line mask based on the drawn area
    uint8_t* dirtyness = malloc(ctx->display_width / 2);
    if (!dirtyness) return false;

    memset(dirtyness, 0, ctx->display_width / 2);

    for (int i = 0; i < ctx->display_width; i++) {
        if ((i >= ctx->area.x) && (i < ctx->area.x + ctx->area.width)) {
            dirtyness[i / 2] |= i % 2 ? 0xF0 : 0x0F;
        }
    }
    epd_populate_line_mask(ctx->line_mask, dirtyness, ctx->display_width / 4);

    // mask the line pattern with the populated mask
    memset(ctx->static_line_buffer, fill_byte, ctx->display_width / 4);
    epd_apply_line_mask(ctx->static_line_buffer, ctx->line_mask, ctx->display_width / 4);
    epd_output_reverse_pixels(ctx->static_line_buffer, ctx->display_width / 4);

    free(dirtyness);
    return true;
}

void epd_push_pixels_lcd(RenderContext_t* ctx, short time, int color) {
    ctx->current_frame = 0;
    ctx->lines_total = ctx->display_height;
    ctx->lines_consumed = 0;
    ctx->static_line_buffer = malloc(ctx->display_width / 4);
    if (!ctx->static_line_buffer || !push_pixels_populate_line(ctx, color)) {
        free(ctx->static_line_buffer);
        ctx->static_line_buffer = NULL;
        ctx->error = EPD_DRAW_FAILED_ALLOC;
        return;
    }

    epd_lcd_frame_done_cb((frame_done_func_t)handle_lcd_frame_done, ctx);
    epd_lcd_line_source_cb((line_cb_func_t)&push_pixels_isr, ctx);

    epd_set_mode(1);
    epd_lcd_start_frame();
    if (xSemaphoreTake(ctx->frame_done, pdMS_TO_TICKS(SCAN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE("epd_lcd", "push pixels frame timeout");
        ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
        epd_lcd_line_source_cb(NULL, NULL);
        epd_lcd_abort_frame();
        xSemaphoreTake(ctx->frame_done, 0);
    }
    epd_set_mode(0);

    free(ctx->static_line_buffer);
    ctx->static_line_buffer = NULL;
}

#define int_min(a, b) (((a) < (b)) ? (a) : (b))

/* Prefill before the scan starts. The vendor raised this to 192 rows for S31;
   measured here it costs ~48 ms extra per page turn and does not change the
   20 MHz outcome (the feeder converts ~34 px/us and 20 MHz needs ~62 px/us, so
   any prefill depth drains within a frame). Kept at the vendor's validated
   127-row value until the line path itself is faster; fixed row ownership and
   the full-prefix commit below stay. The two producers own alternating rows, so
   each queue must hold at least half of the prefill (2 x 128 slots in render.c). */
#define THINGSOS_PREFILL_LINES 127
static int s_prefill_lines = THINGSOS_PREFILL_LINES;

void epd_lcd_set_prefill_lines(int lines) {
    if (lines < 4) lines = 4;
    s_prefill_lines = lines;
}

int epd_lcd_prefill_lines(void) {
    return s_prefill_lines;
}
__attribute__((optimize("O3"))) void IRAM_ATTR
lcd_calculate_frame(RenderContext_t* ctx, int thread_id) {
    assert(ctx->lut_lookup_func != NULL);
    uint8_t* input_line = ctx->feed_line_buffers[thread_id];

    LineQueue_t* lq = &ctx->line_queues[thread_id];
    int l = 0;

    // 另一生产者可能已启动并报告欠载；不得再次启动同一帧。
    // The other producer may have started and underrun already; never start the frame twice.
    if (ctx->error) return;

    // line must be able to hold 2-pixel-per-byte or 1-pixel-per-byte data
    memset(input_line, 0x00, ctx->display_width);

    EpdRect area = ctx->area;
    int min_y, max_y, bytes_per_line, _ppB;
    const uint8_t* ptr_start;
    get_buffer_params(ctx, &bytes_per_line, &ptr_start, &min_y, &max_y, &_ppB);

    assert(area.width == ctx->display_width && area.x == 0);

    /* Mask and pixel order are frame-wide facts. A full-screen frame (every
       column dirty, as stagger.inc forces for turns) has an all-0xFF mask, so
       the per-line AND is a no-op; and the 1ppB page lookup (S31) already
       produces panel order, so there is nothing left to reverse. */
    const bool mask_full = epd_line_mask_is_full(ctx->line_mask, ctx->display_width / 4);
    const bool reverse_pixels = EPD_OUTPUT_REVERSE_PIXELS && !ctx->lut_output_panel_order;

    // index of the line that triggers the frame output when processed
    const int trigger_line = int_min(s_prefill_lines, ctx->lines_total - 1);

    /* Fixed ownership: producer `thread_id` owns every NUM_RENDER_THREADS-th
       row. Dynamic claims let one core fill its queue before a deeper prefill
       could start, which is exactly the underrun the vendor removed in 2.0.7x. */
    for (l = thread_id; l < ctx->lines_total; l += NUM_RENDER_THREADS) {
        ctx->line_threads[l] = (uint8_t)thread_id;

        // queue is sufficiently filled to fill both bounce buffers, frame
        // can begin
        if (l == trigger_line) {
            if (!wait_prefix(ctx, 0, l)) return;
            epd_lcd_line_source_cb((line_cb_func_t)&retrieve_line_isr, ctx);
            epd_lcd_start_frame();
        }

        if (l < min_y || l >= max_y
            || (ctx->drawn_lines != NULL && !ctx->drawn_lines[l - area.y])) {
            uint8_t* buf = NULL;
            int wait_iters = 0;
            while (buf == NULL) {
                // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    report_underrun(ctx,"isr-empty",ctx->lines_consumed);
                    lq_reset(lq);
                    return;
                };

                buf = lq_current(lq);
                if (buf != NULL) break;
                vTaskDelay(0);
                if (++wait_iters > 30000) {
                    ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                    report_underrun(ctx,"queue-full",l);
                    lq_reset(lq);
                    return;
                }
            }
            memset(buf, 0x00, lq->element_size);
            lq_commit(lq);
            mark_ready(l);
            continue;
        }

        uint32_t* lp = (uint32_t*)input_line;
        const uint8_t* ptr = ptr_start + bytes_per_line * (l - min_y);

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        Cache_Start_DCache_Preload((uint32_t)ptr, ctx->display_width, 0);
#endif

        lp = (uint32_t*)ptr;

        uint8_t* buf = NULL;
        int wait_iters = 0;
        while (buf == NULL) {
            // break in case of errors
                if (ctx->error & EPD_DRAW_EMPTY_LINE_QUEUE) {
                    report_underrun(ctx,"isr-empty",ctx->lines_consumed);
                    lq_reset(lq);
                    return;
                };

            buf = lq_current(lq);
            if (buf != NULL) break;
            vTaskDelay(0);
            if (++wait_iters > 30000) {
                ctx->error |= EPD_DRAW_EMPTY_LINE_QUEUE;
                report_underrun(ctx,"queue-full",l);
                lq_reset(lq);
                return;
            }
        }
        const uint8_t* table=thingsos_row_tables ? thingsos_row_tables[l] : NULL;
        const uint32_t t_convert = esp_cpu_get_cycle_count();
        ctx->lut_lookup_func(lp, buf, table ? table : ctx->conversion_lut, ctx->display_width);
        const uint32_t t_lut = esp_cpu_get_cycle_count();

        // apply the line mask
        if(!mask_full) epd_apply_line_mask_VE(buf, ctx->line_mask, ctx->display_width / 4);
        const uint32_t t_mask = esp_cpu_get_cycle_count();
#if EPD_OUTPUT_REVERSE_PIXELS
        if(reverse_pixels) epd_output_reverse_pixels(buf, ctx->display_width / 4);
#endif
        const uint32_t t_reverse = esp_cpu_get_cycle_count();
        border_hold(buf, l, ctx->display_width, ctx->display_height);
        s_cycles_lut += t_lut - t_convert;
        s_cycles_mask += t_mask - t_lut;
        s_cycles_reverse += t_reverse - t_mask;
        ++s_measured_lines;

        lq_commit(lq);
        mark_ready(l);
    }
    if (thread_id == 0 && s_measured_lines > 0 && s_measure_frames < 2) {
        ++s_measure_frames;
        const uint32_t lines = s_measured_lines;
        ESP_LOGI("epd_feed", "line path: lut=%u mask=%u reverse=%u cycles/line over %u lines "
                 "(full_mask=%d panel_order_lut=%d)",
                 (unsigned)(s_cycles_lut / lines), (unsigned)(s_cycles_mask / lines),
                 (unsigned)(s_cycles_reverse / lines), (unsigned)lines,
                 (int)mask_full, (int)ctx->lut_output_panel_order);
        s_cycles_lut = s_cycles_mask = s_cycles_reverse = 0;
        s_measured_lines = 0;
    }
}

#endif
