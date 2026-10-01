--[[--
Spore client for the sync server, covering the bookmark extension and the stock progress
endpoint.

Modelled on cwasync.koplugin's client, with one difference that matters: this server
authenticates with the stock KOReader scheme -- an `x-auth-user` / `x-auth-key` header
pair, the key being the MD5 of the password -- not HTTP Basic. The credentials are
therefore exactly the ones stock kosync has already stored, and are read, never asked for.

Both endpoints are read and written, so this plugin can stand alone: running it
alongside stock kosync would mean two writers racing over one progress record.

The server stores the bookmark blob as one opaque string with no compare-and-swap, so a
PUT replaces whatever is there. Every write must therefore be the result of a merge
against the GET that immediately preceded it, and the two must sit in one pull -- that
round trip is the whole window in which another peer can be overwritten.
]]

local UIManager = require("ui/uimanager")
local logger = require("logger")
local Diag = require("cp_diag")
local socketutil = require("socketutil")

-- The bookmark blob is larger than a progress record and the device may be on slow WiFi,
-- but this still runs on the UI thread's looper, so the ceiling stays modest.
local SYNC_TIMEOUTS = { 5, 15 }

local CPSyncClient = {
    service_spec = nil,
    service_url = nil,
}

function CPSyncClient:new(o)
    o = o or {}
    setmetatable(o, self)
    self.__index = self
    if o.init then o:init() end
    return o
end

function CPSyncClient:init()
    local Spore = require("Spore")
    self.client = Spore.new_from_spec(self.service_spec, {
        base_url = self.service_url,
    })

    -- The server routes by version through the Accept header, as stock kosync does.
    package.loaded["Spore.Middleware.GinClient"] = {}
    require("Spore.Middleware.GinClient").call = function(_, req)
        req.headers["accept"] = "application/vnd.koreader.v1+json"
    end

    package.loaded["Spore.Middleware.CPSyncAuth"] = {}
    require("Spore.Middleware.CPSyncAuth").call = function(args, req)
        req.headers["x-auth-user"] = args.username
        req.headers["x-auth-key"] = args.userkey
    end

    package.loaded["Spore.Middleware.AsyncHTTP"] = {}
    require("Spore.Middleware.AsyncHTTP").call = function(args, req)
        -- Fall back to the synchronous path when the Turbo looper is absent.
        if not UIManager.looper then return end
        req:finalize()
        local result
        require("httpclient"):new():request({
            url = req.url,
            method = req.method,
            body = req.env.spore.payload,
            on_headers = function(headers)
                for header, value in pairs(req.headers) do
                    if type(header) == "string" then
                        headers:add(header, value)
                    end
                end
            end,
        }, function(res)
            result = res
            -- Turbo reports `code`; Spore reads `status`.
            result.status = res.code
            coroutine.resume(args.thread)
        end)
        return coroutine.create(function() coroutine.yield(result) end)
    end
end

function CPSyncClient:_prepare(username, userkey)
    self.client:reset_middlewares()
    self.client:enable("Format.JSON")
    self.client:enable("GinClient")
    self.client:enable("CPSyncAuth", { username = username, userkey = userkey })
end

--[[--
Run one authenticated request on the looper, reporting the outcome through `callback`.

@param label    what to name this request in the trace
@param invoke   function(client) performing the Spore call
@param callback callback(ok, body, status); `status` is the HTTP status, or nil when the
                request never produced one, and is reported for diagnostics only.
]]
function CPSyncClient:_call(username, userkey, label, invoke, callback)
    self:_prepare(username, userkey)
    socketutil:set_timeout(SYNC_TIMEOUTS[1], SYNC_TIMEOUTS[2])
    local co = coroutine.create(function()
        local ok, res = pcall(invoke, self.client)
        if ok then
            Diag.log(label, "-> HTTP", tostring(res.status))
            callback(res.status == 200, res.body, res.status)
        else
            Diag.log(label, "raised:", tostring(res))
            logger.dbg("CrossPointSync:", label, "failed:", res)
            callback(false, nil, nil)
        end
    end)
    self.client:enable("AsyncHTTP", { thread = co })
    coroutine.resume(co)
    if UIManager.looper then UIManager:setInputTimeout() end
    socketutil:reset_timeout()
end

--[[--
Fetch the bookmark blob for a document.

On success `body.bookmarks` is the blob, which the server stores and returns verbatim as
an opaque JSON *string* -- so it is decoded a second time by the caller, not by Spore.
]]
function CPSyncClient:getBookmarks(username, userkey, document, callback)
    self:_call(username, userkey, "GET /syncs/bookmarks/" .. tostring(document), function(client)
        return client:get_bookmarks({ document = document })
    end, callback)
end

--[[--
Fetch the stored reading position for a document.

On success `body` carries the stock kosync record -- `progress` (an XPointer for a reflow
document), `percentage`, `device`, `device_id`, `timestamp` -- plus, from a CrossPoint
device, the server fork's richer `position` object, which this plugin does not need: the
`progress` XPointer is already what crengine navigates by.
]]
function CPSyncClient:getProgress(username, userkey, document, callback)
    self:_call(username, userkey, "GET /syncs/progress/" .. tostring(document), function(client)
        return client:get_progress({ document = document })
    end, callback)
end

--[[--
Replace the stored bookmark blob for a document.

`blob` is already-serialized JSON: the server keeps the field verbatim and never parses
it, so it travels as a string inside the request body rather than as nested JSON. That is
exactly what the firmware sends (KOReaderSyncClient::updateBookmarks).
]]
function CPSyncClient:putBookmarks(username, userkey, document, blob, callback)
    self:_call(username, userkey, "PUT /syncs/bookmarks (" .. tostring(#blob) .. " bytes)", function(client)
        return client:update_bookmarks({ document = document, bookmarks = blob })
    end, callback)
end

--[[--
Replace the stored reading position for a document.

The server takes `progress`, `percentage` and `device` together and rejects the write if
any is missing (syncs_controller.lua:209), so the caller must have all three before
calling. `device_id` is optional to the server and identifies this installation, which is
how a peer recognises a record as its own rather than one worth following.

The fork also stores a richer `position` object alongside, sent only by CrossPoint
devices, and DELETES it on any write that carries none -- deliberately, since a stale
anchor pointing somewhere the new progress does not is worse than no anchor. A push from
here therefore costs the device the extra precision of its own last record; it still
resolves the plain XPointer, which is what it navigates by.
]]
function CPSyncClient:putProgress(username, userkey, record, callback)
    self:_call(username, userkey, "PUT /syncs/progress (" .. tostring(record.percentage) .. ")", function(client)
        return client:update_progress({
            document = record.document,
            progress = record.progress,
            percentage = record.percentage,
            device = record.device,
            device_id = record.device_id,
        })
    end, callback)
end

return CPSyncClient
