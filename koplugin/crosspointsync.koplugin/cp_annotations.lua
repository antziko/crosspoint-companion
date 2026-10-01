--[[--
Translation between CrossPoint's bookmark records and KOReader annotations.

A CrossPoint mark is anchored by `(spineIndex, progress, paragraphIndex)`, or for a
highlight by a page-local word range, none of which mean anything to crengine. What this
module reads is the `xp` (and, for a highlight, `xp1`) the device resolves for each mark:
real XPointers into the same document. A record without one is skipped, never
approximated -- an annotation on the wrong sentence is worse than a missing one.

Records are kept whole rather than reduced to the two anchors. Writing back needs the
identity fields the device keys on (`s`, `pi`, `q`, `sw`, `ew`), the version `v` that
orders a delete against a create, and every field this plugin does not understand --
dropping one of those on a rewrite would strip it from the device's own record.

Everything here is pure data manipulation over an injected document, so it is testable
without a running KOReader.
]]

local Lamport = require("cp_lamport")

local Annotations = {}

--- Identity of an annotation, for dedup and for matching the ledger.
function Annotations.keyOf(pos0, pos1)
    return tostring(pos0) .. "|" .. tostring(pos1 or "")
end

--[[--
The XPointer pair a KOReader annotation is anchored by.

A highlight carries `pos0`/`pos1`. A plain bookmark carries its XPointer in `page`;
older records sometimes hold a page number there, which is not an anchor.
]]
function Annotations.anchorsOf(item)
    if type(item) ~= "table" then return nil, nil end
    local pos0 = type(item.pos0) == "string" and item.pos0
        or (type(item.page) == "string" and item.page or nil)
    local pos1 = type(item.pos1) == "string" and item.pos1 or nil
    return pos0, pos1
end

--- Keys of the annotations already in the document, so a re-pull adds nothing twice.
function Annotations.existingKeys(annotations)
    local keys = {}
    for _, item in ipairs(annotations or {}) do
        local pos0, pos1 = Annotations.anchorsOf(item)
        if pos0 then keys[Annotations.keyOf(pos0, pos1)] = true end
    end
    return keys
end

