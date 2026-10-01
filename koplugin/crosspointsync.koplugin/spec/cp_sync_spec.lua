local Sync = require("cp_sync")
local Push = require("cp_push")
local Lamport = require("cp_lamport")

-- A document in which every anchor resolves unless `missing` says otherwise.
local function fakeDoc(missing)
    return {
        isXPointerInDocument = function(_, xp) return not (missing or {})[xp] end,
        getPageFromXPointer = function() return 4 end,
        getTextFromXPointers = function() return "quoted" end,
    }
end

local XP = "/body/DocFragment[2]/body/p[7]"
local XP2 = "/body/DocFragment[2]/body/p[9]"

-- A device-made point bookmark: native paragraph anchor, real version.
local function deviceMark(xp, version, para)
    return { s = 1, p = 0.4, v = version, pi = para or 7, ct = "Ch", sn = "snip", cp = 0, pc = 0, xp = xp }
end

local function plan(over)
    local input = {
        remoteB = {}, remoteT = {}, marks = {}, stubs = {}, clock = 0,
        annotations = {}, sending = false, doc = fakeDoc(), datetime = "T",
        newRecord = function(item, version) return Push.newRecord(item, version, {}) end,
    }
    for k, v in pairs(over or {}) do input[k] = v end
    return Sync.plan(input)
end

