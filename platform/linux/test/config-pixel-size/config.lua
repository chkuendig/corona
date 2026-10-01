-- Prints what config.lua sees; run.sh pins the window through app.conf.
local w = display and display.pixelWidth
local h = display and display.pixelHeight
print(string.format("[T] config pixel=%sx%s", tostring(w), tostring(h)))
io.stdout:flush()

application =
{
	content = { width = 320, height = 480, scale = "letterbox" },
}
