--[[--
Deciding what to do with a reading position fetched from the sync server.

CrossPoint stores progress as an XPointer plus a percentage, in exactly the shape stock
kosync uses, under the same document key as the annotation blob. So once a book's key
matches, the position is already there to be read -- which is why this plugin reads it
rather than leaving it to stock kosync, whose own key is computed differently and may not
name the same book.

The rules mirror kosync's: a record from the position we are already at is not a jump, and
a record that matches nothing here is refused rather than approximated. Kept pure and free
of KOReader modules so the decision can be tested without a document open.
]]

local Progress = {
    -- Why no jump happened. Codes rather than sentences, so the wording -- and its
    -- translation -- stays with the UI and this module stays free of KOReader.
    NO_REPLY = "no_reply",
    NO_RECORD = "no_record",
    NOT_USABLE = "not_usable",
    SAME_PLACE = "same_place",
}

--[[--
Round a percentage the way KOReader's Math.roundPercent does.

Progress percentages travel as floats through JSON and Redis, so two devices sitting at
the same place can disagree in the last bits. Four decimals is ~0.01% of a book, well
below one page, and is the precision kosync itself compares at.
]]
function Progress.roundPercent(value)
    local n = tonumber(value)
    if not n then return nil end
    return math.floor(n * 10000 + 0.5) / 10000
end

--[[--
What to do about a fetched position.

@param remote  the server record: `progress`, `percentage`, `device`, `timestamp`
@param here    the local state: `progress`, `percentage`, `has_pages`
@return a verdict table:
        `{ action = "none", reason = <code> }` -- nothing to do; `reason` is one of the
        Progress.NO_* codes, which the caller turns into a translated line
        `{ action = "jump", target = <xpointer|page number>, percentage = , device = ,
           forward = <bool> }`
]]
function Progress.decide(remote, here)
    if type(remote) ~= "table" then
        return { action = "none", reason = Progress.NO_REPLY }
    end
    local target = remote.progress
    if target == nil or target == "" then
        return { action = "none", reason = Progress.NO_RECORD }
    end
    here = here or {}

    -- A paged document (PDF, CBZ) records a page number, not an XPointer. A CrossPoint
    -- device only ever reads EPUBs, so a position from one cannot be placed in a paged
    -- copy of the same title.
    if here.has_pages then
        local page = tonumber(target)
        if not page then
            return { action = "none", reason = Progress.NOT_USABLE }
        end
        target = page
    elseif type(target) ~= "string" then
        return { action = "none", reason = Progress.NOT_USABLE }
    end

    if here.progress ~= nil and here.progress == remote.progress then
        return { action = "none", reason = Progress.SAME_PLACE }
    end

    local remotePct = Progress.roundPercent(remote.percentage)
    local herePct = Progress.roundPercent(here.percentage)
    if remotePct ~= nil and herePct ~= nil and remotePct == herePct then
        return { action = "none", reason = Progress.SAME_PLACE }
    end

    return {
        action = "jump",
        target = target,
        percentage = remotePct,
        device = remote.device,
        -- Only used to word the prompt. A backwards jump is a normal thing to want -- the
        -- device may simply have been read on last -- so it is offered, not suppressed.
        forward = (remotePct or 0) > (herePct or 0),
    }
end

--[[--
Whether this device's position is worth sending.

The rule is simply "has it changed since the last thing this book agreed on", and `sent`
carries more than the pushes: a pull seeds it with whatever the server already held, so a
position taken from the device is not immediately echoed back, and one that was OFFERED
and not taken is not overwritten while the reader has not moved. That last case is the
one that matters -- an automatic pull raises a prompt, and a book closed with the prompt
still standing must not report a position the reader never chose.

Re-sending an unchanged position is not harmless here: the server fork deletes the rich
`position` object on any write lacking one, so an idle echo would strip the anchor a
CrossPoint device left for itself.

@param here the local state: `progress`, `percentage`
@param sent the last position this book has already agreed on, or nil
@return true, or false and a Progress.* code saying why not
]]
function Progress.shouldPush(here, sent)
    here = here or {}
    if here.progress == nil or here.progress == "" then
        return false, Progress.NOT_USABLE
    end
    -- The server rejects a write with no percentage, so there is nothing to send without
    -- one -- and a record missing it would read back as a position with no ordering.
    if tonumber(here.percentage) == nil then
        return false, Progress.NOT_USABLE
    end
    if sent ~= nil and here.progress == sent then
        return false, Progress.SAME_PLACE
    end
    return true
end

return Progress
