--[[--
Building the blob that goes back to the server.

Two kinds of record leave here. Most are records that arrived from a device and are being
passed back unchanged, because the blob is rewritten whole on every PUT and anything
dropped is dropped for every peer -- including fields this plugin has no idea about. The
rest are marks made in KOReader, which need a device identity invented for them; see
`cp_lamport` for why a synthetic one is sound.

Nothing here talks to KOReader. What it cannot derive from an XPointer alone -- the spine
item's page span, the chapter title -- arrives through the injected `ctx`.
]]

local Lamport = require("cp_lamport")
local Annotations = require("cp_annotations")

local Push = {}

-- The device's combined cap on bookmarks and tombstones per book (MAX_BOOKMARKS).
Push.MAX_RECORDS = 128
-- BOOKMARK_CHAPTER_TITLE_MAX and BOOKMARK_SNIPPET_MAX, less their NUL.
Push.CHAPTER_MAX = 47
Push.SNIPPET_MAX = 63

--- Trim to at most `limit` bytes without splitting a UTF-8 sequence.
function Push.trim(text, limit)
    if type(text) ~= "string" then return "" end
    if #text <= limit then return text end
    local cut = limit
    -- Back off over continuation bytes (10xxxxxx) to the start of the character.
    while cut > 0 do
        local b = text:byte(cut + 1)
        if not b or b < 0x80 or b >= 0xC0 then break end
        cut = cut - 1
    end
    return text:sub(1, cut)
end

--[[--
The 0-based spine item an XPointer names.

crengine numbers one DocFragment per spine itemref from 1, and omits the index entirely
in a single-spine book. Mirrors `DocFragmentPath::find`.
]]
function Push.spineIndexOf(xpointer)
    if type(xpointer) ~= "string" then return nil end
    local at = xpointer:find("/body/DocFragment", 1, true)
    if not at then return nil end
    local after = at + #"/body/DocFragment"
    if xpointer:sub(after, after) ~= "[" then
        return 0  -- the indexless single-spine form
    end
    local close = xpointer:find("]", after + 1, true)
    if not close or close == after + 1 then return nil end
    local digits = xpointer:sub(after + 1, close - 1)
    if digits:match("^%d+$") == nil then return nil end
    local n = tonumber(digits)
    if n < 1 then return nil end
    return n - 1
end

--[[--
Turn a KOReader annotation into a device record.

@param item    the annotation
@param version the Lamport stamp to give it
@param ctx     `{ progressOf(pos0, spine) -> 0..1 or nil, chapterOf(pos0) -> string }`,
               both optional
@return the record, or nil when the annotation carries no usable anchor
]]
function Push.newRecord(item, version, ctx)
    ctx = ctx or {}
    local pos0, pos1 = Annotations.anchorsOf(item)
    if not pos0 then return nil end
    local spine = Push.spineIndexOf(pos0)
    if not spine then return nil end

    local isQuote = pos1 ~= nil and pos1 ~= pos0
    -- Advisory only: the device recomputes it from `xp` when it adopts the mark, and it
    -- is not part of the identity, because a KOReader-born mark always has an anchor.
    local progress = ctx.progressOf and tonumber(ctx.progressOf(pos0, spine)) or nil
    if not progress or progress < 0 or progress > 1 then progress = 0 end
    local chapter = (ctx.chapterOf and ctx.chapterOf(pos0)) or item.chapter or ""

    local rec = {
        s = spine,
        p = progress,
        v = version,
        ct = Push.trim(tostring(chapter), Push.CHAPTER_MAX),
        sn = Push.trim(tostring(item.text or ""), Push.SNIPPET_MAX),
        -- The page snapshot is display-only on the device ("page X of Y" in its bookmark
        -- list); 0 means unknown, which is honest here.
        cp = 0,
        pc = 0,
        xp = pos0,
    }
    if isQuote then
        local sw, ew = Lamport.syntheticQuoteKey(pos0, pos1)
        rec.q = true
        rec.es = spine
        rec.ep = progress
        rec.sw = sw
        rec.ew = ew
        rec.xp1 = pos1
        -- A quote is keyed by its word range, so it carries no paragraph anchor.
        rec.pi = Lamport.NO_PARAGRAPH
    else
        rec.pi = Lamport.syntheticParagraph(pos0)
    end
    return rec
end

--- The tombstone fields for a spot. Only these: the device reads no others from `t`.
function Push.tombstoneOf(rec, version)
    local t = {
        s = tonumber(rec.s) or 0,
        pi = tonumber(rec.pi) or Lamport.NO_PARAGRAPH,
        p = tonumber(rec.p) or 0,
        v = version,
    }
    if Lamport.isQuote(rec) then
        t.q = true
        t.sw = tonumber(rec.sw) or 0
        t.ew = tonumber(rec.ew) or 0
    end
    return t
end

--[[--
Serialise merge winners into the wire blob.

@param winners from `Lamport.merge`
@return blob `{ b = {...}, t = {...} }`, or nil plus a reason
]]
function Push.blob(winners)
    local b, t = {}, {}
    for _, w in ipairs(winners or {}) do
        if w.live then
            local rec = {}
            for k, v in pairs(w.rec) do rec[k] = v end
            rec.v = w.v
            b[#b + 1] = rec
        else
            t[#t + 1] = Push.tombstoneOf(w.rec, w.v)
        end
    end
    -- The device parses at most MAX_BOOKMARKS of each array and refuses to re-upload a
    -- set it had to truncate. Sending one it cannot hold whole would either be rejected
    -- there or, on older firmware, silently drop the excess for every peer.
    if #b > Push.MAX_RECORDS then
        return nil, string.format("%d bookmarks exceeds the device's cap of %d", #b, Push.MAX_RECORDS)
    end
    if #t > Push.MAX_RECORDS then
        return nil, string.format("%d tombstones exceeds the device's cap of %d", #t, Push.MAX_RECORDS)
    end
    return { b = b, t = t }
end

--[[--
Has anything actually changed against what the server holds?

A PUT that rewrites the blob with identical content still costs a round trip and still
races another peer, so a pull that changed nothing here should send nothing.
]]
function Push.differs(blob, remoteB, remoteT)
    if not blob then return false end
    if #blob.b ~= #(remoteB or {}) or #blob.t ~= #(remoteT or {}) then return true end
    local function indexBy(list)
        local out = {}
        for _, rec in ipairs(list or {}) do out[#out + 1] = rec end
        return out
    end
    local rb, rt = indexBy(remoteB), indexBy(remoteT)
    local function matched(rec, pool)
        for _, other in ipairs(pool) do
            if Lamport.spotMatch(rec, other) and (tonumber(other.v) or 0) == (tonumber(rec.v) or 0) then
                return true
            end
        end
        return false
    end
    for _, rec in ipairs(blob.b) do if not matched(rec, rb) then return true end end
    for _, rec in ipairs(blob.t) do if not matched(rec, rt) then return true end end
    return false
end

return Push
