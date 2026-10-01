--[[--
Minimal busted-compatible runner.

The specs are written for busted, which is how they run in CI alongside readest's. This
exists so they can also be run with any plain Lua 5.1+ interpreter during development,
with no rocks to install:  texlua spec/run.lua   (or  lua spec/run.lua)
]]

package.path = "./?.lua;./spec/?.lua;" .. package.path

-- KOReader's MD5, which cp_docid uses and which is not available outside it. The digest's
-- exact value is never asserted -- only that two names normalise to the same key -- so an
-- identity stand-in is enough.
package.loaded["ffi/sha2"] = { md5 = function(s) return "md5(" .. s .. ")" end }

local passed, failed, stack = 0, 0, {}

local function label(name)
    local parts = {}
    for _, s in ipairs(stack) do parts[#parts + 1] = s end
    parts[#parts + 1] = name
    return table.concat(parts, " / ")
end

function describe(name, fn)
    stack[#stack + 1] = name
    fn()
    stack[#stack] = nil
end

function it(name, fn)
    local ok, err = pcall(fn)
    if ok then
        passed = passed + 1
    else
        failed = failed + 1
        print("FAIL " .. label(name) .. "\n      " .. tostring(err))
    end
end

local function fmt(v)
    if type(v) == "table" then
        local parts = {}
        for _, x in ipairs(v) do parts[#parts + 1] = tostring(x) end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return tostring(v)
end

local function deepEqual(a, b)
    if a == b then return true end
    if type(a) ~= "table" or type(b) ~= "table" then return false end
    for k, v in pairs(a) do if not deepEqual(v, b[k]) then return false end end
    for k in pairs(b) do if a[k] == nil then return false end end
    return true
end

assert = setmetatable({
    are = {
        equal = function(e, a)
            if e ~= a then error("expected " .. fmt(e) .. ", got " .. fmt(a), 2) end
        end,
        same = function(e, a)
            if not deepEqual(e, a) then error("expected " .. fmt(e) .. ", got " .. fmt(a), 2) end
        end,
    },
    is_true = function(v) if v ~= true then error("expected true, got " .. fmt(v), 2) end end,
    is_false = function(v) if v ~= false then error("expected false, got " .. fmt(v), 2) end end,
    is_nil = function(v) if v ~= nil then error("expected nil, got " .. fmt(v), 2) end end,
}, { __call = function(_, v, msg) if not v then error(msg or "assertion failed", 2) end return v end })

for _, spec in ipairs({
    "spec/cp_docid_spec.lua",
    "spec/cp_lamport_spec.lua",
    "spec/cp_ledger_spec.lua",
    "spec/cp_annotations_spec.lua",
    "spec/cp_push_spec.lua",
    "spec/cp_sync_spec.lua",
    "spec/cp_progress_spec.lua",
    "spec/cp_modes_spec.lua",
}) do
    dofile(spec)
end

print(string.format("%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
