local Lamport = require("cp_lamport")

local function point(spine, para, progress, version)
    return { s = spine, pi = para, p = progress, v = version }
end
local function quote(spine, sw, ew, version)
    return { s = spine, q = true, sw = sw, ew = ew, v = version }
end

describe("cp_lamport", function()
    describe("quantize", function()
        it("rounds to the thousandth of a chapter the firmware compares at", function()
            assert.are.equal(423, Lamport.quantize(0.4234))
            assert.are.equal(424, Lamport.quantize(0.4235))
            assert.are.equal(0, Lamport.quantize(nil))
        end)
    end)

    describe("spotMatch", function()
        it("matches point bookmarks on their paragraph anchor", function()
            assert.is_true(Lamport.spotMatch(point(1, 7, 0.10), point(1, 7, 0.90)))
            assert.is_false(Lamport.spotMatch(point(1, 7, 0.10), point(1, 8, 0.10)))
        end)

        it("does not match across spine items", function()
            assert.is_false(Lamport.spotMatch(point(1, 7, 0.1), point(2, 7, 0.1)))
        end)

        it("falls back to quantized progress when either side has no anchor", function()
            local anchored = point(1, 7, 0.5)
            local bare = { s = 1, p = 0.5004 }
            assert.is_true(Lamport.spotMatch(bare, { s = 1, p = 0.5 }))
            assert.is_true(Lamport.spotMatch(anchored, bare))
            assert.is_false(Lamport.spotMatch(anchored, { s = 1, p = 0.9 }))
        end)

        it("keys a highlight by its word range, never its progress", function()
            assert.is_true(Lamport.spotMatch(quote(2, 10, 14), { s = 2, q = true, sw = 10, ew = 14, p = 0.9 }))
            assert.is_false(Lamport.spotMatch(quote(2, 10, 14), quote(2, 10, 15)))
        end)

        it("never matches a highlight against a point bookmark", function()
            assert.is_false(Lamport.spotMatch(quote(1, 0, 0), point(1, 65535, 0)))
        end)
    end)

    describe("merge", function()
        it("keeps the highest version for a spot", function()
            local winners = Lamport.merge({
                { rec = point(1, 7, 0.1, 2), v = 2, live = true },
                { rec = point(1, 7, 0.1, 9), v = 9, live = true },
            })
            assert.are.equal(1, #winners)
            assert.are.equal(9, winners[1].v)
        end)

        it("buries a bookmark under a later tombstone", function()
            local winners = Lamport.merge({
                { rec = point(1, 7, 0.1, 4), v = 4, live = true },
                { rec = point(1, 7, 0.1), v = 5, live = false },
            })
            assert.is_false(winners[1].live)
        end)

        it("resurrects it when the re-add is later still", function()
            local winners = Lamport.merge({
                { rec = point(1, 7, 0.1, 4), v = 4, live = true },
                { rec = point(1, 7, 0.1), v = 5, live = false },
                { rec = point(1, 7, 0.1, 6), v = 6, live = true },
            })
            assert.is_true(winners[1].live)
            assert.are.equal(6, winners[1].v)
        end)

        it("gives a tie to the live bookmark, as BookmarkStore does", function()
            local winners = Lamport.merge({
                { rec = point(1, 7, 0.1), v = 5, live = false },
                { rec = point(1, 7, 0.1, 5), v = 5, live = true },
            })
            assert.is_true(winners[1].live)
        end)

        it("keeps the anchor when a tombstone arrives after the bookmark", function()
            -- A tombstone carries no anchor, and the caller still needs one to know which
            -- KOReader annotation the dead spot refers to.
            local winners = Lamport.merge({
                { rec = { s = 1, pi = 7, p = 0.1, v = 4, xp = "/anchor", sn = "text" }, v = 4, live = true },
                { rec = { s = 1, pi = 7, p = 0.1 }, v = 5, live = false },
            })
            assert.is_false(winners[1].live)
            assert.are.equal("/anchor", winners[1].rec.xp)
        end)

        it("keeps the anchor when the tombstone is seen first", function()
            -- The order the server happens to return records in must not decide whether
            -- a delete can be applied here.
            local winners = Lamport.merge({
                { rec = { s = 1, pi = 7, p = 0.1 }, v = 5, live = false },
                { rec = { s = 1, pi = 7, p = 0.1, v = 4, xp = "/anchor" }, v = 4, live = true },
            })
            assert.is_false(winners[1].live)
            assert.are.equal(5, winners[1].v)
            assert.are.equal("/anchor", winners[1].rec.xp)
        end)

        it("does not let a losing record downgrade an anchor already held", function()
            local winners = Lamport.merge({
                { rec = { s = 1, pi = 7, p = 0.1, v = 9, xp = "/current" }, v = 9, live = true },
                { rec = { s = 1, pi = 7, p = 0.1, v = 2, xp = "/stale" }, v = 2, live = true },
            })
            assert.are.equal("/current", winners[1].rec.xp)
        end)

        it("keeps distinct spots apart", function()
            local winners = Lamport.merge({
                { rec = point(1, 7, 0.1, 1), v = 1, live = true },
                { rec = point(1, 8, 0.2, 1), v = 1, live = true },
                { rec = quote(1, 3, 5, 1), v = 1, live = true },
            })
            assert.are.equal(3, #winners)
        end)

        it("tolerates an empty entry list", function()
            assert.are.equal(0, #Lamport.merge(nil))
        end)
    end)

    describe("synthetic identity", function()
        it("lands inside the reserved range and never on \"no anchor\"", function()
            for i = 1, 200 do
                local sw, ew = Lamport.syntheticQuoteKey("/body/DocFragment[3]/body/p[" .. i .. "]", "/end" .. i)
                assert(sw >= Lamport.FOREIGN_BASE and sw < 65535, "sw out of range: " .. sw)
                assert(ew >= Lamport.FOREIGN_BASE and ew < 65535, "ew out of range: " .. ew)
                local pi = Lamport.syntheticParagraph("/p[" .. i .. "]")
                assert(pi >= Lamport.FOREIGN_BASE and pi < 65535, "pi out of range: " .. pi)
            end
        end)

        it("is stable for the same anchors", function()
            local a1, b1 = Lamport.syntheticQuoteKey("/x", "/y")
            local a2, b2 = Lamport.syntheticQuoteKey("/x", "/y")
            assert.are.equal(a1, a2)
            assert.are.equal(b1, b2)
        end)

        it("separates different anchors", function()
            local a1 = Lamport.syntheticQuoteKey("/x", "/y")
            local a2, b2 = Lamport.syntheticQuoteKey("/x", "/z")
            local b1 = select(2, Lamport.syntheticQuoteKey("/x", "/y"))
            assert.is_true(a1 ~= a2 or b1 ~= b2)
        end)

        it("recognises its own marks and leaves the device's alone", function()
            local sw, ew = Lamport.syntheticQuoteKey("/x", "/y")
            assert.is_true(Lamport.isForeign({ q = true, sw = sw, ew = ew }))
            assert.is_false(Lamport.isForeign({ q = true, sw = 12, ew = 18 }))
            assert.is_true(Lamport.isForeign({ pi = Lamport.syntheticParagraph("/x") }))
            assert.is_false(Lamport.isForeign({ pi = 412 }))
            -- UINT16_MAX is "no paragraph anchor", not a synthetic one.
            assert.is_false(Lamport.isForeign({ pi = 65535 }))
        end)
    end)

    describe("observe", function()
        it("only ever advances the clock", function()
            assert.are.equal(9, Lamport.observe(4, 9))
            assert.are.equal(9, Lamport.observe(9, 4))
            assert.are.equal(0, Lamport.observe(nil, nil))
        end)
    end)
end)
