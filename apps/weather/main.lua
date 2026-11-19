-- Weather: current conditions and a 7-day forecast from Open-Meteo (https://open-meteo.com,
-- free, no key), with a details screen per day. Every forecast also sets the board's time
-- zone, so this app runs in the background ("background": true in app.json).
--
-- Screens:
--   FORECAST  today on the left, the next 6 days in a 3x2 grid. Updates on its own while
--             showing: a new forecast with a FULL refresh every `refresh_minutes`, new current
--             conditions with a PARTIAL refresh every `partial_seconds`.
--             left/right highlight a day, press opens its details, BACK leaves.
--   DAY       details for one day: stats on the left, hourly graph on the right.
--             left/right step between days, BACK returns to the forecast.

local W, H = screen.width, screen.height
local DAYS = 7

-- Forecast layout (pixels)
local PANEL_W = 75                          -- the "today" panel on the left
local PANEL_CX = PANEL_W // 2
local GRID_X0 = 76                          -- left edge of the day grid
local GRID_COLS = 3
local CELL_W = (W - GRID_X0) // GRID_COLS   -- 73
local CELL_H = H // 2                       -- 64

-- Day details layout
local DETAIL_COL_W = 104                    -- the text column
local GRAPH_X0 = 128                        -- x of hour 0
local GRAPH_STEP = 5                        -- px per hour
local GRAPH_TOP = 34                        -- y of the warmest hour
local GRAPH_BOTTOM = 84                     -- y of the coldest hour
local BARS_BOTTOM = 104                     -- bottom of the rain-chance bars
local BARS_H = 14                           -- a 100% bar is this tall

-- Settings
local METRIC = settings.get("units") == "metric"
local WIND_UNIT = METRIC and "kmh" or "mph"
local PRECIP_FMT = METRIC and "%.1fmm" or "%.2fin"
local FULL_MS = settings.get("refresh_minutes") * 60 * 1000
local PARTIAL_MS = settings.get("partial_seconds") * 1000
local FETCH_MS = 60 * 1000                  -- current conditions at most once a minute
local RETRY_MS = 5 * 60 * 1000              -- after a failed full update
local FIRST_RETRY_MS = 30 * 1000            -- no forecast at all yet

-- ---------- small helpers ----------

-- Round half away from zero, like C's lround
local function round(x)
  if x >= 0 then return math.floor(x + 0.5) end
  return -math.floor(-x + 0.5)
end

