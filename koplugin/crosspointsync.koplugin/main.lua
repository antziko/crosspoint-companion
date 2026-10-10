--[[--
CrossPoint Sync -- keeps the bookmarks and highlights on an Xteink/CrossPoint e-reader
and in KOReader in step, and pulls the device's reading position.

Annotations sync both ways, by the same Lamport rule the firmware uses: every record
carries a version, and for each spot the highest version across {bookmark, tombstone} on
either side wins, with a live bookmark taking a tie. That is what lets a delete on one
device survive a re-add on the other without either having a clock.

Writing is opt-in and off until asked for. The server stores the annotation set as one
opaque blob, last-writer-wins with no compare-and-swap, and a device caps at 128 marks
per book -- every way to lose data is in the write direction, so a merge is always made
against the GET immediately preceding the PUT, and the whole push is refused rather than
truncated.

The reading position travels both ways too, so this plugin can replace stock kosync
rather than sit beside it. Two plugins writing one progress record would race, so turning
position pushing on here means turning stock progress sync off.

Reading the position here rather than leaving all of it to stock kosync is not
duplication: a CrossPoint device files annotations and position under one document key,
and that key is not necessarily the one stock kosync computes. Once a book's key matches
well enough for its annotations to arrive, its position is already sitting under the same
key, and pulling both together is what makes the two devices agree on where you are.
]]

local ConfirmBox = require("ui/widget/confirmbox")
local DataStorage = require("datastorage")
local Device = require("device")
local Dispatcher = require("dispatcher")
local Event = require("ui/event")
local InfoMessage = require("ui/widget/infomessage")
local MultiInputDialog = require("ui/widget/multiinputdialog")
local TextViewer = require("ui/widget/textviewer")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local logger = require("logger")
local random = require("random")
local _ = require("gettext")
local T = require("ffi/util").template

local Annotations = require("cp_annotations")
local DocId = require("cp_docid")
local Ledger = require("cp_ledger")
local Modes = require("cp_modes")
local Push = require("cp_push")
local Sync = require("cp_sync")
local Progress = require("cp_progress")
local CPSyncClient = require("cp_syncclient")
local Diag = require("cp_diag")
local md5 = require("ffi/sha2").md5

local CrossPointSync = WidgetContainer:extend{
    name = "crosspointsync",
    is_doc_only = false,
}

local SETTINGS_KEY = "crosspointsync"

-- Seconds of no page turn before a position push goes out, and how many turns must have
-- happened first. The idle wait is what keeps a skim from sending a request per page; the
-- page count is what keeps a book that is merely open off the network. A book closed or
-- suspended pushes regardless, so these only govern reading that just keeps going.
local PUSH_IDLE_SECONDS = 15
local PUSH_AFTER_PAGES = 10

-- Shared with stock kosync by design: it is the identity of this KOReader installation,
-- not of either plugin, and a peer uses it to recognise a record as its own.
if G_reader_settings:hasNot("device_id") then
    G_reader_settings:saveSetting("device_id", random.uuid())
end

function CrossPointSync:init()
    self.settings = G_reader_settings:readSetting(SETTINGS_KEY) or {
        auto_sync = true,
        filename_mode = true,
        sync_annotations = true,
    }
    -- Added after the first release, so an existing settings table has no value for it.
    if self.settings.sync_progress == nil then self.settings.sync_progress = true end
    -- Likewise, and on by default: syncing annotations is what the plugin is for, and a
    -- configuration written before the off switch existed was doing exactly that.
    if self.settings.sync_annotations == nil then self.settings.sync_annotations = true end
    -- Writing is never turned on by an update. Enabling it uploads every annotation
    -- already in the book, which against a large existing set is exactly the case the
    -- pull-only design was avoiding; that has to be a decision, not a side effect.
    if self.settings.push_changes == nil then self.settings.push_changes = false end
    -- Off for the same reason, and for one more: stock kosync may still be enabled, and
    -- two plugins writing the same record would each undo the other.
    if self.settings.push_progress == nil then self.settings.push_progress = false end
    self.device_id = G_reader_settings:readSetting("device_id")
    self.ui.menu:registerToMainMenu(self)
    self:onDispatcherRegisterActions()
end

function CrossPointSync:onDispatcherRegisterActions()
    Dispatcher:registerAction("crosspointsync_pull", {
        category = "none",
        event = "CrossPointSyncPull",
        title = _("Sync with CrossPoint"),
        reader = true,
    })
    Dispatcher:registerAction("crosspointsync_push_progress", {
        category = "none",
        event = "CrossPointSyncPushProgress",
        title = _("Send my reading position to CrossPoint"),
        reader = true,
    })
end

function CrossPointSync:saveSettings()
    G_reader_settings:saveSetting(SETTINGS_KEY, self.settings)
end

--[[--
Say so when stock progress sync is also set to write.

Nothing here can turn another plugin off -- its settings are its own, and writing to them
from outside is how one release's rename becomes this plugin corrupting them. But two
writers on one record is silent: whichever saved last wins, and the position simply goes
backwards now and then. Worth one message at the moment the second writer is switched on.
]]
function CrossPointSync:_warnIfStockSyncWrites()
    local kosync = G_reader_settings:readSetting("kosync")
    if type(kosync) ~= "table" then return end
    -- Logged out, so it writes nothing whatever its other settings say.
    if not kosync.username or not kosync.userkey then return end
    if kosync.auto_sync == false then return end
    Diag.log("stock progress sync is also configured to write")
    UIManager:show(InfoMessage:new{
        text = _([[KOReader's own progress sync is still on.

Both now upload your position to the same record, and the later save wins -- which is not always the device you were reading on.

Turn it off under Tools -> Progress sync -> Automatically keep documents in sync.]]),
    })
