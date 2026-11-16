// SPDX-License-Identifier: Apache-2.0
#include "thingsos/diagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char output[24000];static size_t used;
static bool sink(void* context,const char* text,size_t n) {
    (void)context;if(used+n>=sizeof(output))return false;memcpy(output+used,text,n);used+=n;output[used]=0;return true;
}
static bool broken(void* context,const char* text,size_t n){(void)context;(void)text;(void)n;return false;}
int main(int argc,char** argv) {
    thingsos_diag_journal_t j={0};assert(!thingsos_diag_valid(&j));
    j.boot_count=1;j.boots[0].id=7;strcpy(j.boots[0].version,"test\"\\\n");
    strcpy(j.boots[0].elf,"abc");strcpy(j.boots[0].running,"ota_0");strcpy(j.boots[0].selected,"rescue");
    for(unsigned i=0;i<100;++i)thingsos_diag_push(&j,(thingsos_diag_event_t){.boot=7,.ms=i,.kind=DIAG_PACK,.code=-1,.error=28});
    assert(j.event_count==DIAG_EVENTS && j.events[0].ms==36 && j.events[63].ms==99);
    thingsos_diag_seal(&j);assert(thingsos_diag_valid(&j));
    assert(thingsos_diag_dump(&j,sink,NULL));assert(strstr(output,"test\\\"\\\\\\u000a"));
    assert(strstr(output,"\"errno\":28") && strstr(output,"\"selected\":\"rescue\""));
    assert(!thingsos_diag_dump(&j,broken,NULL));
    if(argc>1){FILE* f=fopen(argv[1],"wb");assert(f);assert(fwrite(output,1,used,f)==used);assert(!fclose(f));}
    j.events[0].code=0;assert(!thingsos_diag_valid(&j));
    thingsos_diag_seal(&j);j.event_count=DIAG_EVENTS+1;thingsos_diag_seal(&j);assert(!thingsos_diag_valid(&j));
    j.event_count=0;memset(j.boots[0].version,'x',sizeof(j.boots[0].version));thingsos_diag_seal(&j);assert(!thingsos_diag_valid(&j));
    puts("PASS: bounded event rotation, CRC corruption, JSON escaping, sink failure, malformed record rejection");return 0;
}