local function text_centered(cx, y, s, scale)
  screen.text(cx - screen.text_width(s, scale) // 2, y, s, scale)
end

local function hline(x, y, w) screen.rect(x, y, w, 1, true) end

-- Day of week for a date: 0 = Sunday ... 6 = Saturday (Sakamoto's algorithm)
local OFFSET = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4}
local function day_of_week(y, m, d)
  if m < 3 then y = y - 1 end
  return (y + y // 4 - y // 100 + y // 400 + OFFSET[m] + d) % 7
end

-- ---------- icons (each fits roughly 24x24 around cx, cy) ----------

-- WMO weather code -> icon name
local function icon_for_code(code)
  if code == 0 then return "sun" end
  if code <= 2 then return "partly" end
  if code == 3 then return "cloud" end
  if code == 45 or code == 48 then return "fog" end
  if code >= 51 and code <= 67 then return "rain" end
  if code >= 71 and code <= 77 then return "snow" end
  if code >= 80 and code <= 82 then return "rain" end
  if code == 85 or code == 86 then return "snow" end
  if code >= 95 then return "storm" end
  return "cloud"
end

local function draw_sun(cx, cy, r)
  screen.circle(cx, cy, r)
  for i = 0, 7 do                           -- 8 rays, 45 degrees apart
    local a = i * math.pi / 4
    screen.line(cx + round(math.cos(a) * (r + 3)), cy + round(math.sin(a) * (r + 3)),
                cx + round(math.cos(a) * (r + 5)), cy + round(math.sin(a) * (r + 5)))
  end
end

-- Outlined cloud: three overlapping circles on a flat base, drawn black then 2 px smaller in white
local function draw_cloud(cx, cy)
  screen.circle(cx - 5, cy + 1, 5)
  screen.circle(cx + 2, cy - 2, 6)
  screen.circle(cx + 7, cy + 2, 4)
  screen.rect(cx - 5, cy + 1, 13, 6, true)
  screen.circle(cx - 5, cy + 1, 3, screen.WHITE)
  screen.circle(cx + 2, cy - 2, 4, screen.WHITE)
  screen.circle(cx + 7, cy + 2, 2, screen.WHITE)
  screen.rect(cx - 5, cy + 1, 13, 4, true, screen.WHITE)
end

local function thick_line(x0, y0, x1, y1)
  screen.line(x0, y0, x1, y1)
  screen.line(x0 + 1, y0, x1 + 1, y1)
end

local function draw_icon(cx, cy, icon)
  if icon == "sun" then
    draw_sun(cx, cy, 6)
  elseif icon == "partly" then
    draw_sun(cx - 5, cy - 4, 4)
    draw_cloud(cx + 1, cy + 4)
  elseif icon == "cloud" then
    draw_cloud(cx - 1, cy)
  elseif icon == "fog" then
    for i = 0, 3 do                         -- four staggered bars
      local shift = (i % 2 == 1) and 2 or -2
      screen.rect(cx - 9 + shift, cy - 7 + i * 5, 18, 2, true)
    end
  elseif icon == "rain" then
    draw_cloud(cx - 1, cy - 5)
    for i = -1, 1 do thick_line(cx + i * 6 + 1, cy + 5, cx + i * 6 - 2, cy + 11) end
  elseif icon == "snow" then
    draw_cloud(cx - 1, cy - 5)
    for i = -1, 1 do                        -- three small "+" flakes, the middle one lower
      local fx, fy = cx + i * 6, cy + (i == 0 and 10 or 7)
      hline(fx - 2, fy, 5)
      screen.line(fx, fy - 2, fx, fy + 2)
    end
  elseif icon == "storm" then
    draw_cloud(cx - 1, cy - 5)
    thick_line(cx + 1, cy + 3, cx - 3, cy + 8)
    thick_line(cx - 3, cy + 8, cx + 1, cy + 8)
    thick_line(cx + 1, cy + 8, cx - 3, cy + 13)
  end
end

-- 1 px line drawn three times (right and down copies) so it reads in any direction
local function bold_line(x0, y0, x1, y1)
  screen.line(x0, y0, x1, y1)
  screen.line(x0 + 1, y0, x1 + 1, y1)
  screen.line(x0, y0 + 1, x1, y1 + 1)
end

-- Arrow pointing the way the wind blows (up = north). from_deg: where it comes FROM.
local function draw_wind_arrow(cx, cy, from_deg, len)
  local to = (from_deg + 180) * math.pi / 180
  local dx, dy = math.sin(to), -math.cos(to)
  local tip_x, tip_y = cx + round(dx * len), cy + round(dy * len)
  local tail_x, tail_y = cx - round(dx * len), cy - round(dy * len)
  bold_line(tail_x, tail_y, tip_x, tip_y)
  for side = -1, 1, 2 do                    -- the two barbs of the head
    local a = to + math.pi + side * 0.6
    bold_line(tip_x, tip_y, tip_x + round(math.sin(a) * 4), tip_y - round(math.cos(a) * 4))
  end
end

local function draw_droplet(x, y)           -- 5x7, top-left at (x, y)
  screen.circle(x + 2, y + 5, 2)
  screen.line(x + 2, y + 1, x + 1, y + 3)
  screen.line(x + 2, y + 1, x + 3, y + 3)
  screen.pixel(x + 2, y + 2)
end

local function draw_degree(x, y)            -- 5x5 ring, top-left at (x, y)
  screen.circle(x + 2, y + 2, 2)
  screen.pixel(x + 2, y + 2, screen.WHITE)
end

local function dotted_hline(x, y, w)
  for i = 0, w - 1, 2 do screen.pixel(x + i, y) end
end

local function dotted_vline(x, y, h)
  for i = 0, h - 1, 2 do screen.pixel(x, y + i) end
end

-- A number centered on cx at 2x, or 1x if it's wider than max_w
local function number_centered(cx, y, value, max_w)
  local s = tostring(value)
  local scale = screen.text_width(s, 2) <= max_w and 2 or 1
  text_centered(cx, y + (scale == 2 and 0 or 4), s, scale)
end

-- Label strip at the top of a day area; highlighted = white text on black
local function day_label(x, y, w, s, highlighted)
  local tx = x + w // 2 - screen.text_width(s, 1) // 2
  if highlighted then
    screen.rect(x, y, w, 11, true)
    screen.text(tx, y + 2, s, 1, screen.WHITE)
  else
    screen.text(tx, y + 2, s, 1)
  end
end

-- ---------- the forecast screen ----------

local DAY3 = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"}
local DAY_FULL = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}

-- Local time of day at the forecast's location, "HH:MM", or nil before the clock is set
local function local_hhmm(w)
  if not time.synced() then return nil end
  local t = (time.now() + w.utc_offset) % 86400
  return string.format("%02d:%02d", t // 3600, t % 3600 // 60)
end

local function draw_today_panel(w, highlighted)
  local today = w.days[1]
  day_label(0, 1, PANEL_W, string.format("Today %02d/%02d", today.month, today.day), highlighted)

  draw_icon(15, 28, icon_for_code(w.current_code))      -- the weather right now
  local s = tostring(w.current_temp)
  local scale = screen.text_width(s, 3) <= 36 and 3 or 2
  local ty = scale == 3 and 18 or 22
  draw_degree(screen.text(31, ty, s, scale), ty)

  text_centered(PANEL_CX, 48, string.format("Hi %d Lo %d", today.temp_high, today.temp_low), 1)

  s = string.format("%d%% rain", today.rain_chance)
  local left = PANEL_CX - (7 + screen.text_width(s, 1)) // 2
  draw_droplet(left, 61)
  screen.text(left + 7, 61, s, 1)

  dotted_hline(4, 74, PANEL_W - 8)
  text_centered(PANEL_CX, 80, "As of " .. (local_hhmm(w) or "--:--"), 1)

  -- Current wind: arrow + big speed + unit, centered as one group
  s = tostring(w.current_wind)
  local group_w = 14 + screen.text_width(s, 2) + 2 + screen.text_width(WIND_UNIT, 1)
  left = PANEL_CX - group_w // 2
  draw_wind_arrow(left + 5, 100, w.current_wind_dir, 5)
  screen.text(screen.text(left + 14, 94, s, 2), 101, WIND_UNIT, 1)
  text_centered(PANEL_CX, 112, string.format("Max %d%s", today.wind_max, WIND_UNIT), 1)
end

-- One grid cell, top-left at (x0, y0)
local function draw_day_cell(x0, y0, d, highlighted)
  day_label(x0, y0 + 1, CELL_W - 1, string.format("%s %02d/%02d",
            DAY3[day_of_week(d.year, d.month, d.day) + 1], d.month, d.day), highlighted)
  x0 = x0 + (CELL_W - 58) // 2              -- the rest is laid out for 58 px: center it
  draw_icon(x0 + 14, y0 + 27, icon_for_code(d.weather_code))
  number_centered(x0 + 43, y0 + 13, d.temp_high, 28)
  hline(x0 + 39, y0 + 29, 9)
  number_centered(x0 + 43, y0 + 32, d.temp_low, 28)
  draw_droplet(x0 + 2, y0 + 51)
  screen.text(x0 + 9, y0 + 51, string.format("%d%%", d.rain_chance), 1)
  draw_wind_arrow(x0 + 40, y0 + 54, d.wind_dir, 4)
  screen.text(x0 + 47, y0 + 51, tostring(d.wind_max), 1)
end

local weather                               -- latest good forecast
local hover = -1                            -- highlighted day (0 = today ... 6), -1 = none
local in_day = false                        -- the details screen is showing

local function show(full)
  screen.begin(full)
  draw_today_panel(weather, hover == 0)
  screen.rect(PANEL_W, 0, 1, H, true)
  dotted_hline(GRID_X0, CELL_H - 1, W - GRID_X0)
  for c = 1, GRID_COLS - 1 do dotted_vline(GRID_X0 + c * CELL_W - 1, 0, H) end
  for i = 1, DAYS - 1 do                    -- tomorrow onward, left to right, top row first
    local cell = i - 1
    draw_day_cell(GRID_X0 + (cell % GRID_COLS) * CELL_W, (cell // GRID_COLS) * CELL_H,
                  weather.days[i + 1], hover == i)
  end
  screen.finish()
end

local function show_error(title, detail)
  screen.begin(true)
  screen.rect(0, 0, W, H, false)
  text_centered(W // 2, 30, title, 2)
  text_centered(W // 2, 64, detail, 1)
  screen.finish()
end

-- ---------- the day details screen ----------

local CONDITIONS = {
  [0] = "Clear", [1] = "Mostly clear", [2] = "Partly cloudy", [3] = "Overcast",
  [45] = "Fog", [48] = "Fog", [51] = "Drizzle", [53] = "Drizzle", [55] = "Drizzle",
  [56] = "Freezing drizzle", [57] = "Freezing drizzle", [61] = "Light rain", [63] = "Rain",
  [65] = "Heavy rain", [66] = "Freezing rain", [67] = "Freezing rain", [71] = "Light snow",
  [73] = "Snow", [75] = "Heavy snow", [77] = "Snow grains", [80] = "Light showers",
  [81] = "Showers", [82] = "Heavy showers", [85] = "Snow showers", [86] = "Snow showers",
  [95] = "Thunderstorm", [96] = "T-storm w/ hail", [99] = "T-storm w/ hail",
}

local COMPASS = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}
local function compass(deg)
  return COMPASS[((deg % 360 + 360 + 22) // 45) % 8 + 1]
end

local function draw_hourly_graph(day)
  local d = weather.days[day + 1]
  local tmin, tmax = d.hourly_temp[1], d.hourly_temp[1]
  for h = 2, 24 do
    tmin = math.min(tmin, d.hourly_temp[h])
    tmax = math.max(tmax, d.hourly_temp[h])
  end
  local range = tmax - tmin > 0 and tmax - tmin or 1

  screen.text(W - 3 - screen.text_width("Hourly", 1), 23, "Hourly", 1)
  screen.text(108, GRAPH_TOP - 3, tostring(tmax), 1)
  screen.text(108, GRAPH_BOTTOM - 4, tostring(tmin), 1)
  draw_droplet(112, BARS_BOTTOM - 8)        -- marks the row of rain bars

  for h = 6, 18, 6 do dotted_vline(GRAPH_X0 + h * GRAPH_STEP, GRAPH_TOP, BARS_BOTTOM - GRAPH_TOP) end
  local labels = {"12a", "6a", "12p", "6p"}
  for i = 0, 3 do
    local x = GRAPH_X0 + i * 6 * GRAPH_STEP
    screen.text(x - screen.text_width(labels[i + 1], 1) // 2, BARS_BOTTOM + 4, labels[i + 1], 1)
  end

  for h = 0, 23 do                          -- rain chance per hour
    local bar_h = d.hourly_rain[h + 1] * BARS_H // 100
    if bar_h > 0 then
      screen.rect(GRAPH_X0 + h * GRAPH_STEP - 2, BARS_BOTTOM - bar_h, 4, bar_h, true)
    end
  end
  hline(GRAPH_X0 - 2, BARS_BOTTOM, 23 * GRAPH_STEP + 5)

  for h = 0, 22 do                          -- temperature line, hour to hour
    local y0 = GRAPH_BOTTOM - (d.hourly_temp[h + 1] - tmin) * (GRAPH_BOTTOM - GRAPH_TOP) // range
    local y1 = GRAPH_BOTTOM - (d.hourly_temp[h + 2] - tmin) * (GRAPH_BOTTOM - GRAPH_TOP) // range
    bold_line(GRAPH_X0 + h * GRAPH_STEP, y0, GRAPH_X0 + (h + 1) * GRAPH_STEP, y1)
  end

  if day == 0 and time.synced() then        -- today: a small triangle at the current hour
    local x = GRAPH_X0 + ((time.now() + weather.utc_offset) % 86400) // 3600 * GRAPH_STEP
    for i = 0, 3 do hline(x - 3 + i, GRAPH_TOP - 7 + i, 7 - 2 * i) end
  end
end

local function show_day(day, full)
  local d = weather.days[day + 1]
  screen.begin(full)
  screen.text(4, 3, string.format("%s %02d/%02d",
              day == 0 and "Today" or DAY_FULL[day_of_week(d.year, d.month, d.day) + 1], d.month, d.day), 2)
  local counter = string.format("%d/%d", day + 1, DAYS)
  screen.text(W - 3 - screen.text_width(counter, 1), 7, counter, 1)
  hline(0, 20, W)
  screen.rect(DETAIL_COL_W + 1, 21, 1, H - 21, true)

  draw_icon(16, 36, icon_for_code(d.weather_code))
  local s = tostring(d.temp_high)
  local scale = screen.text_width(s, 3) <= 33 and 3 or 2
  local y = scale == 3 and 25 or 30
  local x = screen.text(34, y, s, scale)
  draw_degree(x, y)
  s = tostring(d.temp_low)                  -- the low, bottom-aligned with the high
  local low_scale = x + 7 + screen.text_width(s, 2) <= DETAIL_COL_W and 2 or 1
  screen.text(x + 7, low_scale == 2 and 32 or 38, s, low_scale)

  screen.text(3, 52, CONDITIONS[d.weather_code] or "", 1)
  screen.text(3, 63, string.format("Rain %d%% " .. PRECIP_FMT, d.rain_chance, d.precip_sum), 1)
  screen.text(3, 74, string.format("Wind %d%s %s", d.wind_max, WIND_UNIT, compass(d.wind_dir)), 1)
  screen.text(3, 85, string.format("Gust %d%s UV %d", d.gust_max, WIND_UNIT, d.uv_max), 1)
  screen.text(3, 96, string.format("Sun %s-%s", d.sunrise, d.sunset), 1)
  screen.text(3, 107, string.format("Feels %d/%d", d.feels_high, d.feels_low), 1)

  draw_hourly_graph(day)
  screen.finish()
end

-- ---------- downloading ----------

local function location()
  local lat, lon = tonumber(settings.get("latitude")), tonumber(settings.get("longitude"))
  if lat and lon and lat >= -90 and lat <= 90 and lon >= -180 and lon <= 180 then return lat, lon end
end

local function forecast_url()
  local lat, lon = location()
  local units = METRIC and "&temperature_unit=celsius&wind_speed_unit=kmh&precipitation_unit=mm"
                        or "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch"
  return "https://api.open-meteo.com/v1/forecast" ..
    "?latitude=" .. lat .. "&longitude=" .. lon ..
    "&daily=weather_code,temperature_2m_max,temperature_2m_min," ..
    "precipitation_probability_max,wind_speed_10m_max,wind_direction_10m_dominant," ..
    "apparent_temperature_max,apparent_temperature_min,precipitation_sum," ..
    "wind_gusts_10m_max,uv_index_max,sunrise,sunset" ..
    "&hourly=temperature_2m,precipitation_probability" ..
    "&current=temperature_2m,weather_code,wind_speed_10m,wind_direction_10m" ..
    units .. "&timezone=auto&forecast_days=" .. DAYS
end

local function int(v) return type(v) == "number" and round(v) or 0 end   -- missing/null -> 0
local function at(arr, i) return type(arr) == "table" and arr[i] or nil end
local function hhmm(s) return type(s) == "string" and s:match("T(%d%d:%d%d)") or "--:--" end

-- Open-Meteo's JSON -> our forecast table, or nil + why
local function parse(body)
  local ok, root = pcall(json.decode, body)
  if not ok or type(root) ~= "table" then return nil, "not valid JSON" end
  local current, daily = root.current, root.daily
  if type(current) ~= "table" or type(daily) ~= "table" then return nil, "no current or daily data" end
  if type(daily.time) ~= "table" or #daily.time < DAYS then return nil, "fewer than 7 days" end
  local hourly = type(root.hourly) == "table" and root.hourly or {}
  local w = {
    utc_offset = int(root.utc_offset_seconds),
    current_temp = int(current.temperature_2m),
    current_code = int(current.weather_code),
    current_wind = int(current.wind_speed_10m),
    current_wind_dir = int(current.wind_direction_10m),
    days = {},
  }
  for i = 1, DAYS do
    local y, m, dd = tostring(daily.time[i]):match("^(%d+)-(%d+)-(%d+)")
    if not y then return nil, "unexpected date format" end
    local d = {
      year = tonumber(y), month = tonumber(m), day = tonumber(dd),
      weather_code = int(at(daily.weather_code, i)),
      temp_high = int(at(daily.temperature_2m_max, i)),
      temp_low = int(at(daily.temperature_2m_min, i)),
      rain_chance = int(at(daily.precipitation_probability_max, i)),
      wind_max = int(at(daily.wind_speed_10m_max, i)),
      wind_dir = int(at(daily.wind_direction_10m_dominant, i)),
      feels_high = int(at(daily.apparent_temperature_max, i)),
      feels_low = int(at(daily.apparent_temperature_min, i)),
      precip_sum = tonumber(at(daily.precipitation_sum, i)) or 0,
      gust_max = int(at(daily.wind_gusts_10m_max, i)),
      uv_max = int(at(daily.uv_index_max, i)),
      sunrise = hhmm(at(daily.sunrise, i)),
      sunset = hhmm(at(daily.sunset, i)),
      hourly_temp = {}, hourly_rain = {},
    }
    for h = 1, 24 do                        -- this day's slice of the hourly arrays
      d.hourly_temp[h] = int(at(hourly.temperature_2m, (i - 1) * 24 + h))
      d.hourly_rain[h] = int(at(hourly.precipitation_probability, (i - 1) * 24 + h))
    end
    w.days[i] = d
  end
  return w
end

local have = false                          -- a forecast has arrived
local fetching = false                      -- a download is in flight
local full_pending = false                  -- ...and its arrival needs a full refresh
local requested = false                     -- a download was started at least once
local last_full, last_fetch = 0, 0          -- time.uptime() of the last full / any download

local function on_download(status, body)
  fetching = false
  local full = full_pending or not have     -- the first forecast always gets a full draw
  local on_forecast = app.showing() and not in_day
  full_pending = false

  local w, why
  if status == 200 then w, why = parse(body) else why = status and ("HTTP " .. status) or body end
  if not w then
    log("download failed: " .. tostring(why))
    if have and full then
      last_full = time.uptime() - FULL_MS + RETRY_MS    -- try the full update again in 5 minutes
    elseif not have and on_forecast then
      show_error("No forecast", "Weather service error, retrying")
    end
    return
  end

  weather, have = w, true
  if full then last_full = time.uptime() end
  time.set_zone(w.utc_offset)               -- every app's time zone
  if on_forecast then show(full) end
end

-- Starts a download. full = a full refresh when it lands.
local function request(full)
  requested = true
  if fetching then
    if full then full_pending = true end    -- already on its way: make it a full update
    return
  end
  if not location() then return end
  local ok, why = net.get(forecast_url(), nil, on_download)
  if ok then
    fetching, full_pending, last_fetch = true, full, time.uptime()
  else
    log("couldn't start the download: " .. tostring(why))
  end
end

-- ---------- screens and callbacks ----------

local function show_forecast()
  in_day = false
  app.wake_in(PARTIAL_MS)
  if not location() then
    show_error("Set your location", "Weather settings, on the board's web page")
    return
  end
  if not have then
    show_error("Weather", "Downloading the forecast...")
    request(true)                           -- drawn when it arrives
    return
  end
  show(true)                                -- what we have, right away
  local now = time.uptime()
  if now - last_full >= FULL_MS then
    request(true)                           -- the forecast is due anyway
  elseif now - last_fetch >= FETCH_MS then
    request(false)                          -- current conditions are stale
  end
end

function on_open()
  hover = -1
  show_forecast()
end

function on_button(b)
  if b == "back" then
    if not in_day then return false end     -- leave the app
    ui.loading()
    show_forecast()                         -- the day we came from stays highlighted
    return true
  end
  if not have then return true end

  if in_day then
    if b == "left" or b == "right" then
      hover = b == "left" and (hover + DAYS - 1) % DAYS or (hover + 1) % DAYS
      show_day(hover, false)
    end
  elseif b == "left" or b == "right" then
    if hover < 0 then
      hover = 0                             -- first press: today
    elseif b == "left" then
      hover = (hover + DAYS - 1) % DAYS
    else
      hover = (hover + 1) % DAYS
    end
    show(false)
  elseif b == "select" then
    if hover < 0 then hover = 0 end
    ui.loading()
    in_day = true
    app.wake_in(-1)                         -- the details screen doesn't update on its own
    show_day(hover, true)
  end
  return true
end

-- Scheduled update while the forecast is on screen
function on_tick()
  if in_day then return end
  app.wake_in(PARTIAL_MS)
  local now = time.uptime()
  if not have or now - last_full >= FULL_MS then
    request(true)
  elseif now - last_fetch >= FETCH_MS then
    request(false)
  else
    show(false)                             -- just redraw (updates "As of")
  end
end

-- Every 30 s, open or not: get a first forecast as soon as possible (other apps need the
-- time zone), then keep it fresh so the time zone follows daylight saving changes
function on_background()
  if fetching or app.showing() then return end
  local now = time.uptime()
  if not have then
    if not requested or now >= last_fetch + FIRST_RETRY_MS then request(true) end
  elseif now - last_full >= FULL_MS then
    request(true)
  end
end
