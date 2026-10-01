--[[--
What this plugin knows about each mark, between one sync and the next.

Reconciling by Lamport version needs more than the set of keys the old `_seen` setting
held. To emit a tombstone the device will honour, this side has to reproduce the mark's
device identity -- its spine, paragraph or word range -- and a version strictly above the
bookmark's. To rewrite the blob without damaging it, it also has to carry back every
field it does not itself understand. So the ledger stores the whole record, keyed by the
KOReader annotation it produced.

It lives in the book's own sidecar (`ui.doc_settings`), not in the global settings: the
clock is per book, exactly as the device's is.

Pure apart from the setting store, which is injected, so it tests without KOReader.
]]

local Ledger = {}

Ledger.MARKS_KEY = "crosspointsync_marks"
Ledger.CLOCK_KEY = "crosspointsync_clock"
-- Marks deleted here that the server has not been told about, because uploading was off
-- at the time. Held as whole records, for the same reason marks are: a tombstone needs
-- the device identity, and the record is the only place it exists once the annotation is
-- gone. Flushed on the first sync that may upload.
Ledger.PENDING_KEY = "crosspointsync_pending"
-- What the pull-only releases wrote: a set of annotation keys, no versions, no identity.
Ledger.LEGACY_KEY = "crosspointsync_seen"

--[[--
Read the ledger for a book.

An older `_seen` set is carried over as `stubs`: keys this plugin is known to have
inserted, with nothing else recorded about them. A stub is enough to keep the plugin from
mistaking its own annotation for one the user made, and it is replaced by the real record
on the first pull that finds the mark still on the server. Until then it cannot be
tombstoned -- there is no identity to tombstone -- so a mark deleted here in that one
window is simply re-pulled.

@return `{ marks = {key -> record}, pending = {key -> record}, stubs = {key -> true},
          clock = <number> }`
]]
function Ledger.load(doc_settings)
    local ledger = { marks = {}, pending = {}, stubs = {}, clock = 0 }
    if not doc_settings then return ledger end

    local marks = doc_settings:readSetting(Ledger.MARKS_KEY)
    if type(marks) == "table" then
        for key, rec in pairs(marks) do
            if type(rec) == "table" then ledger.marks[key] = rec end
        end
    end

    local pending = doc_settings:readSetting(Ledger.PENDING_KEY)
    if type(pending) == "table" then
        for key, rec in pairs(pending) do
            if type(rec) == "table" then ledger.pending[key] = rec end
        end
    end

    local legacy = doc_settings:readSetting(Ledger.LEGACY_KEY)
    if type(legacy) == "table" then
        for key, seen in pairs(legacy) do
            if seen and ledger.marks[key] == nil then ledger.stubs[key] = true end
        end
    end

    ledger.clock = tonumber(doc_settings:readSetting(Ledger.CLOCK_KEY)) or 0
    return ledger
end

--- Write the ledger back, and retire the legacy set once its keys have been absorbed.
function Ledger.save(doc_settings, ledger)
    if not doc_settings or not ledger then return end
    doc_settings:saveSetting(Ledger.MARKS_KEY, ledger.marks or {})
    doc_settings:saveSetting(Ledger.CLOCK_KEY, ledger.clock or 0)
    -- Dropped rather than written empty: nothing owed is the ordinary state, and a book
    -- that never deletes anything should not carry the key at all.
    if next(ledger.pending or {}) == nil then
        doc_settings:delSetting(Ledger.PENDING_KEY)
    else
        doc_settings:saveSetting(Ledger.PENDING_KEY, ledger.pending)
    end
    if next(ledger.stubs or {}) == nil then
        doc_settings:delSetting(Ledger.LEGACY_KEY)
    end
end

--- Drop everything recorded for this book, leaving the annotations themselves alone.
function Ledger.forget(doc_settings)
    if not doc_settings then return end
    doc_settings:delSetting(Ledger.MARKS_KEY)
    doc_settings:delSetting(Ledger.PENDING_KEY)
    doc_settings:delSetting(Ledger.CLOCK_KEY)
    doc_settings:delSetting(Ledger.LEGACY_KEY)
end

--- Is this annotation one the plugin inserted, whether or not it has a full record yet?
function Ledger.manages(ledger, key)
    if not ledger then return false end
    return ledger.marks[key] ~= nil or ledger.stubs[key] == true
end

--- Every record the ledger holds, as merge entries. Stubs contribute nothing: they have
--- no identity to match a spot on.
function Ledger.entries(ledger)
    local out = {}
    for _, rec in pairs((ledger or {}).marks or {}) do
        out[#out + 1] = { rec = rec, v = tonumber(rec.v) or 0, live = true }
    end
    return out
end

return Ledger
