-- Reload under a live video tap, driven by run.sh. scene.lua names the fill
-- colour; run.sh rewrites it, the simulator's file watcher reloads the
-- project, and the frames on the FIFO show which generation is on screen.

local colour = require("scene")
local rgb = { red = { 1, 0, 0 }, blue = { 0, 0, 1 } }

display.newRect(display.contentCenterX, display.contentCenterY,
	display.actualContentWidth, display.actualContentHeight):setFillColor(unpack(rgb[colour]))

-- Something must change every frame or the display has nothing to render.
-- Keep it in a corner, away from the centre pixel the reader samples.
local spinner = display.newRect(20, 20, 16, 16)
spinner:setFillColor(1, 1, 1)
Runtime:addEventListener("enterFrame", function() spinner.rotation = spinner.rotation + 6 end)

timer.performWithDelay(300, function()
	print("[T] loaded " .. colour)
	io.stdout:flush()
end)