end

--[[--
Credentials, preferring this plugin's own over stock kosync's.

Borrowing kosync's login means one set of details to keep, so it is still tried -- but its
settings are internal to that plugin and have changed shape across releases, so a login
set here always wins and is what makes the plugin usable when the borrowed one is missing.

The password itself is never stored: the server authenticates with `x-auth-key`, the MD5
of the password, so that is all that is kept.
]]
function CrossPointSync:getCredentials()
    if self.settings.username and self.settings.userkey then
        Diag.log("credential source: this plugin")
        return {
            username = self.settings.username,
            userkey = self.settings.userkey,
            server = self.settings.server,
        }
    end

    local kosync = G_reader_settings:readSetting("kosync")
    if type(kosync) ~= "table" then
        Diag.log("no \"kosync\" settings table, and no login set here")
        self:_reportSyncSettings()
        return nil
    end
    -- Naming the fields kosync actually has makes a rename there diagnosable rather than
    -- silent, since this plugin cannot otherwise tell "logged out" from "renamed".
    local keys = {}
    for k in pairs(kosync) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    Diag.log("kosync settings present, fields:", table.concat(keys, ", "))
    if not kosync.username or not kosync.userkey then
        self:_reportSyncSettings()
        return nil
    end
    Diag.log("credential source: stock kosync")
    return {
        username = kosync.username,
        userkey = kosync.userkey,
        server = kosync.custom_server,
    }
end

