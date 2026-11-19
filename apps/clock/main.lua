-- Clock: the local time in big digits with the date underneath, redrawn every minute.
-- A quick partial refresh each minute, and a full one on the hour (clears ghosting) or when
-- the time zone changed (the first forecast after power-on can correct it).
--
-- Also the reference example for the app API: see docs/api.md.

local DAYS = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"}
local MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}

local TIME_SCALE = 7    -- the 5x7 font at 7x: 35x49 px digits
local TIME_Y = 22       -- top of the digits
local DATE_Y = 84       -- top of the date line (2x text)

local drawn_zone        -- time zone the screen was drawn with

-- "5:05" + "PM", or "17:05" + "" with the 24-hour setting
local function time_texts(t)
  if settings.get("h24") then
    return string.format("%02d:%02d", t.hour, t.min), ""
  end
  local h = t.hour % 12
  if h == 0 then h = 12 end
  return string.format("%d:%02d", h, t.min), t.hour < 12 and "AM" or "PM"
end

local function draw(full)
  local t = time.date()
  local clock, ampm, date
  if t then
    clock, ampm = time_texts(t)
    date = string.format("%s %s %d", DAYS[t.wday], MONTHS[t.month], t.day)
  else
    clock, ampm, date = "--:--", "", "Waiting for time"
  end

  screen.begin(full)
  -- Big time with AM/PM to its right, centered together as one group
  local time_w = screen.text_width(clock, TIME_SCALE)
  local ampm_w = ampm ~= "" and 4 + screen.text_width(ampm, 2) or 0
  local x = (screen.width - time_w - ampm_w) // 2
  screen.text(x, TIME_Y, clock, TIME_SCALE)
  if ampm ~= "" then
    screen.text(x + time_w + 4, TIME_Y, ampm, 2)    -- top-aligned with the digits
  end
  screen.text((screen.width - screen.text_width(date, 2)) // 2, DATE_Y, date, 2)
  screen.finish()

  drawn_zone = time.zone()
  app.wake_in((60 - time.now() % 60) * 1000 + 200)  -- just after the next minute starts
end

function on_open()
  draw(true)
end

function on_tick()
  local t = time.date()
  draw(time.zone() ~= drawn_zone or (t ~= nil and t.min == 0))
end

-- No on_button: BACK leaves, the other buttons do nothing
