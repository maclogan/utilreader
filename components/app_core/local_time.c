#include "local_time.h"

#include "settings.h"                   // remembering the time zone

#define CLOCK_SET_AFTER 1700000000      // time() is below this until internet time has synced (Nov 2023)
#define KEY             "utc_off"

static int offset = 0;
static bool offset_known = false;

void local_time_init(void)
{
    const int32_t unset = INT32_MIN;
    int32_t saved = settings_get_i32(KEY, unset);
    if (saved != unset) {
        offset = saved;
        offset_known = true;
    }
}

bool local_time_synced(void)
{
    return time(NULL) >= CLOCK_SET_AFTER;
}

int local_time_offset(void)
{
    return offset;
}

bool local_time_offset_known(void)
{
    return offset_known;
}

void local_time_set_offset(int utc_offset)
{
    if (offset_known && offset == utc_offset) {
        return;
    }
    offset = utc_offset;
    offset_known = true;
    settings_set_i32(KEY, utc_offset);
}

bool local_time_now(struct tm *out)
{
    if (!local_time_synced()) {
        return false;
    }
    time_t now = time(NULL) + offset;                       // shift to local time...
    gmtime_r(&now, out);                                    // ...then split it like UTC
    return true;
}
