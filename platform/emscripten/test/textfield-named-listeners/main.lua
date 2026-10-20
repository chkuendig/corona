-- Real native proxies exercise the C override and the registry DisplayObject.
local json = require("json")
local failures, errors, resizeCount = 0, 0, 0
local function record(kind, data)
    data = data or {}
    data.kind = kind
    print("[ENGINE-TEST] " .. json.encode(data))
end
local function check(ok, name, data)
    if not ok then failures = failures + 1 end
    data = data or {}
    data.name, data.status = name, ok and "PASS" or "FAIL"
    record("check", data)
end
local function frames(count, callback)
    local function tick()
        count = count - 1
        if count == 0 then
            Runtime:removeEventListener("enterFrame", tick)
            callback()
        end
    end
    Runtime:addEventListener("enterFrame", tick)
end
Runtime:addEventListener("unhandledError", function(event)
    errors = errors + 1
    record("runtime-error", { message = tostring(event.errorMessage),
        stack = tostring(event.stackTrace), count = errors })
    -- Keep collecting the deliberate baseline failure through all resize steps.
    return true
end)

local function field(y, box)
    local object
    if box then
        object = native.newTextBox(160, y, 240, 48)
        object.isEditable = true
    else
        object = native.newTextField(160, y, 240, 36)
    end
    object.size = 24
    return object
end
local dead = field(90)
dead:addEventListener("userInput", function() end)
local live = field(160)
local previousScale, previousSize = display.contentScaleY, live.size
local fontMode = live.isFontSizeScaled
Runtime:addEventListener("resize", function()
    resizeCount = resizeCount + 1
    local index, oldScale, oldSize = resizeCount, previousScale, previousSize
    frames(4, function()
        local scale, size = display.contentScaleY, live.size
        local expected = oldSize * oldScale / scale
        check(scale ~= oldScale, "scale changed", { index = index,
            oldScale = oldScale, scale = scale })
        check(math.abs(size - expected) <= 1, "live font scaling",
            { index = index, size = size, expected = expected })
        check(live.isFontSizeScaled == fontMode, "font mode restored")
        previousScale, previousSize = scale, size
        record("resize", { index = index, oldScale = oldScale, scale = scale,
            size = size, errors = errors })
    end)
end)

local weak = setmetatable({}, { __mode = "v" })
local function collect()
    collectgarbage("collect")
    collectgarbage("collect")
end
local function witnessCount()
    local count = 0
    for _ in pairs(weak) do count = count + 1 end
    return count
end
local function payload(event, object)
    check(event.name == "userInput" and event.target == object, "input target/name")
    if event.phase == "editing" then
        check(type(event.text) == "string" and type(event.oldText) == "string"
            and type(event.startPosition) == "number"
            and event.newCharacters == 0 and event.numDeleted == 0, "editing payload")
    end
end
local function named()
    local object = field(230)
    local first, second, tableCount, implicit = 0, 0, 0, 0
    local function a() first = first + 1 end
    local function b() second = second + 1 end
    check(object:addEventListener("engineProbe", a) == true, "named add return")
    check(object:addEventListener("engineProbe", b) == true, "second named add")
    check(object:respondsToEvent("engineProbe"), "respondsToEvent")
    object:dispatchEvent({ name = "engineProbe" })
    check(first == 1 and second == 1, "two named listeners")
    check(object:removeEventListener("engineProbe", a) == true, "named remove return")
    object:dispatchEvent({ name = "engineProbe" })
    check(first == 1 and second == 2, "one named listener removed")
    check(object:removeEventListener("engineProbe", a) == nil, "missing remove return")
    local listener
    listener = { engineTable = function(self, event)
        check(self == listener, "table listener self")
        check(event.name == "engineTable", "table event name")
        tableCount = tableCount + 1
    end }
    object:addEventListener("engineTable", listener)
    object:dispatchEvent({ name = "engineTable" })
    object.engineImplicit = function(self) implicit = implicit + 1 end
    object:addEventListener("engineImplicit")
    object:dispatchEvent({ name = "engineImplicit" })
    check(tableCount == 1 and implicit == 1, "table and implicit listeners")
    -- Ordinary dispatch inserts duplicates; removal removes one occurrence.
    object:addEventListener("engineDuplicate", a)
    object:addEventListener("engineDuplicate", a)
    object:dispatchEvent({ name = "engineDuplicate" })
    check(first == 3, "duplicate insertion")
    object:removeEventListener("engineDuplicate", a)
    object:dispatchEvent({ name = "engineDuplicate" })
    check(first == 4, "duplicate removal")

    -- Intercept only the registry class, never a per-object method or upvalue.
    local registry = debug.getregistry()
    local class = registry.DisplayObject
    local original = class.addEventListener
    local marker = {}
    class.addEventListener = function(...)
        local args = { ... }
        check(select("#", ...) == 4 and args[1] == object
            and args[2] == "engineBridge" and args[3] == a and args[4] == marker,
            "original delegate arguments")
        return marker, nil, 17
    end
    local function pack(...) return { n = select("#", ...), ... } end
    local values = pack(pcall(object.addEventListener, object, "engineBridge", a, marker))
    class.addEventListener = original
    check(values.n == 4 and values[1] and values[2] == marker
        and values[3] == nil and values[4] == 17, "delegate result stack")
    for _ = 1, 64 do
        class.addEventListener = function() error("engine delegate probe") end
        local ok, message = pcall(object.addEventListener, object, "engineError", a)
        class.addEventListener = original
        check(not ok and tostring(message):find("engine delegate probe", 1, true),
            "delegate error stack")
        check(object:addEventListener("engineBalanced", a) == true, "stack after error")
        object:removeEventListener("engineBalanced", a)
    end
    for _, badClass in ipairs({ false, {} }) do
        registry.DisplayObject = badClass
        local ok = pcall(object.addEventListener, object, "engineMissing", a)
        registry.DisplayObject = class
        check(not ok, "missing registry delegate errors")
    end
    local finalized, tableFinalized, removed = 0, 0, 0
    local function finalize(event)
        check(event.name == "finalize" and event.target == object, "finalize target/name")
        finalized = finalized + 1
    end
    local function unwanted() removed = removed + 1 end
    object:addEventListener("finalize", finalize)
    object:addEventListener("finalize", { finalize = function(self, event)
        check(event.name == "finalize" and event.target == object, "table finalize target/name")
        tableFinalized = tableFinalized + 1
    end })
    object:addEventListener("finalize", unwanted)
    object:removeEventListener("finalize", unwanted)
    object:removeSelf()
    frames(8, function()
        check(finalized == 1 and tableFinalized == 1 and removed == 0,
            "finalize function/table/removal",
            { finalized = finalized, tableFinalized = tableFinalized, removed = removed })
        for index = 1, 4 do
            -- No app userInput, text boxes, and parent removal all need cleanup.
            local group = display.newGroup()
            local child = field(230, index % 2 == 0)
            group:insert(child)
            local count = 0
            child:addEventListener("finalize", function() count = count + 1 end)
            group:removeSelf()
            frames(8, function()
                check(count == 1 and child.size == nil, "group removal cycle",
                    { cycle = index, finalized = count })
            end)
        end
        frames(12, function() record("named-done", { failures = failures }) end)
    end)