local function keys(set)
    local out = {}
    for k in pairs(set) do out[#out + 1] = k end
    table.sort(out)
    return out
end

describe("cp_sync round trip", function()
    it("places the device's marks on a first pull", function()
        local p = plan({ remoteB = { deviceMark(XP, 3) } })
        assert.are.equal(1, #p.items)
        assert.are.equal(0, #keys(p.stale))
        assert.are.equal(3, p.byKey[XP .. "|"].v)
        -- The clock has cleared everything seen, so anything stamped next outranks it.
        assert.are.equal(3, p.clock)
        assert.are.equal(4, p.stamp)
    end)

    it("does nothing on a second identical pull", function()
        local rec = deviceMark(XP, 3)
        local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                         annotations = { { page = XP } } })
        assert.are.equal(0, #keys(p.stale))
        assert.are.equal(3, p.clock)
    end)

    it("removes a mark the device deleted", function()
        local rec = deviceMark(XP, 3)
        local p = plan({
            remoteB = {},
            remoteT = { { s = 1, pi = 7, p = 0.4, v = 4 } },
            marks = { [XP .. "|"] = rec },
            annotations = { { page = XP } },
        })
        assert.are.same({ XP .. "|" }, keys(p.stale))
        assert.are.equal(0, #p.items)
    end)

    it("resurrects one the device deleted and then made again", function()
        -- The annotation is present here, so nothing was deleted on this side: the only
        -- question is whether the device's newer re-add beats its own older tombstone.
        local p = plan({
            remoteB = { deviceMark(XP, 6) },
            remoteT = { { s = 1, pi = 7, p = 0.4, v = 5 } },
            marks = { [XP .. "|"] = deviceMark(XP, 3) },
            annotations = { { page = XP } },
        })
        assert.are.equal(0, #keys(p.stale))
        assert.are.equal(1, #p.items)
    end)

    describe("with uploading on", function()
        it("tombstones a mark deleted here, above everything it has seen", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                             annotations = {}, sending = true })
            assert.are.same({ XP .. "|" }, p.deleted)
            assert.are.equal(1, #p.blob.t)
            assert.are.equal(0, #p.blob.b)
            -- Strictly above, because the firmware gives a version tie to the bookmark.
            assert.is_true(p.blob.t[1].v > 3)
            assert.are.equal(7, p.blob.t[1].pi)
            -- And it does not come back as an insertion in the same pass.
            assert.are.equal(0, #p.items)
        end)

        it("keeps the record back, so a failed upload can retry the delete", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                             annotations = {}, sending = true })
            assert.are.equal(3, p.deletedRecords[XP .. "|"].v)
        end)

        it("sends a mark made here under a synthetic identity", function()
            local p = plan({
                annotations = { { pos0 = XP .. "/text().0", pos1 = XP .. "/text().9", drawer = "lighten",
                                  text = "hello" } },
                sending = true,
            })
            assert.are.equal(1, #p.created)
            assert.are.equal(1, #p.blob.b)
            local rec = p.blob.b[1]
            assert.is_true(Lamport.isForeign(rec))
            assert.are.equal(1, rec.s)
            assert.are.equal(XP .. "/text().0", rec.xp)
            assert.are.equal(1, p.sent)
        end)

        it("never touches an annotation it did not make", function()
            -- Not in the ledger, so it is "created" -- it is uploaded, never deleted.
            local p = plan({ annotations = { { page = XP } }, sending = true })
            assert.are.equal(0, #keys(p.stale))
            assert.are.equal(0, #p.deleted)
        end)

        it("passes a device mark it cannot place straight back", function()
            -- No anchor means it cannot be shown here -- but dropping it from the upload
            -- would delete it for every peer.
            local orphan = { s = 4, p = 0.8, v = 2, pi = 3, sn = "unanchored" }
            local p = plan({ remoteB = { orphan }, annotations = { { page = XP } }, sending = true })
            assert.are.equal(1, p.skipped)
            local found = false
            for _, rec in ipairs(p.blob.b) do
                if rec.sn == "unanchored" then found = true end
            end
            assert.is_true(found)
        end)

        it("carries fields it does not understand back unchanged", function()
            local rec = deviceMark(XP, 3)
            rec.futureField = "keep"
            local p = plan({ remoteB = { rec }, annotations = {}, sending = true,
                             marks = { ["/other|"] = deviceMark("/other", 1, 99) } })
            local carried
            for _, r in ipairs(p.blob.b) do if r.xp == XP then carried = r end end
            assert.are.equal("keep", carried.futureField)
        end)

        it("sends nothing when the server already holds this exact set", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                             annotations = { { page = XP } }, sending = true })
            assert.is_nil(p.blob)
            assert.are.equal(0, p.sent)
        end)

        it("refuses outright rather than send a set the device cannot hold", function()
            local remoteB = {}
            for i = 1, Push.MAX_RECORDS do
                remoteB[i] = { s = 1, pi = i, p = 0, v = 1, xp = "/x" .. i }
            end
            local p = plan({
                remoteB = remoteB,
                annotations = { { page = XP .. "/text().1" } },
                sending = true,
            })
            assert.is_nil(p.blob)
            assert.is_true(p.refused ~= nil)
        end)
    end)

    describe("with uploading off", function()
        -- A deletion made while the server cannot be told is owed, not undone. Putting
        -- the annotation back would also destroy the only record of the delete, so
        -- turning uploading on afterwards could never propagate it.
        it("holds a mark deleted here as owed instead of putting it back", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec }, annotations = {} })
            assert.are.equal(0, #p.items)
            assert.is_nil(p.blob)
            assert.are.same({ XP .. "|" }, keys(p.pending))
        end)

        it("keeps it owed across any number of syncs", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, pending = { [XP .. "|"] = rec }, annotations = {} })
            assert.are.equal(0, #p.items)
            assert.are.same({ XP .. "|" }, keys(p.pending))
        end)

        it("uploads nothing for a mark made here", function()
            local p = plan({ annotations = { { page = XP } }, sending = false })
            assert.is_nil(p.blob)
            assert.are.equal(0, p.sent)
        end)
    end)

    describe("a deletion owed from an earlier sync", function()
        it("goes out on the first sync that may upload", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, pending = { [XP .. "|"] = rec },
                             annotations = {}, sending = true })
            assert.are.equal(1, #p.blob.t)
            assert.are.equal(0, #p.blob.b)
            -- Strictly above, because the firmware gives a version tie to the bookmark.
            assert.is_true(p.blob.t[1].v > 3)
            assert.are.equal(1, p.sent)
            -- Still owed. An accepted upload is not proof the delete landed: the device
            -- may hold a higher version it has not published, and a tie goes to the
            -- bookmark. Only a pull that shows the spot gone settles it.
            assert.are.same({ XP .. "|" }, keys(p.pending))
            assert.are.equal(0, #p.settled)
        end)

        it("is settled by the pull that shows the server has stopped holding it", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = {}, remoteT = { { s = 1, pi = 7, p = 0.4, v = 4 } },
                             pending = { [XP .. "|"] = rec },
                             annotations = {}, sending = true })
            assert.are.same({ XP .. "|" }, p.settled)
            assert.are.equal(0, #keys(p.pending))
            assert.is_nil(p.blob)
        end)

        -- The case the firmware's adoption bump used to produce: the tombstone was
        -- accepted, lost the tie-break on the device, and the record came back live.
        it("is re-sent above the version that beat it, and never re-inserted here", function()
            local beat = deviceMark(XP, 40)
            local p = plan({ remoteB = { beat }, pending = { [XP .. "|"] = deviceMark(XP, 3) },
                             annotations = {}, sending = true })
            assert.are.same({ XP .. "|" }, keys(p.pending))
            assert.are.equal(1, #p.blob.t)
            assert.is_true(p.blob.t[1].v > 40)
            -- The whole point: the live record must not be put back as an annotation.
            assert.are.equal(0, #p.items)
        end)

        it("is taken back when the mark is made here again", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, pending = { [XP .. "|"] = rec },
                             annotations = { { page = XP } }, sending = true })
            assert.are.same({ XP .. "|" }, p.revived)
            assert.are.equal(0, #keys(p.pending))
            -- The server already holds it live; sending a second record for the same
            -- spot is what taking the deletion back exists to avoid.
            assert.is_nil(p.blob)
        end)

        -- Deleting a highlight and drawing it again over the same words is the common
        -- way to "fix" one, so both outcomes matter: identical anchors must cost nothing,
        -- and a selection that lands a character off must not have its new record
        -- swallowed by the tombstone for the old one.
        describe("a highlight deleted and then drawn again", function()
            local quote = { s = 1, p = 0.4, v = 12, pi = Lamport.NO_PARAGRAPH, q = true,
                            sw = 3, ew = 9, ct = "Ch", sn = "snip", cp = 0, pc = 0,
                            xp = XP, xp1 = XP2 }
            local owed = { [XP .. "|" .. XP2] = quote }

            it("costs nothing when the anchors come back identical", function()
                local p = plan({ remoteB = { quote }, pending = owed, sending = true,
                                 annotations = { { pos0 = XP, pos1 = XP2, page = XP, text = "q" } } })
                assert.are.same({ XP .. "|" .. XP2 }, p.revived)
                assert.are.equal(0, #keys(p.pending))
                assert.is_nil(p.blob)
            end)

            it("becomes a delete plus a create when the anchors differ", function()
                local wider = XP2 .. "1"
                local p = plan({ remoteB = { quote }, pending = owed, sending = true,
                                 annotations = { { pos0 = XP, pos1 = wider, page = XP, text = "q" } } })
                assert.are.equal(0, #p.revived)
                assert.are.equal(1, #p.blob.t)
                assert.are.equal(1, #p.blob.b)
                -- Distinct spots, or the tombstone would bury the replacement.
                assert.is_false(Lamport.spotMatch(p.blob.b[1], p.blob.t[1]))
            end)
        end)

        it("names every buried spot, so a failed upload can leave them owed", function()
            local a = deviceMark(XP, 3)
            local b = deviceMark(XP2, 4, 9)
            local p = plan({ remoteB = { a, b },
                             marks = { [XP .. "|"] = a },
                             pending = { [XP2 .. "|"] = b },
                             annotations = {}, sending = true })
            assert.are.same({ XP .. "|", XP2 .. "|" }, keys(p.deletedRecords))
            assert.are.equal(2, #p.blob.t)
        end)
    end)

    describe("when a device adopts a mark made here", function()
        -- The device resolves the anchor, files the mark under its own coordinates, and
        -- tombstones the synthetic spot. Both records name the same XPointer.
        local synthetic = Push.newRecord({ page = XP }, 5, {})
        local native = deviceMark(XP, 7, 41)

        it("re-keys the annotation instead of removing or duplicating it", function()
            local p = plan({
                remoteB = { native },
                remoteT = { { s = synthetic.s, pi = synthetic.pi, p = synthetic.p, v = 6 } },
                marks = { [XP .. "|"] = synthetic },
                annotations = { { page = XP } },
                sending = true,
            })
            -- The dead spot names the same annotation as the live one, so nothing goes.
            assert.are.equal(0, #keys(p.stale))
            assert.are.equal(41, p.byKey[XP .. "|"].pi)
            assert.is_false(Lamport.isForeign(p.byKey[XP .. "|"]))
        end)

        it("stops treating it as a local creation afterwards", function()
            local p = plan({
                remoteB = { native },
                marks = { [XP .. "|"] = native },
                annotations = { { page = XP } },
                sending = true,
            })
            assert.are.equal(0, #p.created)
        end)

        -- Deleting it is the case the re-keying makes dangerous: the ledger still names
        -- the spot the mark had before the device adopted it, so a tombstone raised from
        -- the ledger alone buries a spot that is already dead and leaves the device's own
        -- record live -- which the next pull then puts back here.
        describe("and it is deleted here afterwards", function()
            local KEY = XP .. "|" .. XP2
            local mine = Push.newRecord({ pos0 = XP, pos1 = XP2, text = "q" }, 1, {})
            local theirs = { s = 1, p = 0.4, v = 7, q = true, sw = 3, ew = 9,
                             pi = Lamport.NO_PARAGRAPH, ct = "Ch", sn = "q", cp = 0, pc = 0,
                             xp = XP, xp1 = XP2 }

            local function buries(p)
                for _, t in ipairs(p.blob and p.blob.t or {}) do
                    if Lamport.spotMatch(t, theirs) then return true end
                end
                return false
            end

            it("buries the device's own record, not just the synthetic spot", function()
                local p = plan({
                    remoteB = { theirs },
                    remoteT = { { s = mine.s, q = true, sw = mine.sw, ew = mine.ew,
                                  pi = Lamport.NO_PARAGRAPH, p = mine.p, v = 6 } },
                    marks = { [KEY] = mine }, clock = 7,
                    annotations = {},
                    sending = true,
                })
                assert.is_true(buries(p))
                -- Nothing live survives the spot, so the pull cannot re-insert it here.
                assert.are.equal(0, #p.blob.b)
                assert.are.equal(0, #p.items)
            end)

            it("does the same for a deletion owed from a sync that could not upload", function()
                local p = plan({
                    remoteB = { theirs },
                    pending = { [KEY] = mine }, clock = 7,
                    annotations = {},
                    sending = true,
                })
                assert.is_true(buries(p))
                assert.are.equal(0, #p.items)
            end)

            it("leaves the device's record alone while the annotation is still here", function()
                local p = plan({
                    remoteB = { theirs },
                    marks = { [KEY] = mine }, clock = 7,
                    annotations = { { pos0 = XP, pos1 = XP2, page = XP, text = "q" } },
                    sending = true,
                })
                assert.is_false(buries(p))
            end)
        end)
    end)

    describe("upgrading from a pull-only release", function()
        it("does not mistake an already-pulled annotation for a new one", function()
            local p = plan({
                remoteB = { deviceMark(XP, 3) },
                stubs = { [XP .. "|"] = true },
                annotations = { { page = XP } },
                sending = true,
            })
            assert.are.equal(0, #p.created)
            assert.is_nil(p.blob)  -- nothing changed, so nothing is sent
        end)

        it("reports a stub whose annotation is gone as untombstonable", function()
            local p = plan({ stubs = { [XP .. "|"] = true }, annotations = {}, sending = true })
            assert.are.same({ XP .. "|" }, p.orphaned)
            assert.are.equal(0, #p.deleted)
        end)

        it("replaces the stub with a real record on that first pull", function()
            local p = plan({
                remoteB = { deviceMark(XP, 3) },
                stubs = { [XP .. "|"] = true },
                annotations = { { page = XP } },
            })
            assert.are.equal(3, p.byKey[XP .. "|"].v)
        end)
    end)

    it("skips an anchor that does not resolve in this copy of the book", function()
        local p = plan({ remoteB = { deviceMark(XP, 1), deviceMark(XP2, 1, 9) },
                         doc = fakeDoc({ [XP2] = true }) })
        assert.are.equal(1, #p.items)
        assert.are.equal(1, p.skipped)
    end)

    describe("the clock it records afterwards", function()
        it("does not move when the pull sent nothing", function()
            -- Recorded on every pull, so advancing it here would climb by one forever
            -- and leave this side's versions permanently ahead of the device's.
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                             annotations = { { page = XP } }, sending = true })
            assert.is_nil(p.blob)
            assert.are.equal(p.clock, p.commitClock)
        end)

        it("does not move when uploading is off", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec }, annotations = {} })
            assert.are.equal(p.clock, p.commitClock)
        end)

        it("moves to the stamp when something was actually sent", function()
            local rec = deviceMark(XP, 3)
            local p = plan({ remoteB = { rec }, marks = { [XP .. "|"] = rec },
                             annotations = {}, sending = true })
            assert.is_true(p.blob ~= nil)
            assert.are.equal(p.stamp, p.commitClock)
            assert.is_true(p.commitClock > p.clock)
        end)
    end)

    it("tolerates a pull with nothing on either side", function()
        local p = plan({})
        assert.are.equal(0, #p.items)
        assert.are.equal(0, #keys(p.stale))
        assert.is_nil(p.blob)
    end)
end)
