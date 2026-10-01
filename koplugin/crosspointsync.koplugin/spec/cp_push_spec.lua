local Push = require("cp_push")
local Lamport = require("cp_lamport")

local QUOTE = {
    pos0 = "/body/DocFragment[3]/body/p[2]/text()[1].4",
    pos1 = "/body/DocFragment[3]/body/p[2]/text()[1].42",
    drawer = "lighten",
    text = "the quoted sentence",
}
local BOOKMARK = { page = "/body/DocFragment[1]/body/p[9]" }

describe("cp_push", function()
    describe("spineIndexOf", function()
        it("takes DocFragment[N] to mean spine item N-1", function()
            assert.are.equal(2, Push.spineIndexOf("/body/DocFragment[3]/body/p[1]"))
        end)

        it("takes the indexless single-spine form as item 0", function()
            assert.are.equal(0, Push.spineIndexOf("/body/DocFragment/body/p[1]"))
        end)

        it("refuses anything that names no fragment", function()
            assert.is_nil(Push.spineIndexOf("/body/p[1]"))
            assert.is_nil(Push.spineIndexOf("/body/DocFragment[]/body"))
            assert.is_nil(Push.spineIndexOf("/body/DocFragment[x]/body"))
            assert.is_nil(Push.spineIndexOf("/body/DocFragment[0]/body"))
            assert.is_nil(Push.spineIndexOf(nil))
        end)
    end)

    describe("trim", function()
        it("leaves a short string alone", function()
            assert.are.equal("abc", Push.trim("abc", 10))
        end)

        it("never splits a UTF-8 character", function()
            -- "a" + U+00E9 (two bytes) + "b"; a two-byte cut would land mid-character.
            assert.are.equal("a", Push.trim("a\xC3\xA9b", 2))
            assert.are.equal("a\xC3\xA9", Push.trim("a\xC3\xA9b", 3))
        end)

        it("treats a non-string as empty", function()
            assert.are.equal("", Push.trim(nil, 5))
        end)
    end)

    describe("newRecord", function()
        it("gives a highlight a synthetic word range in the reserved band", function()
            local rec = Push.newRecord(QUOTE, 11, {})
            assert.is_true(rec.q)
            assert.are.equal(2, rec.s)
            assert.are.equal(11, rec.v)
            assert.are.equal(QUOTE.pos0, rec.xp)
            assert.are.equal(QUOTE.pos1, rec.xp1)
            assert.are.equal("the quoted sentence", rec.sn)
            assert.is_true(Lamport.isForeign(rec))
            -- Keyed by the range, so it carries no paragraph anchor.
            assert.are.equal(Lamport.NO_PARAGRAPH, rec.pi)
        end)

        it("gives a point bookmark a synthetic paragraph anchor and no range", function()
            local rec = Push.newRecord(BOOKMARK, 4, {})
            assert.is_nil(rec.q)
            assert.is_nil(rec.sw)
            assert.are.equal(0, rec.s)
            assert.is_true(Lamport.isForeign(rec))
        end)

        it("takes the chapter and the intra-spine progress from the caller", function()
            local rec = Push.newRecord(QUOTE, 1, {
                chapterOf = function() return "Chapter Four" end,
                progressOf = function() return 0.375 end,
            })
            assert.are.equal("Chapter Four", rec.ct)
            assert.are.equal(0.375, rec.p)
            assert.are.equal(0.375, rec.ep)
        end)

        it("falls back to zero progress rather than an impossible one", function()
            assert.are.equal(0, Push.newRecord(QUOTE, 1, { progressOf = function() return 4.2 end }).p)
            assert.are.equal(0, Push.newRecord(QUOTE, 1, { progressOf = function() return nil end }).p)
        end)

        it("caps the chapter title and snippet at what the device stores", function()
            local rec = Push.newRecord({ page = "/body/DocFragment[1]/body/p[1]", text = string.rep("x", 200) }, 1, {
                chapterOf = function() return string.rep("y", 200) end,
            })
            assert.are.equal(Push.SNIPPET_MAX, #rec.sn)
            assert.are.equal(Push.CHAPTER_MAX, #rec.ct)
        end)

        it("refuses an annotation with no anchor this side can use", function()
            assert.is_nil(Push.newRecord({ page = 42 }, 1, {}))
            assert.is_nil(Push.newRecord({ page = "/body/p[1]" }, 1, {}))
        end)

        it("two different marks do not collide", function()
            local a = Push.newRecord(QUOTE, 1, {})
            local b = Push.newRecord({ pos0 = QUOTE.pos0, pos1 = "/other", drawer = "lighten" }, 1, {})
            assert.is_false(Lamport.spotMatch(a, b))
        end)
    end)

    describe("tombstoneOf", function()
        it("emits only the fields the device reads from a tombstone", function()
            local t = Push.tombstoneOf({ s = 2, pi = 9, p = 0.4, sn = "text", xp = "/a", ct = "Ch" }, 7)
            assert.are.equal(2, t.s)
            assert.are.equal(9, t.pi)
            assert.are.equal(7, t.v)
            assert.is_nil(t.sn)
            assert.is_nil(t.xp)
            assert.is_nil(t.ct)
            assert.is_nil(t.q)
        end)

        it("carries a highlight's word range", function()
            local t = Push.tombstoneOf({ s = 1, q = true, sw = 4, ew = 9, p = 0.2 }, 3)
            assert.is_true(t.q)
            assert.are.equal(4, t.sw)
            assert.are.equal(9, t.ew)
        end)
    end)

    describe("blob", function()
        it("keeps fields it does not understand, so a rewrite strips nothing", function()
            local rec = { s = 1, pi = 4, p = 0.1, v = 2, sn = "t", somethingNew = "keep me" }
            local blob = Push.blob({ { rec = rec, v = 2, live = true } })
            assert.are.equal("keep me", blob.b[1].somethingNew)
        end)

        it("stamps the winner's version onto the record it sends", function()
            local blob = Push.blob({ { rec = { s = 1, pi = 4, v = 2 }, v = 9, live = true } })
            assert.are.equal(9, blob.b[1].v)
        end)

        it("splits winners into bookmarks and tombstones", function()
            local blob = Push.blob({
                { rec = { s = 1, pi = 4 }, v = 1, live = true },
                { rec = { s = 1, pi = 5 }, v = 2, live = false },
            })
            assert.are.equal(1, #blob.b)
            assert.are.equal(1, #blob.t)
        end)

        it("refuses a set the device could not hold whole", function()
            local winners = {}
            for i = 1, Push.MAX_RECORDS + 1 do
                winners[i] = { rec = { s = 1, pi = i }, v = 1, live = true }
            end
            local blob, reason = Push.blob(winners)
            assert.is_nil(blob)
            assert.is_true(reason:find("cap", 1, true) ~= nil)
        end)
    end)

    describe("differs", function()
        it("says no when the server already holds this exact set", function()
            local remoteB = { { s = 1, pi = 4, p = 0.1, v = 3 } }
            local blob = Push.blob({ { rec = remoteB[1], v = 3, live = true } })
            assert.is_false(Push.differs(blob, remoteB, {}))
        end)

        it("says yes when a version moved", function()
            local remoteB = { { s = 1, pi = 4, p = 0.1, v = 3 } }
            local blob = Push.blob({ { rec = remoteB[1], v = 4, live = true } })
            assert.is_true(Push.differs(blob, remoteB, {}))
        end)

        it("says yes when a mark was added here", function()
            assert.is_true(Push.differs(Push.blob({ { rec = { s = 1, pi = 4 }, v = 1, live = true } }), {}, {}))
        end)

        it("says yes when a bookmark became a tombstone", function()
            local remoteB = { { s = 1, pi = 4, p = 0.1, v = 3 } }
            local blob = Push.blob({ { rec = remoteB[1], v = 4, live = false } })
            assert.is_true(Push.differs(blob, remoteB, {}))
        end)
    end)
end)
