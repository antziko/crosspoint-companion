--[[--
Lamport reconciliation, in the same terms the CrossPoint firmware uses.

A CrossPoint device has no clock -- an ESP32-C3 has no battery-backed RTC -- so bookmark
sync orders create-against-delete by a per-book logical counter instead. Every record
carries `v`; for each spot the highest `v` across {local bookmark, local tombstone,
remote bookmark, remote tombstone} wins, and on a tie the live bookmark wins so a user
bookmark is never silently dropped. That is `BookmarkStore::mergeFrom`, and this is the
same rule on this side of the wire. Both halves must agree exactly, or a delete made on
one device is undone by the other and the two never settle.

Everything here is pure: no KOReader modules, no document, no I/O.
]]

local Lamport = {}

-- Mirrors BookmarkStore.cpp: PROGRESS_QUANTUM, and UINT16_MAX for "no paragraph anchor".
Lamport.PROGRESS_QUANTUM = 1000
Lamport.NO_PARAGRAPH = 65535

--[[--
Reserved identity range for a mark KOReader made.

The device keys a quote by (spine, startWord, endWord) and a point bookmark by
(spine, paragraphIndex). Word indices are page-local -- they describe only the pagination
that produced them -- and the paragraph index comes from the device's own section cache.
KOReader can reproduce neither, and does not have to: an identity has to be stable and
unique, not derivable. So a KOReader-born mark takes a synthetic key hashed from its
XPointer, in a range the device's own numbering cannot reach, and the device stores and
echoes it verbatim.

A device word index runs to a few hundred and a paragraph index to a few thousand, so
0xC000 is far above anything real. 0xFFFF stays excluded: that is "no anchor".
]]
Lamport.FOREIGN_BASE = 0xC000
Lamport.FOREIGN_SPAN = 0x3FFF  -- keeps the top of the range at 0xFFFE

local function num(v, fallback)
    return tonumber(v) or fallback
end

--- Intra-spine progress at the resolution the firmware compares it at.
function Lamport.quantize(p)
    local n = num(p, 0)
    -- std::lround rounds half away from zero; progress is a fraction of a chapter and so
    -- never negative, but mirror it rather than rely on that.
    if n < 0 then return -math.floor(-n * Lamport.PROGRESS_QUANTUM + 0.5) end
    return math.floor(n * Lamport.PROGRESS_QUANTUM + 0.5)
end

--- A record is a highlight rather than a point bookmark. Absent means point, as on the wire.
function Lamport.isQuote(rec)
    return rec ~= nil and rec.q == true
end

--[[--
Do two records name the same spot?

A port of `keyMatchFull` (BookmarkStore.cpp:74), kept as a predicate rather than reduced
to a hash key on purpose: `pointKeyMatch` compares paragraphs when both sides have an
anchor and quantized progress otherwise, which is not transitive, so no string key can
stand in for it without drifting from the firmware. The sets involved are capped at 128
records, and `mergeFrom` itself scans linearly, so matching this exactly costs nothing.
]]
function Lamport.spotMatch(a, b)
    if a == nil or b == nil then return false end
    local aq, bq = Lamport.isQuote(a), Lamport.isQuote(b)
    if aq ~= bq then return false end

    local as, bs = num(a.s, 0), num(b.s, 0)
    if as ~= bs then return false end

    if aq then
        return num(a.sw, 0) == num(b.sw, 0) and num(a.ew, 0) == num(b.ew, 0)
    end

    local ap, bp = num(a.pi, Lamport.NO_PARAGRAPH), num(b.pi, Lamport.NO_PARAGRAPH)
    if ap ~= Lamport.NO_PARAGRAPH and bp ~= Lamport.NO_PARAGRAPH then
        return ap == bp
    end
    return Lamport.quantize(a.p) == Lamport.quantize(b.p)
end

--- Is this a mark KOReader made that no device has adopted yet?
function Lamport.isForeign(rec)
    if rec == nil then return false end
    if Lamport.isQuote(rec) then
        return num(rec.sw, 0) >= Lamport.FOREIGN_BASE
    end
    local pi = num(rec.pi, Lamport.NO_PARAGRAPH)
    return pi >= Lamport.FOREIGN_BASE and pi < Lamport.NO_PARAGRAPH
end

--[[--
A stable 28-bit hash of a string.

djb2 with the xor step replaced by addition, and reduced mod 2^28 so every intermediate
stays exact in a double: 2^28 * 33 is about 8.9e9, far inside 2^53. Written this way
because the obvious FNV-1a needs xor, and neither LuaJIT's `bit` nor 5.3's operators are
available across every interpreter these specs run under.
]]
function Lamport.hash(s)
    local h = 5381
    for i = 1, #s do
        h = (h * 33 + s:byte(i)) % 268435456
    end
    return h
end

--- The synthetic (startWord, endWord) a KOReader highlight is keyed by.
function Lamport.syntheticQuoteKey(pos0, pos1)
    local h = Lamport.hash(tostring(pos0) .. "|" .. tostring(pos1 or ""))
    local lo = h % Lamport.FOREIGN_SPAN
    local hi = math.floor(h / Lamport.FOREIGN_SPAN) % Lamport.FOREIGN_SPAN
    return Lamport.FOREIGN_BASE + lo, Lamport.FOREIGN_BASE + hi
end

--- The synthetic paragraph index a KOReader point bookmark is keyed by.
function Lamport.syntheticParagraph(pos0)
    return Lamport.FOREIGN_BASE + (Lamport.hash(tostring(pos0)) % Lamport.FOREIGN_SPAN)
end

--[[--
Resolve every spot to a single winner.

@param entries list of `{ rec = <record>, v = <number>, live = <bool> }`, in any order.
               `live` false means the entry is a tombstone for that spot.
@return list of winners, `{ rec = , v = , live = }`, one per distinct spot, in first-seen
        order.

`v` and `live` are the outcome of the contest. `rec` is not: it is the best-described
record seen for the spot, win or lose. A tombstone arrives carrying only the identity
fields, so a spot a tombstone wins would otherwise have no `xp` -- and the caller needs
one to know which KOReader annotation just died. Any record naming an anchor is therefore
kept in preference to one that does not, whichever of them won.
]]
function Lamport.merge(entries)
    local winners = {}
    for _, e in ipairs(entries or {}) do
        local spot
        for _, w in ipairs(winners) do
            if Lamport.spotMatch(w.rec, e.rec) then spot = w break end
        end
        if not spot then
            winners[#winners + 1] = { rec = e.rec, v = e.v or 0, live = e.live and true or false }
        else
            local v = e.v or 0
            local better = v > spot.v or (v == spot.v and e.live and not spot.live)
            if better then
                spot.v = v
                spot.live = e.live and true or false
                -- Only a live record may replace the stored one here: a tombstone would
                -- drop the snippet and anchor a later re-add at this spot still needs.
                if e.live then spot.rec = e.rec end
            end
            -- Held separately from the contest, for the reason in the doc comment above.
            if type(e.rec.xp) == "string" and type(spot.rec.xp) ~= "string" then
                spot.rec = e.rec
            end
        end
    end
    return winners
end

--- The clock, advanced past everything seen. Mirrors `observeVersion`.
function Lamport.observe(clock, v)
    local n = num(v, 0)
    local c = num(clock, 0)
    return n > c and n or c
end

return Lamport