--[[--
Build the KOReader items a set of records asks for.

@param records   list of device records (the blob's `b` array, or the live winners of a
                 merge)
@param doc       the open document (isXPointerInDocument, getPageFromXPointer,
                 getTextFromXPointers)
@param toc       optional; used only for the chapter label
@param datetime  timestamp string to stamp the items with -- CrossPoint devices have no
                 clock, so their marks carry none and the pull time is the best available
@return items    list of annotation items, in record order
@return byKey    map of annotation key -> the record that produced it
@return skipped  how many records named an anchor this document could not resolve
]]
function Annotations.build(records, doc, toc, datetime)
    local Diag = require("cp_diag")
    local items = {}
    local byKey = {}
    local skipped = 0

    for i, rec in ipairs(records or {}) do
        local pos0 = type(rec.xp) == "string" and rec.xp or nil
        if not pos0 then
            -- A mark the device could not anchor: it is still a real bookmark on the
            -- device, just not one that can be placed here.
            Diag.log(string.format("  [%d] NO ANCHOR (device sent no xp); spine=%s snippet=%q",
                i, tostring(rec.s), tostring(rec.sn or ""):sub(1, 40)))
            skipped = skipped + 1
        elseif doc.isXPointerInDocument and not doc:isXPointerInDocument(pos0) then
            -- The anchor does not resolve -- a different edition, or a DOM version this
            -- document was opened with that normalises XPointers differently.
            Diag.log(string.format("  [%d] ANCHOR NOT IN THIS BOOK: %s", i, pos0))
            skipped = skipped + 1
        else
            local is_quote = Lamport.isQuote(rec)
            local pos1 = is_quote and type(rec.xp1) == "string" and rec.xp1 or nil
            if is_quote and pos1 and doc.isXPointerInDocument and not doc:isXPointerInDocument(pos1) then
                Diag.log(string.format("  [%d] end anchor unresolved, keeping start only: %s", i, pos1))
                pos1 = nil  -- keep the start, drop a zero-width or inverted range
            end
            Diag.log(string.format("  [%d] %s ok (v=%s): %s",
                i, is_quote and "highlight" or "bookmark", tostring(rec.v or 0), pos0))

            local key = Annotations.keyOf(pos0, is_quote and pos1 or nil)
            if not byKey[key] then
                byKey[key] = rec
                local pageno = doc.getPageFromXPointer and doc:getPageFromXPointer(pos0) or nil
                local chapter = toc and toc.getTocTitleByPage and toc:getTocTitleByPage(pos0) or nil
                if chapter == "" then chapter = nil end

                local item
                if is_quote and pos1 then
                    -- crengine re-derives the highlighted text from the pair, so the blob
                    -- never carries it; `sn` is only the device's 64-char teaser and is
                    -- used as a fallback when the document cannot supply the text.
                    local text
                    if doc.getTextFromXPointers then
                        text = doc:getTextFromXPointers(pos0, pos1)
                    end
                    if not text or text == "" then text = rec.sn end
                    item = {
                        pos0 = pos0,
                        pos1 = pos1,
                        page = pos0,
                        text = text or "",
                        drawer = "lighten",
                        chapter = chapter,
                        pageno = pageno,
                        datetime = datetime,
                        datetime_updated = datetime,
                    }
                else
                    -- No drawer and an XPointer in `page` is how KOReader stores a plain
                    -- bookmark. A highlight whose end anchor is missing lands here too,
                    -- as a bookmark at its start rather than as a mis-sized highlight.
                    item = {
                        page = pos0,
                        text = rec.sn or "",
                        chapter = chapter,
                        pageno = pageno,
                        datetime = datetime,
                        datetime_updated = datetime,
                    }
                end
                items[#items + 1] = item
            end
        end
    end

    return items, byKey, skipped
end

--[[--
What the user has done here since the last sync.

@param marks       ledger records, annotation key -> record
@param stubs       ledger keys carried over from a pull-only release, with no record
@param annotations `ui.annotation.annotations`
@return created    list of `{ key = , item = }`, in document order -- annotations this
                   plugin has never seen, so they were made here
@return deleted    list of ledger keys whose annotation is gone and whose record is known,
                   so a tombstone can be emitted for them
@return orphaned   list of stub keys whose annotation is gone. Known to have been ours,
                   but with no identity recorded there is nothing to tombstone; the mark
                   comes back on the next pull. One window only, and only for books
                   synced before the ledger existed.

An edit -- a note added, a drawer changed -- is deliberately not a change: the device
stores neither, so there would be nothing to send.
]]
function Annotations.localChanges(marks, stubs, annotations)
    marks, stubs = marks or {}, stubs or {}
    local present = {}
    local created = {}

    for _, item in ipairs(annotations or {}) do
        local pos0, pos1 = Annotations.anchorsOf(item)
        if pos0 then
            local key = Annotations.keyOf(pos0, pos1)
            if not present[key] then
                present[key] = true
                if marks[key] == nil and stubs[key] == nil then
                    created[#created + 1] = { key = key, item = item }
                end
            end
        end
    end

    local deleted, orphaned = {}, {}
    for key in pairs(marks) do
        if not present[key] then deleted[#deleted + 1] = key end
    end
    for key in pairs(stubs) do
        if not present[key] then orphaned[#orphaned + 1] = key end
    end
    -- pairs() has no defined order, and these drive what goes on the wire.
    table.sort(deleted)
    table.sort(orphaned)
    return created, deleted, orphaned
end

--[[--
Indices in `annotations` whose key is in `stale`, highest first.

Descending, because the caller removes them one at a time and a lower index would shift
every one after it.
]]
function Annotations.indicesToRemove(annotations, stale)
    local out = {}
    for i, item in ipairs(annotations or {}) do
        local pos0, pos1 = Annotations.anchorsOf(item)
        if pos0 and stale[Annotations.keyOf(pos0, pos1)] then
            out[#out + 1] = i
        end
    end
    table.sort(out, function(a, b) return a > b end)
    return out
end

return Annotations
