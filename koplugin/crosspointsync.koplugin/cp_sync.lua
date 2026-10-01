--[[--
One pull, decided.

This is the whole reconciliation: what the server holds, what this plugin last recorded,
and what the user has since done here, resolved into the annotations to add, the ones to
take out, the new ledger, and the blob to send back. It is deliberately not part of the
plugin widget -- the rule it implements is the firmware's merge, the failure modes are
data loss in both directions, and neither is something to leave only testable by hand on
two devices.

Pure: the document, the table of contents and the record factory are injected.
]]

local Annotations = require("cp_annotations")
local Lamport = require("cp_lamport")
local Push = require("cp_push")

local Sync = {}

--[[--
Does the server still hold a live record for a deletion this side is owed?

By spot, and by anchor as well: a device that adopts a point bookmark re-files it under
its own paragraph numbering, so the record that has to die may no longer carry the spot
the ledger recorded for it. The XPointer is what survives that.
]]
local function serverStillHolds(key, rec, remoteB)
    for _, other in ipairs(remoteB) do
        if Lamport.spotMatch(rec, other) then return true end
        if type(other.xp) == "string" then
            local full = Annotations.keyOf(other.xp, Lamport.isQuote(other) and other.xp1 or nil)
            if full == key or Annotations.keyOf(other.xp, nil) == key then return true end
        end
    end
    return false
end

--- How a record's identity reads in the trace, in the device's own terms.
local function spotOf(rec)
    if Lamport.isQuote(rec) then
        return string.format("quote s=%s w=%s-%s", tostring(rec.s), tostring(rec.sw), tostring(rec.ew))
    end
    local pi = tonumber(rec.pi) or Lamport.NO_PARAGRAPH
    if pi ~= Lamport.NO_PARAGRAPH then
        return string.format("mark s=%s para=%s", tostring(rec.s), tostring(pi))
    end
    return string.format("mark s=%s p=%s", tostring(rec.s), tostring(Lamport.quantize(rec.p) / 1000))
end

