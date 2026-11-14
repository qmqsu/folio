#include <stdbool.h>
#include <stdlib.h>
extern unsigned wallpaper_cancel_at,wallpaper_cancel_calls,wallpaper_crash_at;
bool folio_io_cancelled(void) {
    unsigned call=++wallpaper_cancel_calls;
    if(wallpaper_crash_at && call==wallpaper_crash_at)_Exit(75);
    return wallpaper_cancel_at && call>=wallpaper_cancel_at;
}
