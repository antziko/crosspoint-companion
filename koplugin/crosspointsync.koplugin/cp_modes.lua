--[[--
What each half of the sync is set to do, as one value instead of two checkboxes.

Bookmarks and highlights run in one of three modes -- off, receive only, two-way -- and
the reading position in one of four, the extra one being send only. The asymmetry is not
an oversight: an annotation upload is built from the merge against the GET that
immediately precedes it, so for annotations receiving is how sending works and send-only
is not a state this plugin can be in. A position carries no merge, so sending yours while
ignoring the device's is a real and useful setting -- it is what "this device decides
where I am" means.

The stored settings keep the names and meanings they have always had, so an existing
configuration reads back unchanged and nothing needs migrating. This module is the one
place that knows how the pair maps onto a mode, and it is free of KOReader so the mapping
can be tested.
]]

local Modes = {
    OFF = "off",
    RECEIVE = "receive",
    SEND = "send",
    TWO_WAY = "two_way",
}

--[[--
The mode bookmarks and highlights are in.

`sync_annotations` is the newer of the two keys, so a settings table written before it
existed has no value for it; absent means on, which is what that configuration did.
]]
function Modes.annotations(settings)
    settings = settings or {}
    if settings.sync_annotations == false then return Modes.OFF end
    return settings.push_changes and Modes.TWO_WAY or Modes.RECEIVE
end

function Modes.setAnnotations(settings, mode)
    settings.sync_annotations = mode ~= Modes.OFF
    settings.push_changes = mode == Modes.TWO_WAY
    return settings
end

--- The mode the reading position is in. Each of the four is one of the four combinations
--- the two stored flags can take, so the mapping is lossless in both directions.
function Modes.progress(settings)
    settings = settings or {}
    local receive = settings.sync_progress and true or false
    local send = settings.push_progress and true or false
    if receive and send then return Modes.TWO_WAY end
    if send then return Modes.SEND end
    if receive then return Modes.RECEIVE end
    return Modes.OFF
end

function Modes.setProgress(settings, mode)
    settings.sync_progress = mode == Modes.RECEIVE or mode == Modes.TWO_WAY
    settings.push_progress = mode == Modes.SEND or mode == Modes.TWO_WAY
    return settings
end

return Modes
