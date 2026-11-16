/* SPDX-License-Identifier: Apache-2.0
 * Main-task-only incremental title/section discovery. The first TOC frame is
 * shown before any cold chapter scan; a visible row retains its drawn target. */
static size_t reader_toc_current(void) {
    size_t count=reader_toc_count();
    if(!count)return SIZE_MAX;
    if(!app.epub_open) {
        size_t index=current_page()/10;return index<count?index:count-1;
    }
    if(!app.epub.section_toc_count)return app.chapter_index<count?app.chapter_index:count-1;
    /* Match page-start position, including subheadings within one chapter.
       Equal anchors prefer the deepest entry so only one card is selected. */
    size_t best=SIZE_MAX;
    for(size_t i=0;i<count;++i) {
        const thingsos_toc_entry_t* entry=&app.epub.section_toc[i];
        if(entry->chapter>app.chapter_index || (entry->chapter==app.chapter_index&&entry->offset>app.start))continue;
        if(best==SIZE_MAX){best=i;continue;}
        const thingsos_toc_entry_t* prior=&app.epub.section_toc[best];
        if(entry->chapter>prior->chapter || (entry->chapter==prior->chapter&&
            (entry->offset>prior->offset || (entry->offset==prior->offset&&entry->level>=prior->level))))best=i;
    }
    return best;
}
static size_t reader_toc_center_start(void) {
    size_t count=reader_toc_count(),current=reader_toc_current();
    if(count<=6 || current==SIZE_MAX)return 0;
    size_t start=current>3?current-3:0;
    return start<count-6?start:count-6;
}
static void reader_toc_relocate(thingsos_toc_entry_t anchor) {
    if(app.toc_follow_current){app.list_start=reader_toc_center_start();return;}
    /* A manual scroll owns the viewport even when new sections are inserted. */
    size_t fallback=SIZE_MAX;bool same_level=false;
    for(size_t n=0;n<reader_toc_count();++n) {
        thingsos_toc_entry_t candidate=reader_toc_entry(n);
        if(candidate.chapter!=anchor.chapter || candidate.offset!=anchor.offset)continue;
        if(candidate.level==anchor.level&&!strcmp(candidate.title,anchor.title)){app.list_start=n;return;}
        if(fallback==SIZE_MAX || (!same_level&&candidate.level==anchor.level)) {
            fallback=n;same_level=candidate.level==anchor.level;
        }
    }
    if(fallback!=SIZE_MAX)app.list_start=fallback;
}
static void reader_toc_poll(void) {
    if(!app.toc_pending || !app.opened || !app.epub_open || app.page!=THINGSOS_UI_CHAPTERS ||
       app.dirty || now()<app.activity_at+500 || thingsos_weidu_busy() || thingsos_network_busy())return;
    thingsos_toc_entry_t anchor=reader_toc_entry(app.list_start);
    bool done=thingsos_epub_titles_step(&app.epub);
    if(done && app.wd_id[0]) {
        if(!thingsos_wd_navigation_step(WD_ROOT,app.wd_id,&app.epub,&done))done=true;
    }
    reader_toc_relocate(anchor);
    app.toc_pending=!done;
    bool changed=app.toc_draw_total!=reader_toc_count() || app.toc_draw_start!=app.list_start ||
        app.toc_draw_current!=reader_toc_current();
    for(size_t i=0;!changed && i<app.toc_draw_count;++i) {
        thingsos_toc_entry_t entry=reader_toc_entry(app.list_start+i);
        changed=entry.chapter!=app.toc_draw_entries[i].chapter || entry.offset!=app.toc_draw_entries[i].offset ||
            entry.level!=app.toc_draw_entries[i].level || strcmp(entry.title,app.toc_draw_entries[i].title);
    }
    if(changed && (done || now()-app.toc_redraw_at>=1000)) {
        app.toc_redraw_at=now();app.dirty=true;
    }
}