--[[--
Work out what this pull should do.

@param input
  `remoteB`, `remoteT`  the blob's live records and tombstones (either may be absent)
  `marks`, `stubs`      the ledger, as `cp_ledger` loads it
  `pending`             deletions made here that have not been uploaded yet
  `clock`               the ledger's Lamport clock
  `annotations`         `ui.annotation.annotations`
  `sending`             whether changes made here may be uploaded
  `doc`, `toc`          for resolving anchors and labelling chapters
  `datetime`            timestamp to stamp inserted annotations with
  `newRecord`           `function(item, version) -> record or nil`, for a mark made here
  `log`                 optional `function(...)` trace sink

@return plan
  `items`            annotations to insert, in record order
  `stale`            set of annotation keys to remove
  `byKey`            the new ledger records, annotation key -> record
  `skipped`          records whose anchor does not resolve in this copy of the book
  `clock`            the clock advanced past everything seen
  `stamp`            the version given to changes made here (`clock + 1`)
  `created`, `deleted`, `orphaned`  what the user did here since the last pull
  `revived`          pending deletions whose annotation is back, so nothing is sent
  `settled`          owed deletions the server has stopped holding, now dropped
  `pending`          the deletions still owed to the server after this pull -- every spot
                     buried here, cleared only by a later pull that shows it gone
  `deletedRecords`   key -> record for every spot buried this pull, so a failed upload
                     can leave them owed rather than forgetting them
  `commitClock`      the clock to record once the upload has been dealt with. Only a
                     pull that actually stamped something advances it; a pull that sent
                     nothing must leave it alone, or it climbs by one every time
  `blob`             what to PUT, or nil when there is nothing to send
  `refused`          why no blob was produced, when a set was too large to send
  `sent`             how many of the user's own changes the blob carries
]]
function Sync.plan(input)
    local log = input.log or function() end
    local remoteB = input.remoteB or {}
    local remoteT = input.remoteT or {}
    local stubs = input.stubs or {}
    local sending = input.sending and true or false

    -- A deletion is owed until a pull shows the spot gone from the server, not merely
    -- until an upload is accepted. A tombstone can be accepted and still lose: a device
    -- that raised the record's version without publishing it outranks a stamp made from
    -- what the server showed, and the merge gives a tie to the live bookmark. Dropping
    -- the deletion at that point is what lets the next pull put the annotation back.
    --
    -- Reviving one is the user re-making the mark at the same spot: the server still
    -- holds that record, so the right answer is to track it again rather than send a
    -- second one for the same spot.
    local here = Annotations.existingKeys(input.annotations)
    local marks, pending, revived, settled = {}, {}, {}, {}
    for key, rec in pairs(input.marks or {}) do marks[key] = rec end
    for key, rec in pairs(input.pending or {}) do
        if here[key] then
            marks[key] = rec
            revived[#revived + 1] = key
        elseif serverStillHolds(key, rec, remoteB) then
            pending[key] = rec
        else
            settled[#settled + 1] = key
        end
    end
    table.sort(revived)
    table.sort(settled)

    -- The clock must clear everything this pull can see before anything new is stamped:
    -- a tombstone that ties with the bookmark it is meant to bury loses, because a tie
    -- goes to the live bookmark.
    local known = 0
    for _ in pairs(marks) do known = known + 1 end
    local stubbed = 0
    for _ in pairs(stubs) do stubbed = stubbed + 1 end
    local owed = 0
    for _ in pairs(pending) do owed = owed + 1 end
    log(string.format("merge inputs: remote b=%d t=%d, ledger=%d untracked=%d owed-deletes=%d, uploading %s",
        #remoteB, #remoteT, known, stubbed, owed, sending and "ON" or "off"))
    if #revived > 0 then
        log("deletion taken back (the mark is here again): " .. table.concat(revived, ", "))
    end
    if #settled > 0 then
        log("deletion confirmed, the server no longer holds it: " .. table.concat(settled, ", "))
    end

    local clock = input.clock or 0
    for _, rec in ipairs(remoteB) do clock = Lamport.observe(clock, rec.v) end
    for _, rec in ipairs(remoteT) do clock = Lamport.observe(clock, rec.v) end
    for _, rec in pairs(marks) do clock = Lamport.observe(clock, rec.v) end
    for _, rec in pairs(pending) do clock = Lamport.observe(clock, rec.v) end
    local stamp = clock + 1

    local created, deleted, orphaned = Annotations.localChanges(marks, stubs, input.annotations)
    log(string.format("here since last sync: %d new, %d deleted, %d untracked-deleted; clock=%d",
        #created, #deleted, #orphaned, clock))
    if #orphaned > 0 then
        -- Carried over from a release that recorded keys but no identity, so there is
        -- nothing to tombstone. One pull only: the rest of the set has real records now.
        log("cannot tombstone (no identity recorded): " .. table.concat(orphaned, ", "))
    end

    -- Remote entries first, so a record the device has since re-anchored supersedes this
    -- side's older copy at the same version rather than the other way round.
    local entries = {}
    for _, rec in ipairs(remoteB) do entries[#entries + 1] = { rec = rec, v = tonumber(rec.v) or 0, live = true } end
    for _, rec in ipairs(remoteT) do entries[#entries + 1] = { rec = rec, v = tonumber(rec.v) or 0, live = false } end

    -- Every spot this pull buries: deleted here since the last sync, plus deletions
    -- carried from syncs that were not allowed to upload. A tombstone is raised for both
    -- whether or not it can be sent yet -- burying it locally is what stops the next pull
    -- putting the annotation back, and `pending` is what remembers it is still owed.
    local buried = {}
    for _, key in ipairs(deleted) do buried[key] = marks[key] end
    for key, rec in pairs(pending) do buried[key] = rec end

    -- A mark made here that a device has since adopted comes back under that device's own
    -- coordinates: the same XPointer, a different spot. The ledger still names the spot
    -- the mark had before the adoption, so burying that alone leaves the device's copy
    -- live and the next pull simply puts the annotation back. A deletion is therefore
    -- resolved by anchor as well -- the one identity both sides agree on -- and every
    -- record naming a buried anchor goes down with it.
    local adopted = {}
    for _, rec in ipairs(remoteB) do
        if type(rec.xp) == "string" then
            local full = Annotations.keyOf(rec.xp, Lamport.isQuote(rec) and rec.xp1 or nil)
            -- A highlight whose end anchor did not resolve here was placed, and so is
            -- tracked, under the shorter key.
            local short = Annotations.keyOf(rec.xp, nil)
            if (buried[full] or buried[short]) and not here[full] and not here[short] then
                adopted[#adopted + 1] = rec
            end
        end
    end

    for key, rec in pairs(marks) do
        if buried[key] then
            log(string.format("  tombstoning %s at v=%d (was v=%s) -- deleted here%s",
                spotOf(rec), stamp, tostring(rec.v), sending and "" or ", held until uploading is on"))
            entries[#entries + 1] = { rec = rec, v = stamp, live = false }
        else
            entries[#entries + 1] = { rec = rec, v = tonumber(rec.v) or 0, live = true }
        end
    end
    for key, rec in pairs(pending) do
        log(string.format("  tombstoning %s at v=%d (was v=%s) -- deleted here earlier%s",
            spotOf(rec), stamp, tostring(rec.v), sending and "" or ", still held"))
        entries[#entries + 1] = { rec = rec, v = stamp, live = false }
    end
    for _, rec in ipairs(adopted) do
        -- Already buried at its old spot above; this is the same deletion following the
        -- mark to where the device filed it, so it is not counted as a second change.
        log(string.format("  tombstoning %s at v=%d (was v=%s) -- a device's copy of a mark deleted here",
            spotOf(rec), stamp, tostring(rec.v)))
        entries[#entries + 1] = { rec = rec, v = stamp, live = false }
    end

    local madeHere = 0
    if sending and input.newRecord then
        for _, c in ipairs(created) do
            local rec = input.newRecord(c.item, stamp)
            if rec then
                log(string.format("  sending %s at v=%d -- made here: %s", spotOf(rec), stamp, tostring(c.key)))
                entries[#entries + 1] = { rec = rec, v = stamp, live = true }
                madeHere = madeHere + 1
            else
                log("cannot send (no usable anchor): " .. tostring(c.key))
            end
        end
    end

    local winners = Lamport.merge(entries)

    -- Live winners are what this document should hold. Dead ones name what to take out,
    -- but only where an anchor is known: a tombstone straight off the wire carries none,
    -- which is exactly why this can only ever remove marks it put here itself.
    local liveRecords, deadKeys = {}, {}
    for _, w in ipairs(winners) do
        if w.live then
            liveRecords[#liveRecords + 1] = w.rec
        elseif type(w.rec.xp) == "string" then
            deadKeys[Annotations.keyOf(w.rec.xp, Lamport.isQuote(w.rec) and w.rec.xp1 or nil)] = true
            -- A highlight whose end anchor did not resolve here was inserted as a plain
            -- bookmark, under the shorter key.
            deadKeys[Annotations.keyOf(w.rec.xp, nil)] = true
        end
    end

    local deadCount = 0
    for _ in pairs(deadKeys) do deadCount = deadCount + 1 end
    log(string.format("merged to %d spots: live=%d dead=%d (%d dead spots are placeable here)",
        #winners, #liveRecords, #winners - #liveRecords, deadCount))

    local items, byKey, skipped = Annotations.build(liveRecords, input.doc, input.toc, input.datetime)

    -- A mark a device adopts comes back re-keyed: the synthetic spot KOReader invented is
    -- tombstoned and a native-keyed record takes its place, both naming the same
    -- XPointer. Removing by spot alone would delete the annotation the new record claims,
    -- so a key some live winner still holds is never a removal.
    local stale = {}
    for key in pairs(deadKeys) do
        if not byKey[key] then
            stale[key] = true
            log("  removing here: " .. key)
        else
            -- The spot died but its anchor lives on under a new identity: a device has
            -- adopted this mark. Re-keyed, not removed.
            log("  re-keyed by a device, keeping the annotation: " .. key)
        end
    end

    local buriedCount = 0
    for _ in pairs(buried) do buriedCount = buriedCount + 1 end

    local plan = {
        items = items,
        stale = stale,
        byKey = byKey,
        skipped = skipped,
        clock = clock,
        stamp = stamp,
        created = created,
        deleted = deleted,
        orphaned = orphaned,
        revived = revived,
        settled = settled,
        deletedRecords = buried,
        sent = 0,
        -- Nothing was stamped unless a blob comes out of this; see below.
        commitClock = clock,
        -- Every deletion this pull buries stays owed, whatever becomes of the upload.
        -- Held as whole records, because once the annotation is gone the ledger is the
        -- only place the device identity still exists. The next pull clears the ones the
        -- server has stopped holding; see `settled` above.
        pending = buried,
    }
    if not sending then
        if buriedCount > 0 then
            log(string.format("%d deletion(s) held until uploading is on", buriedCount))
        end
        return plan
    end

    local blob, refused = Push.blob(winners)
    if not blob then
        log("upload refused: " .. tostring(refused))
        plan.refused = refused
        return plan
    end
    if not Push.differs(blob, remoteB, remoteT) then
        log("nothing to upload: the server already holds this set")
        return plan
    end
    plan.blob = blob
    plan.sent = madeHere + buriedCount
    plan.commitClock = stamp
    log(string.format("uploading b=%d t=%d (%d of them changes made here)", #blob.b, #blob.t, plan.sent))
    return plan
end

return Sync
