-- Native text field input on the Linux simulator, driven through the input
-- FIFO by run.sh. Every observation is one "[T] ..." line on stdout; run.sh
-- asserts on them.

local function log(...)
	print("[T] " .. string.format(...))
	io.stdout:flush()
end

display.newRect(display.contentCenterX, display.contentCenterY, 320, 480):setFillColor(0.2)

-- A plain field: .text must read back what was typed.
local email = native.newTextField(160, 80, 240, 40)
email:addEventListener("userInput", function(event)
	if event.phase == "editing" then
		log("email editing text=%s read=%s new=%s", event.text, event.target.text, event.newCharacters)
	else
		log("email %s read=%s", event.phase, event.target.text)
	end
end)

-- A field whose handler rewrites its own text, the usual shape of a numeric
-- code input: keep digits, at most 6.
local code = native.newTextField(160, 160, 240, 40)
code:addEventListener("userInput", function(event)
	if event.phase == "editing" then
		local cleaned = event.target.text:gsub("%D", ""):sub(1, 6)
		if cleaned ~= event.target.text then
			event.target.text = cleaned
		end
		log("code editing read=%s", event.target.text)
	else
		log("code %s read=%s", event.phase, event.target.text)
	end
end)

-- A field for text longer than one SDL_TEXTINPUT event (31 bytes), including
-- multi-byte characters that straddle where a naive cut would fall.
local long = native.newTextField(160, 240, 240, 40)
long:addEventListener("userInput", function(event)
	if event.phase == "editing" then
		log("long editing new=%s", event.newCharacters)
	end
end)

-- Mouse listeners must see the injected button as pressed on the press and
-- through a drag, like a real mouse.
Runtime:addEventListener("mouse", function(event)
	if event.type ~= "move" then
		log("mouse %s primary=%s", event.type, tostring(event.isPrimaryButtonDown))
	end
end)

-- An ordinary display object below the fields: the first tap after a field
-- must reach it.
local button = display.newRect(160, 360, 200, 60)
button:setFillColor(0, 0.5, 1)
button:addEventListener("touch", function(event)
	log("button %s", event.phase)
	if event.phase == "ended" then
		log("final email=%s code=%s", email.text, code.text)
	end
	return true
end)

timer.performWithDelay(500, function() log("ready") end)