--[[--
List the sync-related settings KOReader actually holds, without printing their values.

Called only when borrowing a login failed. Progress sync working while this plugin sees
no credentials means they live somewhere this does not look; naming the keys that exist
turns that into something reportable instead of a dead end.
]]
function CrossPointSync:_reportSyncSettings()
    local ok, data = pcall(function() return G_reader_settings.data end)
    if not ok or type(data) ~= "table" then
        Diag.log("could not inspect G_reader_settings")
        return
    end
    local found = {}
    for k, v in pairs(data) do
        local name = tostring(k)
        if name:lower():find("sync", 1, true) then
            if type(v) == "table" then
                local sub = {}
                for kk in pairs(v) do sub[#sub + 1] = tostring(kk) end
                table.sort(sub)
                found[#found + 1] = name .. " = {" .. table.concat(sub, ", ") .. "}"
            else
                found[#found + 1] = name .. " : " .. type(v)
            end
        end
    end
    table.sort(found)
    if #found == 0 then
        Diag.log("no sync-related keys in G_reader_settings")
    else
        Diag.log("sync-related settings found:")
        for _, line in ipairs(found) do Diag.log("   ", line) end
    end
end

--- Prompt for server, username and password, and keep them for this plugin alone.
function CrossPointSync:editCredentials()
    local dialog
    dialog = MultiInputDialog:new{
        title = _("CrossPoint sync server"),
        fields = {
            {
                text = self.settings.server or "",
                hint = _("Server address, e.g. https://192.168.50.67:7200"),
            },
            {
                text = self.settings.username or "",
                hint = _("Username"),
            },
            {
                text = "",
                hint = _("Password (leave blank to keep the current one)"),
                text_type = "password",
            },
        },
        buttons = {{
            {
                text = _("Cancel"),
                id = "close",
                callback = function() UIManager:close(dialog) end,
            },
            {
                text = _("Save"),
                callback = function()
                    local f = dialog:getFields()
                    local server = (f[1] or ""):gsub("^%s+", ""):gsub("%s+$", ""):gsub("/+$", "")
                    local username = (f[2] or ""):gsub("^%s+", ""):gsub("%s+$", "")
                    local password = f[3] or ""
                    UIManager:close(dialog)

                    self.settings.server = server ~= "" and server or nil
                    self.settings.username = username ~= "" and username or nil
                    if password ~= "" then
                        self.settings.userkey = md5(password)
                    end
                    self:saveSettings()
                    -- The client caches its base URL, so a changed server needs a new one.
                    self.client = nil

                    UIManager:show(InfoMessage:new{
                        text = self.settings.username and self.settings.userkey
                            and T(_("Signed in as %1."), self.settings.username)
                            or _("Login cleared; stock progress sync will be used instead."),
                    })
                end,
            },
        }},
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

-- How each mode reads in the menu. The wording says which way things travel without
-- making the user think in directions: "receive" is the one everybody wants, and
-- "two-way" is the one that needs a warning.
local MODE_LABEL = {
    [Modes.OFF] = _("Off"),
    [Modes.RECEIVE] = _("Receive from CrossPoint"),
    [Modes.SEND] = _("Send to CrossPoint"),
    [Modes.TWO_WAY] = _("Two-way"),
}

--[[--
One row of a three-state choice.

`radio` is cosmetic: where a KOReader build does not know the field the rows draw as
check marks, which is how mutually exclusive menu rows have always looked and behaves
the same.
]]
local function modeItem(mode, get, set)
    return {
        text = MODE_LABEL[mode],
        radio = true,
        checked_func = function() return get() == mode end,
        callback = function() set(mode) end,
    }
end

function CrossPointSync:_annotationMode() return Modes.annotations(self.settings) end
function CrossPointSync:_progressMode() return Modes.progress(self.settings) end

function CrossPointSync:_setAnnotationMode(mode)
    Modes.setAnnotations(self.settings, mode)
    self:saveSettings()
end

function CrossPointSync:_setProgressMode(mode)
    Modes.setProgress(self.settings, mode)
    self:saveSettings()
    self:_registerProgressEvents()
end

--[[--
Ask before uploading annotations for the first time.

Turning this on is not like the other settings: it uploads the whole book's existing set
at once and makes this device able to change what the others hold. That has to be a
decision rather than a tap.
]]
function CrossPointSync:_confirmAnnotationUpload()
    UIManager:show(ConfirmBox:new{
        text = _([[Also send your bookmarks and highlights?

Every annotation this book already has is uploaded on the next sync, which may be a lot more than you expect, and this device can then change what the others hold. Any annotation you have already deleted here goes out as a delete.

An upload is refused rather than truncated if the result would not fit the 128 marks a device can store.]]),
        ok_text = _("Send mine too"),
        ok_callback = function() self:_setAnnotationMode(Modes.TWO_WAY) end,
    })
end

function CrossPointSync:addToMainMenu(menu_items)
    menu_items.crosspointsync = {
        text = _("CrossPoint sync"),
        sorting_hint = "tools",
        sub_item_table = {
            {
                text = _("Sync now"),
                keep_menu_open = false,
                enabled_func = function() return self.ui and self.ui.document ~= nil end,
                callback = function() self:_syncNow(true) end,
            },
            {
                text = _("Sync automatically"),
                help_text = _([[Sync when a book opens.

While the reading position is set to two-way, yours is also sent when you close the book, when the device suspends, and after a pause in reading.]]),
                checked_func = function() return self.settings.auto_sync end,
                callback = function()
                    self.settings.auto_sync = not self.settings.auto_sync
                    self:saveSettings()
                end,
                separator = true,
            },
            {
                text_func = function()
                    return T(_("Bookmarks and highlights: %1"), MODE_LABEL[self:_annotationMode()])
                end,
                help_text = _([[What happens to this book's bookmarks and highlights when it syncs.

While this is set to receive, the device is the only writer, so an annotation you delete here stays deleted here but the device keeps it. The delete is remembered, and goes out on the first sync after you switch to two-way.]]),
                sub_item_table = {
                    modeItem(Modes.OFF,
                        function() return self:_annotationMode() end,
                        function(mode) self:_setAnnotationMode(mode) end),
                    modeItem(Modes.RECEIVE,
                        function() return self:_annotationMode() end,
                        function(mode) self:_setAnnotationMode(mode) end),
                    {
                        text = MODE_LABEL[Modes.TWO_WAY],
                        radio = true,
                        checked_func = function() return self:_annotationMode() == Modes.TWO_WAY end,
                        callback = function() self:_confirmAnnotationUpload() end,
                    },
                },
            },
            {
                text_func = function()
                    return T(_("Reading position: %1"), MODE_LABEL[self:_progressMode()])
                end,
                help_text = _([[What happens to where you are in this book when it syncs.

A sync you asked for goes straight to the device's position; an automatic one asks first, so opening a book never moves you without warning.

Send only uploads yours and never takes the device's, so this device decides where you are. Two-way does both.

Turn KOReader's own progress sync OFF before sending: both write the same record, and whichever saved last wins, which is not always the device you were reading on.]]),
                sub_item_table = {
                    modeItem(Modes.OFF,
                        function() return self:_progressMode() end,
                        function(mode) self:_setProgressMode(mode) end),
                    modeItem(Modes.RECEIVE,
                        function() return self:_progressMode() end,
                        function(mode) self:_setProgressMode(mode) end),
                    {
                        text = MODE_LABEL[Modes.SEND],
                        radio = true,
                        checked_func = function() return self:_progressMode() == Modes.SEND end,
                        callback = function()
                            self:_setProgressMode(Modes.SEND)
                            self:_warnIfStockSyncWrites()
                        end,
                    },
                    {
                        text = MODE_LABEL[Modes.TWO_WAY],
                        radio = true,
                        checked_func = function() return self:_progressMode() == Modes.TWO_WAY end,
                        callback = function()
                            self:_setProgressMode(Modes.TWO_WAY)
                            self:_warnIfStockSyncWrites()
                        end,
                    },
                },
                separator = true,
            },
            {
                text_func = function()
                    return self.settings.username
                        and T(_("Server and login (%1)"), self.settings.username)
                        or _("Server and login (using progress sync's)")
                end,
                keep_menu_open = true,
                help_text = _([[Set the sync server and login for this plugin. Leave the username blank to fall back to the standard progress-sync login instead.]]),
                callback = function() self:editCredentials() end,
            },
            {
                text = _("Advanced"),
                sub_item_table = {
                    {
                        text = _("Match books by filename"),
                        help_text = _([[Match the CrossPoint device's filename-based sync mode instead of the content hash. Use this only if the device is set the same way -- otherwise the two never find each other's annotations.]]),
                        checked_func = function() return self.settings.filename_mode end,
                        callback = function()
                            self.settings.filename_mode = not self.settings.filename_mode
                            self:saveSettings()
                        end,
                        separator = true,
                    },
                    {
                        text = _("Show sync diagnostics"),
                        help_text = _([[What the last sync actually did: the document key it asked for, what the server returned, and why any annotation could not be placed.]]),
                        keep_menu_open = true,
                        callback = function()
                            UIManager:show(TextViewer:new{
                                title = _("CrossPoint sync diagnostics"),
                                text = Diag.text(),
                                justified = false,
                            })
                        end,
                    },
                    {
                        text = _("Save sync diagnostics to a file"),
                        keep_menu_open = true,
                        callback = function()
                            local path, err = Diag.dump()
                            UIManager:show(InfoMessage:new{
                                text = path and T(_("Saved to:\n%1"), path)
                                            or T(_("Could not save: %1"), err),
                            })
                        end,
                        separator = true,
                    },
                    {
                        text = _("Forget what was synced for this book"),
                        help_text = _([[Every mark synced so far is remembered, along with the version that orders it against the other devices' changes. Forgetting that leaves the annotations in place but stops tracking them: the next sync treats what is here as new, and, if uploading is on, sends it all again.]]),
                        enabled_func = function() return self.ui and self.ui.doc_settings ~= nil end,
                        callback = function()
                            Ledger.forget(self.ui.doc_settings)
                            UIManager:show(InfoMessage:new{ text = _("Sync record for this book cleared.") })
                        end,
                    },
                },
            },
        },
    }
end

function CrossPointSync:onReaderReady()
    -- Per-book, so a position agreed for the last book cannot suppress this one's first
    -- push. Left nil deliberately: with nothing agreed yet, the first close does send.
    self._sentProgress = nil
    self._progressKey = nil
    self._turnsSincePush = 0
    self:_registerProgressEvents()
    if self.settings.auto_sync then
        self:_syncNow(false)
    end
end

--[[--
Attach the position-push handlers, or detach them.

WidgetContainer dispatches by looking the handler up on the instance, so assigning nil
is what stops a disabled setting from costing a call on every page turn.
]]
function CrossPointSync:_registerProgressEvents()
    if self.settings.push_progress then
        self.onCloseDocument = self._onCloseDocument
        self.onSuspend = self._onSuspend
        self.onPageUpdate = self._onPageUpdate
    else
        self.onCloseDocument = nil
        self.onSuspend = nil
        self.onPageUpdate = nil
        self:_cancelScheduledPush()
    end
end

function CrossPointSync:_cancelScheduledPush()
    if self._pushTask then
        UIManager:unschedule(self._pushTask)
        self._pushTask = nil
    end
end

--[[--
Send after a pause, once enough pages have turned.

Rescheduled on every turn, so the request goes out when reading stops rather than during
it, and the page gate keeps a book that is merely open from talking to the network.
]]
function CrossPointSync:_onPageUpdate(page)
    if page == nil then return end
    if page == self._lastPage then return end
    self._lastPage = page
    self._turnsSincePush = (self._turnsSincePush or 0) + 1
    if self._turnsSincePush < PUSH_AFTER_PAGES then return end
    self:_cancelScheduledPush()
    self._pushTask = function()
        self._pushTask = nil
        self:pushProgress(false)
    end
    UIManager:scheduleIn(PUSH_IDLE_SECONDS, self._pushTask)
end

function CrossPointSync:_onSuspend()
    self:_cancelScheduledPush()
    self:pushProgress(false)
end

--[[--
Send on close.

Everything the request needs is read before it is made, because the document instance is
torn down as soon as this returns and the reply arrives after that.
]]
function CrossPointSync:_onCloseDocument()
    self:_cancelScheduledPush()
    self:pushProgress(false)
end

function CrossPointSync:onCloseWidget()
    self:_cancelScheduledPush()
end

function CrossPointSync:onCrossPointSyncPushProgress()
    self:pushProgress(true)
    return true
end

function CrossPointSync:onCrossPointSyncPull()
    self:_syncNow(true)
    return true
end

--[[--
End the sync, whatever happened, and send this device's position if that is still owed.

Safe to call more than once: the pending flag is cleared here, so the push goes out on
the first of them.

The push comes last on purpose. A device that is further along is taken first, and
`Progress.shouldPush` then compares against what the sync agreed on and suppresses the
send -- so what survives to be uploaded is the case worth uploading: the server holding
no position for this book at all.
]]
function CrossPointSync:_finish()
    self._running = false
    if self._pushAfterSync then
        self._pushAfterSync = false
        self:pushProgress(false)
    end
end

--[[--
The one sync action: bookmarks, highlights and reading position, each as far as its own
mode allows.

@param interactive true when the user asked for it, which is what decides whether
                   outcomes are shown and whether a position jump is taken or offered.
]]
function CrossPointSync:_syncNow(interactive)
    if self:_annotationMode() == Modes.OFF and self:_progressMode() == Modes.OFF then
        Diag.log("sync: nothing is turned on")
        if interactive then
            UIManager:show(InfoMessage:new{
                text = _("Nothing is set to sync. Turn on bookmarks and highlights, or the reading position."),
                timeout = 3,
            })
        end
        return
    end
    self:pull(interactive)
end

function CrossPointSync:_notify(text, interactive)
    self:_finish()
    Diag.log("outcome:", text)
    if interactive then
        UIManager:show(InfoMessage:new{ text = text, timeout = 3 })
    else
        logger.dbg("CrossPointSync:", text)
    end
end

--[[--
Run the sync: fetch and merge the annotation set, then deal with the reading position.

Each half is skipped when its mode says off, and `_finish` sends this device's position
at the end if that is owed.

@param interactive true when the user asked; only then are outcomes shown on screen. An
                   automatic sync stays silent unless it actually changed something.
]]
function CrossPointSync:pull(interactive)
    if not self.ui or not self.ui.document then return end
    -- A pull opens a book automatically and the user can ask for one at the same time.
    -- Two in flight would merge against the same blob and upload one over the other, and
    -- the second Diag.reset() would throw away the first one's trace.
    if self._running then
        Diag.log("a pull is already in flight; ignoring this one")
        if interactive then
            UIManager:show(InfoMessage:new{ text = _("A sync is already running."), timeout = 2 })
        end
        return
    end
    self._running = true
    -- Decided at the start of the sync rather than read at the end of it, so a mode
    -- changed while requests are in flight cannot leave a push half-owed.
    self._pushAfterSync = self.settings.push_progress and true or false

    Diag.reset()
    Diag.log("--- sync", interactive and "(manual)" or "(automatic)", os.date("%Y-%m-%d %H:%M:%S"))
    Diag.log("file:", tostring(self.ui.document.file))
    Diag.log("bookmarks and highlights:", self:_annotationMode())
    Diag.log("reading position:", self:_progressMode())

    local annotations = self:_annotationMode() ~= Modes.OFF
    -- ui.annotation is the 2024 unification of bookmarks and highlights. Without it there
    -- is no single list to insert into, and the shapes below do not apply. It says nothing
    -- about the reading position, so it only stops the sync when annotations are wanted.
    if annotations and (not self.ui.annotation or not self.ui.annotation.annotations) then
        Diag.log("ui.annotation missing -- KOReader predates the unified annotation list")
        self:_notify(_("This KOReader version is too old: it has no unified annotation list."), interactive)
        return
    end
    if annotations then
        Diag.log("existing annotations here:", #self.ui.annotation.annotations)
    end

    local creds = self:getCredentials()
    Diag.log("kosync credentials:", creds and ("user=" .. tostring(creds.username)) or "ABSENT")
    if not creds then
        self:_notify(_("No login. Set one under \"Server and login\", or set up the standard progress sync first."), interactive)
        return
    end
    Diag.log("server:", tostring(creds.server))
    if not creds.server then
        self:_notify(_("No server address. Set one under \"Server and login\"."), interactive)
        return
    end

    local document = DocId.forDocument(self.ui, self.settings.filename_mode)
    local alternate = DocId.forDocument(self.ui, not self.settings.filename_mode)
    Diag.log("match mode:", self.settings.filename_mode and "filename" or "binary (content hash)")
    Diag.log("  document key (binary)  :", tostring(DocId.forDocument(self.ui, false)))
    Diag.log("  document key (filename):", tostring(DocId.fromFilename(self.ui.document.file)))
    Diag.log("  -> asking the server for:", tostring(document))
    if not document then
        self:_notify(_("Could not identify this book for sync."), interactive)
        return
    end
    if alternate == document then alternate = nil end

    local keys = alternate and { document, alternate } or { document }
    self:_ensureClient(creds)
    if not annotations then
        -- `quiet` keeps the report from saying anything about annotations, which would
        -- otherwise read as "already up to date" for a half of the sync that never ran.
        self:pullProgress(creds, keys, 1, interactive,
                          { added = 0, removed = 0, skipped = 0, quiet = true })
        return
    end
    self:fetch(creds, keys, 1, interactive)
end

--- Build the Spore client on first use. One per session; the server address only
--- changes through the credentials dialog, which drops it.
function CrossPointSync:_ensureClient(creds)
    if not self.client then
        self.client = CPSyncClient:new{
            service_spec = DataStorage:getDataDir() .. "/plugins/crosspointsync.koplugin/api.json",
            service_url = creds.server,
        }
    end
end

--[[--
Fetch the annotation blob, trying each candidate document key in turn.

@param keys  the keys to try, most likely first
@param index which of them this call is for
]]
function CrossPointSync:fetch(creds, keys, index, interactive)
    local document = keys[index]
    self.client:getBookmarks(creds.username, creds.userkey, document, function(ok, body, status)
        if not ok then
            Diag.log("request failed; HTTP status:", tostring(status))
            self:_notify(_("Could not reach the sync server."), interactive)
            return
        end
        local raw = body and body.bookmarks
        local empty = type(raw) ~= "string" or raw == ""
        if empty and keys[index + 1] then
            Diag.log("nothing under that key; retrying with the other match mode:", keys[index + 1])
            self:fetch(creds, keys, index + 1, interactive)
            return
        end
        local summary = self:apply(body)
        -- Nothing anywhere means no key is confirmed, so an upload goes under the
        -- configured one rather than whichever was tried last.
        local pushTo = empty and keys[1] or document
        self:push(creds, pushTo, summary, function()
            -- The device files annotations and reading position under one key, so a key
            -- that produced a blob is the one to ask for the position. When none did,
            -- neither is confirmed and both are worth trying again.
            self:pullProgress(creds, empty and keys or { document }, 1, interactive, summary)
        end)
    end)
end

--[[--
Where in its own chapter an XPointer sits, as a fraction.

CrossPoint stores a mark's position as progress *within the spine item*, which KOReader
has no direct notion of. The DocFragment's own bounds give it: the page the fragment
starts on, against the page the next one starts on. Advisory only -- it is never part of
a mark's identity, and the device recomputes it from the anchor when it adopts the mark.

@return 0..1, or nil when the fragment's bounds cannot be resolved.
]]
function CrossPointSync:_intraSpineProgress(xpointer, spine)
    local doc = self.ui.document
    if not doc or not doc.getPageFromXPointer then return nil end

    local function pageOf(xp)
        if not xp then return nil end
        local ok, page = pcall(function() return doc:getPageFromXPointer(xp) end)
        return ok and tonumber(page) or nil
    end
    -- crengine omits the index in a single-spine book, so which form resolves is the
    -- book's property, not ours to assume.
    local function fragmentBody(index)
        local candidates = { string.format("/body/DocFragment[%d]/body", index + 1) }
        if index == 0 then candidates[#candidates + 1] = "/body/DocFragment/body" end
        for _, xp in ipairs(candidates) do
            if not doc.isXPointerInDocument then return xp end
            local ok, present = pcall(function() return doc:isXPointerInDocument(xp) end)
            if ok and present then return xp end
        end
        return nil
    end

    local here = pageOf(xpointer)
    local first = pageOf(fragmentBody(spine))
    if not here or not first then return nil end
    local after = pageOf(fragmentBody(spine + 1))
    -- The last chapter has no successor to bound it, and a one-page chapter has no span.
    if not after or after <= first then return 0 end
    local frac = (here - first) / (after - first)
    if frac < 0 then return 0 end
    if frac > 1 then return 1 end
    return frac
end

--[[--
Merge the fetched blob with what is here, and apply the result.

The rule is the firmware's, in the firmware's terms: every record on either side enters
as one entry at its own version, a mark deleted here becomes a tombstone one tick above
everything seen, and the highest version for each spot wins. What falls out is at once
the new state of this document, the new ledger, and the blob to send back.

@return a summary `{ added, removed, skipped, sending, note, blob, refused, commit }`,
        reported once the reading position has been dealt with too, so one pull produces
        one message. `commit(ok)` records what became of the upload and must be called
        exactly once -- with `true` when there was nothing to upload.
]]
function CrossPointSync:apply(body)
    local ui = self.ui
    local mgr = ui.annotation

    -- The server stores the blob verbatim as an opaque string, so it comes back
    -- double-encoded: a JSON body whose `bookmarks` field is itself JSON.
    local raw = body and body.bookmarks
    Diag.log("response blob:", type(raw) == "string" and (#raw .. " bytes") or ("absent (" .. type(raw) .. ")"))

    local remoteB, remoteT, note = {}, {}, nil
    if type(raw) == "string" and raw ~= "" then
        local ok, blob = pcall(function() return require("json").decode(raw) end)
        if not ok or type(blob) ~= "table" then
            -- Stop here rather than merge against nothing: an upload built on a blob that
            -- could not be read would replace real records with a guess.
            Diag.log("blob did not decode as JSON:", tostring(blob))
            return { added = 0, removed = 0, skipped = 0,
                     note = _("The stored annotation data could not be read."),
                     commit = function() end }
        end
        remoteB = type(blob.b) == "table" and blob.b or {}
        remoteT = type(blob.t) == "table" and blob.t or {}
        Diag.log("blob decoded: b=" .. #remoteB, "t=" .. #remoteT)
    else
        note = _("No CrossPoint annotations stored for this book.")
    end

    local ledger = Ledger.load(ui.doc_settings)
    local plan = Sync.plan({
        remoteB = remoteB,
        remoteT = remoteT,
        marks = ledger.marks,
        stubs = ledger.stubs,
        pending = ledger.pending,
        clock = ledger.clock,
        annotations = mgr.annotations,
        sending = self.settings.push_changes and true or false,
        doc = ui.document,
        toc = ui.toc,
        datetime = os.date("%Y-%m-%d %H:%M:%S"),
        newRecord = function(item, version)
            return Push.newRecord(item, version, {
                chapterOf = function(xp)
                    return ui.toc and ui.toc.getTocTitleByPage and ui.toc:getTocTitleByPage(xp) or ""
                end,
                progressOf = function(xp, spine) return self:_intraSpineProgress(xp, spine) end,
            })
        end,
        log = function(line) Diag.log(line) end,
    })
    Diag.log("placeable:", #plan.items, "unplaceable:", plan.skipped)

    -- Remove before adding, so a mark deleted and re-made at the same spot settles as one
    -- annotation rather than two.
    local removed = 0
    for _, index in ipairs(Annotations.indicesToRemove(mgr.annotations, plan.stale)) do
        local item = mgr.annotations[index]
        table.remove(mgr.annotations, index)
        ui:handleEvent(Event:new("AnnotationsModified", { item, nb_highlights_added = 0, index_modified = index }))
        removed = removed + 1
    end

    local existing = Annotations.existingKeys(mgr.annotations)
    local added = 0
    for _, item in ipairs(plan.items) do
        local key = Annotations.keyOf(item.pos0 or item.page, item.pos1)
        if not existing[key] then
            existing[key] = true
            local index = mgr:addItem(item)
            ui:handleEvent(Event:new("AnnotationsModified", { item, index_modified = index }))
            added = added + 1
        end
    end

    -- Written now rather than after the upload, so a change made here is not offered a
    -- second time on the next pull. A failed upload is the exception: commit(false) leaves
    -- the deletions owed, so they are retried rather than quietly abandoned.
    ledger.marks = plan.byKey
    ledger.pending = plan.pending
    ledger.stubs = {}
    ledger.clock = plan.clock
    Ledger.save(ui.doc_settings, ledger)

    Diag.log("result: added=" .. added, "removed=" .. removed)
    if added > 0 or removed > 0 then
        ui:handleEvent(Event:new("BookmarkUpdated"))
    end

    return {
        added = added,
        removed = removed,
        skipped = plan.skipped,
        sent = plan.sent,
        note = note,
        blob = plan.blob,
        refused = plan.refused,
        commit = function(ok)
            if ok then
                ledger.clock = plan.commitClock
                Ledger.save(ui.doc_settings, ledger)
                return
            end
            -- Nothing reached the server, so the deletions are still owed. They go back
            -- as owed rather than as live marks: the user deleted them, and resurrecting
            -- the annotations here just to delete them again on the next pull is the
            -- round trip this ledger exists to stop.
            for key, rec in pairs(plan.deletedRecords) do
                if rec then ledger.pending[key] = rec end
            end
            Ledger.save(ui.doc_settings, ledger)
        end,
    }
end

--[[--
Send the merged blob, if there is one, then continue.

@param done called exactly once, whatever happened -- the reading position is pulled
            after this either way.
]]
function CrossPointSync:push(creds, document, summary, done)
    if not summary or not summary.blob then
        if summary and summary.commit then summary.commit(true) end
        done()
        return
    end

    Diag.log("uploading under key:", tostring(document))
    local ok, encoded = pcall(function() return require("json").encode(summary.blob) end)
    if not ok or type(encoded) ~= "string" then
        Diag.log("could not encode the blob to upload:", tostring(encoded))
        summary.commit(false)
        summary.uploadFailed = true
        done()
        return
    end

    self.client:putBookmarks(creds.username, creds.userkey, document, encoded, function(sent, body, status)
        Diag.log("upload:", sent and "ok -- the server now holds this set"
                                  or ("FAILED, HTTP " .. tostring(status) .. "; deletions kept for the next pull"))
        summary.commit(sent)
        summary.uploadFailed = not sent
        local items = sent and type(body) == "table"
            and Push.quoteTexts(body.need_text, summary.blob, self.ui.annotation and self.ui.annotation.annotations)
        if not items or #items == 0 then
            done()
            return
        end
        -- Best effort: the server asks again on the next upload if this one is lost.
        self.client:putQuoteTexts(creds.username, creds.userkey, document, items, function(ok, _, textStatus)
            Diag.log("quote text:", #items, ok and "sent" or ("FAILED, HTTP " .. tostring(textStatus)))
            done()
        end)
    end)
end

--[[--
Show the one message a pull produces.

@param progressLine what became of the reading position, or nil to say nothing about it.
                    The caller decides, because an automatic pull that is about to raise a
                    confirmation has nothing to announce yet.
]]
function CrossPointSync:_report(summary, interactive, progressLine)
    self:_finish()
    summary = summary or { added = 0, removed = 0, skipped = 0 }
    local sent = summary.sent or 0
    local changed = summary.added > 0 or summary.removed > 0 or sent > 0
    local lines = {}
    -- `quiet` means annotations were not part of this sync at all, so every outcome
    -- below is zero by not having run and none of it is news.
    if not summary.quiet then
        if summary.added > 0 or summary.removed > 0 then
            lines[#lines + 1] = T(_("Annotations: %1 added, %2 removed."), summary.added, summary.removed)
        elseif changed then
            lines[#lines + 1] = _("Annotations: nothing new from the device.")
        elseif interactive then
            lines[#lines + 1] = summary.note or _("Annotations: already up to date.")
        end
    end
    -- Only reported when something was actually sent: saying "0 uploaded" on every pull
    -- would be noise, and an upload that changed nothing is not news.
    if sent > 0 then
        lines[#lines + 1] = summary.uploadFailed
            and T(_("%1 of your changes could not be uploaded."), sent)
            or T(_("Sent %1 of your changes to the device."), sent)
    end
    if summary.refused then
        lines[#lines + 1] = _("Your changes were not uploaded: the set would be too large for the device to hold.")
    end
    -- Only worth raising when the user is already being told something; on an automatic
    -- pull an unplaceable mark is a permanent property of this copy of the book, and
    -- would otherwise be reported on every open.
    if summary.skipped > 0 and (changed or interactive) then
        lines[#lines + 1] = T(_("%1 could not be placed in this copy of the book."), summary.skipped)
    end
    if progressLine then lines[#lines + 1] = progressLine end
    if #lines == 0 then return end

    local text = table.concat(lines, "\n")
    -- Parenthesised: gsub also returns a replacement count, which Diag.log would
    -- otherwise append to the line as a stray number.
    Diag.log("reported:", (text:gsub("\n", " / ")))
    UIManager:show(InfoMessage:new{ text = text, timeout = 3 })
end

--- Where this device currently is, in the shape Progress.decide compares against.
function CrossPointSync:_localProgress()
    local ui = self.ui
    local has_pages = ui.document and ui.document.info and ui.document.info.has_pages or false
    local view = has_pages and ui.paging or ui.rolling
    if not view then return { has_pages = has_pages } end
    local okProgress, progress = pcall(function() return view:getLastProgress() end)
    local okPercent, percentage = pcall(function() return view:getLastPercent() end)
    return {
        has_pages = has_pages,
        progress = okProgress and progress or nil,
        percentage = okPercent and percentage or nil,
    }
end

--- Navigate to a position the verdict has already validated.
function CrossPointSync:_gotoProgress(verdict, hasPages)
    local ui = self.ui
    -- Leave a return point, so a jump to the wrong place is one Back away from undone.
    if ui.link and ui.link.addCurrentLocationToStack then
        pcall(function() ui.link:addCurrentLocationToStack() end)
    end
    if hasPages then
        ui:handleEvent(Event:new("GotoPage", verdict.target))
    else
        ui:handleEvent(Event:new("GotoXPointer", verdict.target))
    end
    Diag.log("position: moved to", tostring(verdict.target))
end

-- Why the position did not move, in the user's words. Progress.decide reports a code so
-- that the decision itself stays testable without KOReader's gettext.
local PROGRESS_REASON = {
    [Progress.NO_REPLY] = _("the server did not answer"),
    [Progress.NO_RECORD] = _("the server has no position for this book"),
    [Progress.NOT_USABLE] = _("the stored position does not fit this copy of the book"),
    [Progress.SAME_PLACE] = _("you are already there"),
}

--[[--
Fetch the stored reading position, trying each candidate document key in turn.

@param summary what the annotation pass did, carried through so both halves of the pull
               are reported together.
]]
function CrossPointSync:pullProgress(creds, keys, index, interactive, summary)
    if not self.settings.sync_progress then
        Diag.log("reading position: not pulled (turned off)")
        -- Send-only with annotations off would otherwise report nothing at all for a sync
        -- the user asked for: the push that follows in _finish is the whole of the work.
        self:_report(summary, interactive,
            interactive and self._pushAfterSync and _("Reading position: sending yours.") or nil)
        return
    end

    local document = keys[index]
    self.client:getProgress(creds.username, creds.userkey, document, function(ok, body, status)
        if not ok then
            Diag.log("position request failed; HTTP status:", tostring(status))
            self:_report(summary, interactive,
                interactive and _("The reading position could not be fetched.") or nil)
            return
        end
        local stored = type(body) == "table" and body.progress or nil
        if (stored == nil or stored == "") and keys[index + 1] then
            Diag.log("no position under that key; retrying with the other match mode:", keys[index + 1])
            self:pullProgress(creds, keys, index + 1, interactive, summary)
            return
        end
        -- A push goes back under the key the position was actually found at, which is not
        -- necessarily the configured one when the fallback match mode answered.
        self._progressKey = document
        self:applyProgress(body, interactive, summary)
    end)
end

function CrossPointSync:applyProgress(body, interactive, summary)
    local here = self:_localProgress()
    local herePct = tonumber(here.percentage)
    Diag.log("position here :", tostring(here.progress),
        herePct and string.format("(%.2f%%)", herePct * 100) or "")
    if type(body) == "table" then
        local therePct = tonumber(body.percentage)
        Diag.log("position there:", tostring(body.progress),
            therePct and string.format("(%.2f%%)", therePct * 100) or "",
            body.device and ("on " .. tostring(body.device)) or "")
    end

    local verdict = Progress.decide(body, here)
    if verdict.action ~= "jump" then
        Diag.log("position: staying put --", tostring(verdict.reason))
        -- Already agreed, so there is nothing to send until the reader moves.
        if verdict.reason == Progress.SAME_PLACE then self._sentProgress = here.progress end
        -- Still set: _finish clears it, and _report is what calls _finish. Saying the
        -- server has none would otherwise be the last word on a sync that is about to
        -- put one there.
        local line = nil
        if interactive then
            if verdict.reason == Progress.NO_RECORD and self._pushAfterSync then
                line = _("Reading position: the device has none yet; sending yours.")
            else
                local reason = PROGRESS_REASON[verdict.reason]
                if reason then line = T(_("Reading position: %1."), reason) end
            end
        end
        self:_report(summary, interactive, line)
        return
    end

    -- An anchor from a different edition resolves to nothing, and jumping on it would
    -- land somewhere arbitrary; refused the same way an annotation anchor is.
    local doc = self.ui.document
    if not here.has_pages and doc.isXPointerInDocument and not doc:isXPointerInDocument(verdict.target) then
        Diag.log("position: anchor does not resolve in this book:", tostring(verdict.target))
        self:_report(summary, interactive,
            interactive and _("The device's reading position is not in this copy of the book.") or nil)
        return
    end

    -- Whether or not the jump is taken, the stored position is now known here, so it is
    -- what this book has agreed on: taking it means we are there, and declining it -- or
    -- closing the book with the prompt still up -- must not overwrite it with a position
    -- the reader never chose. Reading on changes `progress` and lifts the suppression.
    self._sentProgress = verdict.target

    local pct = verdict.percentage and math.floor(verdict.percentage * 100 + 0.5) or nil
    if interactive then
        self:_gotoProgress(verdict, here.has_pages)
        self:_report(summary, true,
            pct and T(_("Reading position: moved to %1%."), pct) or _("Reading position: moved."))
        return
    end

    -- An automatic pull runs as the book opens, so it asks first: a silent jump then is
    -- indistinguishable from having lost your place.
    self:_report(summary, false, nil)
    local device = verdict.device or _("the other device")
    UIManager:show(ConfirmBox:new{
        text = pct and T(_("Go to the position read on %1 (%2%)?"), device, pct)
                    or T(_("Go to the position read on %1?"), device),
        ok_text = _("Go"),
        ok_callback = function() self:_gotoProgress(verdict, here.has_pages) end,
    })
end

--[[--
Send this device's reading position.

@param interactive true when the user asked for it; only then is the outcome shown, and
                   only then is an unchanged position still worth reporting.
]]
function CrossPointSync:pushProgress(interactive)
    if not interactive and not self.settings.push_progress then return end
    if not self.ui or not self.ui.document then return end

    local here = self:_localProgress()
    local ok, why = Progress.shouldPush(here, self._sentProgress)
    if not ok then
        Diag.log("position push: skipped --", tostring(why))
        if interactive then
            local reason = why == Progress.SAME_PLACE
                and _("The server already has this position.")
                or _("There is no position to send for this book.")
            UIManager:show(InfoMessage:new{ text = reason, timeout = 3 })
        end
        return
    end

    local creds = self:getCredentials()
    if not creds or not creds.server then
        Diag.log("position push: no credentials or no server")
        if interactive then
            UIManager:show(InfoMessage:new{
                text = _("No login. Set one under \"Server and login\"."), timeout = 3 })
        end
        return
    end

    -- The key the pull confirmed, falling back to the configured one when no pull has
    -- run for this book -- a first push must still go somewhere the device will look.
    local document = self._progressKey or DocId.forDocument(self.ui, self.settings.filename_mode)
    if not document then
        Diag.log("position push: could not identify this book")
        return
    end

    -- Read before the request, not inside the callback: a push on close races the
    -- document's teardown, and self.ui.document is gone by the time the reply lands.
    local record = {
        document = document,
        progress = here.progress,
        percentage = Progress.roundPercent(here.percentage),
        device = Device.model,
        device_id = self.device_id,
    }
    Diag.log("position push:", tostring(record.progress),
        string.format("(%.2f%%)", (record.percentage or 0) * 100), "under", tostring(document))

    self:_ensureClient(creds)
    self.client:putProgress(creds.username, creds.userkey, record, function(sent, _body, status)
        if sent then
            -- Only on success: a failed push must stay pending, or a dropped connection
            -- would silently cost this reading session its position.
            self._sentProgress = record.progress
            self._turnsSincePush = 0
            Diag.log("position push: ok")
        else
            Diag.log("position push: FAILED, HTTP", tostring(status))
        end
        if interactive then
            UIManager:show(InfoMessage:new{
                text = sent and _("Reading position sent.") or _("The reading position could not be sent."),
                timeout = 3,
            })
        end
    end)
end

return CrossPointSync
