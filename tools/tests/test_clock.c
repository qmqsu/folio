#include <assert.h>
#include <stdio.h>
#include "../../main/folio_clock.h"
int main(void) {
    assert(folio_clock_days(2024,2)==29 && folio_clock_days(2025,2)==28);
    assert(folio_clock_days(2100,2)==28 && folio_clock_days(2000,2)==29);
    struct tm t={.tm_year=126,.tm_mon=10,.tm_mday=1,.tm_hour=12,.tm_min=30};
    uint32_t utc,china;assert(folio_clock_to_utc(&t,0,&utc));
    assert(folio_clock_to_utc(&t,480,&china) && utc-china==8*3600);
    t=(struct tm){.tm_year=124,.tm_mon=1,.tm_mday=29,.tm_hour=23,.tm_min=59};
    assert(folio_clock_to_utc(&t,0,&utc) && utc==1709251140);
    t.tm_year=125;assert(!folio_clock_to_utc(&t,0,&utc));
    t.tm_mday=28;t.tm_hour=24;assert(!folio_clock_to_utc(&t,0,&utc));
    t.tm_hour=23;t.tm_min=60;assert(!folio_clock_to_utc(&t,0,&utc));
    t.tm_min=59;assert(!folio_clock_to_utc(&t,841,&utc));
    t=(struct tm){.tm_year=124,.tm_mon=0,.tm_mday=1};
    assert(folio_clock_to_utc(&t,0,&utc) && utc==1704067200);
    assert(!folio_clock_to_utc(&t,480,&utc));
    puts("Clock conversion: leap dates, timezone offsets, invalid fields and RTC bounds passed.");
}
