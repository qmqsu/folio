#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
static inline int folio_clock_days(int year,int month) {
    static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(month<1||month>12)return 0;
    return days[month-1]+(month==2 && year%4==0 && (year%100!=0||year%400==0));
}
static inline bool folio_clock_to_utc(const struct tm* t,int timezone,uint32_t* utc) {
    int y=t->tm_year+1900,m=t->tm_mon+1;
    if(y<2024||y>2099||m<1||m>12||t->tm_mday<1||t->tm_mday>folio_clock_days(y,m)||
       t->tm_hour<0||t->tm_hour>23||t->tm_min<0||t->tm_min>59||timezone< -720||timezone>840)return false;
    int64_t days=0;
    for(int year=1970;year<y;++year)days+=365+(folio_clock_days(year,2)==29);
    for(int month=1;month<m;++month)days+=folio_clock_days(y,month);
    days+=t->tm_mday-1;
    int64_t seconds=days*86400+t->tm_hour*3600+t->tm_min*60-(int64_t)timezone*60;
    if(seconds<1704067200LL||seconds>4102444799LL)return false;
    *utc=(uint32_t)seconds;return true;
}
