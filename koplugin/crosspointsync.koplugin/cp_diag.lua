--[[--
A small in-memory trace of the last sync attempt, so a pull that produced nothing can be
read back afterwards instead of guessed at.

Every failure this plugin can hit -- no credentials, a document key neither side agrees
on, an empty blob, anchors that do not resolve in this copy of the book -- ends in the
same visible outcome: no annotations appear. The trace is what tells them apart.

Lines go to KOReader's own logger as well, so they also land in crash.log; the ring here
exists so the user can read them on the device without a console, and dump them to a file.
]]

local Diag = {
    -- Bounded so a long auto-sync session cannot grow this without limit. A pull writes
    -- one line per record plus the merge decisions, so a book at the 128-mark cap is the
    -- worst case and this clears it; the oldest lines are dropped first.
    MAX_LINES = 320,
    lines = {},
}

--- Drop everything recorded so far. Called at the start of each pull so the trace always
--- describes one attempt rather than an accumulation of them.
function Diag.reset()
    Diag.lines = {}
end

--- Record one line. Extra arguments are appended, space-separated; tables are not
--- traversed, so callers format their own detail.
function Diag.log(...)
    local parts = {}
    for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
    end
    local line = table.concat(parts, " ")
    Diag.lines[#Diag.lines + 1] = line
    if #Diag.lines > Diag.MAX_LINES then
        table.remove(Diag.lines, 1)
    end
    local ok, logger = pcall(require, "logger")
    if ok then logger.info("CrossPointSync:", line) end
end

--- The trace as one block of text, oldest first.
function Diag.text()
    if #Diag.lines == 0 then
        return "No sync has run yet in this session."
    end
    return table.concat(Diag.lines, "\n")
end

--- Where dump() writes. Kept next to the other KOReader data so it is easy to find over
--- USB without knowing the plugin's install path.
function Diag.path()
    return require("datastorage"):getDataDir() .. "/crosspointsync-debug.txt"
end

--[[--
Write the trace to Diag.path().

@return path on success, or nil plus an error message.
]]
function Diag.dump()
    local path = Diag.path()
    local f, err = io.open(path, "w")
    if not f then return nil, tostring(err) end
    f:write(Diag.text(), "\n")
    f:close()
    return path
end

return Diag