end

local input, inputCounts, phases
local function setupInput(tableListener)
    input, inputCounts, phases = field(280), { A = 0, B = 0 }, {}
    local label = tableListener and "table-control" or "function-control"
    input.placeholder = label
    local object = input
    object:addEventListener("userInput", function() inputCounts.A = inputCounts.A + 1 end)
    local function receive(event)
        inputCounts.B = inputCounts.B + 1
        phases[event.phase] = true
        payload(event, object)
        record("input", { control = label, phase = event.phase,
            text = event.text, oldText = event.oldText,
            startPosition = event.startPosition, recipient = "B" })
    end
    local listener = receive
    if tableListener then listener = { userInput = function(self, event) receive(event) end } end
    check(object:addEventListener("userInput", listener) == nil, "userInput zero result")
    object:addEventListener("engineInterleaved", function() end)
    object:addEventListener("userInput", 42)
    check(object:removeEventListener("userInput", listener) == nil, "userInput removal scope")
    record("input-ready", { control = label })
end
local function finishInput()
    check(inputCounts.A == 0 and inputCounts.B > 0, "last valid input listener wins",
        { A = inputCounts.A, B = inputCounts.B })
    for _, phase in ipairs({ "began", "editing", "submitted", "ended" }) do
        check(phases[phase], "input phase " .. phase)
    end
    input:removeSelf()
    input = nil
end
local reentrant
local function setupLifetime()
    reentrant = field(300)
    reentrant.placeholder = "lifetime-control"
    local object = reentrant
    local function makeListener(key, callback)
        local token = { key = key }
        weak[key] = token
        return function(event)
            payload(event, object)
            callback(event, token)
        end
    end
    object:addEventListener("userInput", makeListener("replaced", function() end))
    object:addEventListener("userInput", makeListener("active", function(event, token)
        if event.phase == "began" then
            object:addEventListener("userInput", makeListener("next", function(nextEvent, nextToken)
                record("lifetime-input", { phase = nextEvent.phase, recipient = "next" })
                if nextEvent.phase == "submitted" then
                    object:removeSelf()
                    collect()
                    check(weak.next == nextToken, "removed listener alive in callback",
                        { witnesses = witnessCount() })
                end
            end))
            collect()
            check(weak.active == token, "replaced listener alive in callback",
                { witnesses = witnessCount() })
        end
    end))
    collect()
    check(weak.replaced == nil, "replacement releases registry reference",
        { witnesses = witnessCount() })
    record("lifetime-ready")
end

local step, busy = 0, false
local commands = {
    function()
        dead:removeSelf()
        frames(8, function()
            check(dead.size == nil, "removed proxy size is nil")
            record("removed", { sizeType = type(dead.size) })
            busy = false
        end)
    end,
    function() named(); frames(24, function() busy = false end) end,
    function() setupInput(false); busy = false end,
    function() finishInput(); setupInput(true); busy = false end,
    function() finishInput(); setupLifetime(); busy = false end,
    function()
        frames(12, function()
            collect()
            check(weak.active == nil and weak.next == nil, "teardown releases registry references",
                { witnesses = witnessCount(), frames = 12 })
            record("complete", { failures = failures, errors = errors, resizes = resizeCount })
            busy = false
        end)
    end,
}
local button = display.newRect(160, 450, 300, 50)
button:setFillColor(0.2, 0.4, 0.7)
button:addEventListener("tap", function()
    if busy then return true end
    busy, step = true, step + 1
    if commands[step] then commands[step]() end
    return true
end)
display.newText("Advance test", 160, 450, native.systemFont, 20)
Runtime:addEventListener("system", function(event)
    record("system", { type = event.type })
end)
frames(8, function()
    record("ready", { version = system.getInfo("build"), scale = previousScale,
        size = previousSize, fontMode = fontMode, resizes = resizeCount })
end)
