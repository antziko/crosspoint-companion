--[[--
Document identity, matching CrossPoint's two sync modes.

A CrossPoint device keys a book's sync record either by KOReader's own partial-MD5 of the
file's contents, or by an MD5 of its normalised filename. The second mode exists because
the same book optimised for two Xteink models is two different files; it only agrees
across devices if both sides normalise the name the same way, and the failure is silent —
a mismatched key just looks like a book with no annotations. So the two normalisation
rules below are transcribed from lib/KOReaderSync/KOReaderDocumentId.h and covered by the
same cases its host tests use.
]]

local md5 = require("ffi/sha2").md5

local DocId = {}

--- Filename with no directory part. Splits on "/" only, as the firmware does, so a
--- backslash inside a name is part of the name rather than a separator.
function DocId.basename(path)
    if type(path) ~= "string" then return "" end
    return path:match("([^/]+)$") or path
end

--[[--
Drop the auto-optimizer device tag so an optimised copy keys the same as the original.

Only "(X3)" and "(X4)" are recognised, in either placement, and both require the
separating space — so a real title ending in "(X11)" is not falsely stripped.
]]
function DocId.stripDeviceTag(basename)
    local s = basename

    -- Leading "(X3) " / "(X4) ", exactly five characters.
    local prefix = s:sub(1, 5)
    if prefix == "(X3) " or prefix == "(X4) " then
        s = s:sub(6)
    end

    -- Trailing " (X3)" / " (X4)", immediately before the final extension.
    local dot = s:match("^.*()%.")
    local stemEnd = dot and (dot - 1) or #s
    if stemEnd >= 5 then
        local tag = s:sub(stemEnd - 4, stemEnd)
        if tag == " (X3)" or tag == " (X4)" then
            s = s:sub(1, stemEnd - 5) .. s:sub(stemEnd + 1)
        end
    end

    return s
end

--[[--
Canonicalise "{a} - {b}" to one ordering, so an author/title swap shares a key.

Fires only when the stem holds exactly one " - ": a name with a subtitle would otherwise
be mis-split into a wrong, but confidently stable, key. Apply after stripDeviceTag.
]]
function DocId.swapAuthorTitle(basename)
    local dot = basename:match("^.*()%.")
    local stem = dot and basename:sub(1, dot - 1) or basename
    local ext = dot and basename:sub(dot) or ""

    local first = stem:find(" - ", 1, true)
    if not first then return basename end
    if stem:find(" - ", first + 3, true) then return basename end

    local a = stem:sub(1, first - 1)
    local b = stem:sub(first + 3)
    if a == "" or b == "" then return basename end
    if b < a then a, b = b, a end
    return a .. " - " .. b .. ext
end

--[[--
The canonical name a filename-mode key is computed from.

stripDeviceTag then swapAuthorTitle, twice. The repeat is not redundant: a tag can sit on
the title rather than on the whole name ("Author - (X4) Title.epub"), where it is at
neither end of the filename and survives the first strip. Canonicalising the order moves
it to the front, the second strip removes it, and the second swap restores the ordering
that removal changed:

    "Author - (X4) Title.epub"
      -> strip: unchanged              -> swap: "(X4) Title - Author.epub"
      -> strip: "Title - Author.epub"  -> swap: canonical order

A tag trailing the FIRST component ("Author (X4) - Title.epub") is still not recognised:
it is at neither end and no reordering brings it to one.

Mirrors KOReaderDocumentId::canonicalFilename. The two must agree exactly.
]]
function DocId.canonicalFilename(basename)
    return DocId.swapAuthorTitle(DocId.stripDeviceTag(
        DocId.swapAuthorTitle(DocId.stripDeviceTag(basename))))
end

--- The filename-mode document key: MD5 of the canonical basename.
function DocId.fromFilename(path)
    local name = DocId.canonicalFilename(DocId.basename(path))
    if name == "" then return nil end
    return md5(name)
end

--[[--
The key for the currently open document.

Binary mode reuses the partial-MD5 KOReader has already computed and cached in the book's
sidecar, rather than re-reading the file.
]]
function DocId.forDocument(ui, filename_mode)
    if not ui or not ui.document then return nil end
    if filename_mode then
        return DocId.fromFilename(ui.document.file)
    end
    local cached = ui.doc_settings and ui.doc_settings:readSetting("partial_md5_checksum")
    if cached then return cached end
    if ui.document.fast_digest then return ui.document:fast_digest() end
    return nil
end

return DocId
