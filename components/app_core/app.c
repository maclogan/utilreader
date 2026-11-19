#include "app.h"

static const app_t *showing;            // app on screen, NULL = menu

const app_t *app_showing(void)
{
    return showing;
}

void app_set_showing(const app_t *app)
{
    showing = app;
}

bool app_is_showing(const app_t *app)
{
    return showing == app;
}

bool app_due(TickType_t deadline)
{
    return (int32_t)(xTaskGetTickCount() - deadline) >= 0;
}

TickType_t app_ticks_until(TickType_t deadline)
{
    return app_due(deadline) ? 0 : deadline - xTaskGetTickCount();
}
