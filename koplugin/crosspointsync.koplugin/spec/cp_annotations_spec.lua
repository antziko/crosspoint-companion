local Annotations = require("cp_annotations")

-- A document that resolves every anchor it is told about and nothing else.
local function fakeDoc(known, text)
    return {
        isXPointerInDocument = function(_, xp) return known == nil or known[xp] == true end,
        getPageFromXPointer = function(_, _) return 7 end,
        getTextFromXPointers = function(_, _, _) return text end,
    }
end

local DT = "2026-01-01 00:00:00"

describe("cp_annotations", function()
    describe("build", function()
        it("turns a point bookmark into a KOReader bookmark", function()
            local blob = { b = { { xp = "/body/DocFragment[2]/body/p[7]", sn = "teaser", v = 5, s = 1, pi = 9 } } }
            local items, byKey, skipped = Annotations.build(blob.b, fakeDoc(), nil, DT)

            assert.are.equal(1, #items)
            assert.are.equal("/body/DocFragment[2]/body/p[7]", items[1].page)
            assert.is_nil(items[1].drawer)
            assert.are.equal("teaser", items[1].text)
            assert.are.equal(7, items[1].pageno)
            assert.are.equal(0, skipped)
            -- The whole record is handed back, not just the key: rewriting the blob
            -- later needs the identity and version fields intact.
            local rec = byKey["/body/DocFragment[2]/body/p[7]|"]
            assert.are.equal(5, rec.v)
            assert.are.equal(9, rec.pi)
        end)

        it("turns a quote into a highlight and takes its text from the document", function()
            local blob = { b = { {
                xp = "/body/DocFragment[2]/body/p[1]/text()[1].4",
                xp1 = "/body/DocFragment[2]/body/p[1]/text()[1].15",
                q = true,
                sn = "short teaser",
            } } }
            local items = Annotations.build(blob.b, fakeDoc(nil, "the real highlighted text"), nil, DT)

            assert.are.equal(1, #items)
            assert.are.equal("lighten", items[1].drawer)
            assert.are.equal("/body/DocFragment[2]/body/p[1]/text()[1].4", items[1].pos0)
            assert.are.equal("/body/DocFragment[2]/body/p[1]/text()[1].15", items[1].pos1)
            assert.are.equal("the real highlighted text", items[1].text)
        end)

        it("falls back to the stored teaser when the document yields no text", function()
            local blob = { b = { { xp = "a", xp1 = "b", q = true, sn = "teaser" } } }
            local items = Annotations.build(blob.b, fakeDoc(nil, ""), nil, DT)
            assert.are.equal("teaser", items[1].text)
        end)

        it("skips a record with no anchor rather than guessing at one", function()
            local blob = { b = { { sn = "orphan", s = 3, p = 0.4 } } }
            local items, _, skipped = Annotations.build(blob.b, fakeDoc(), nil, DT)
            assert.are.equal(0, #items)
            assert.are.equal(1, skipped)
        end)

        it("skips an anchor this document cannot resolve", function()
            local blob = { b = { { xp = "/gone" } } }
            local items, _, skipped = Annotations.build(blob.b, fakeDoc({}), nil, DT)
            assert.are.equal(0, #items)
            assert.are.equal(1, skipped)
        end)

        it("demotes a quote whose end anchor does not resolve to a bookmark", function()
            local blob = { b = { { xp = "start", xp1 = "gone", q = true, sn = "t" } } }
            local items = Annotations.build(blob.b, fakeDoc({ start = true }), nil, DT)
            assert.are.equal(1, #items)
            assert.is_nil(items[1].drawer)
            assert.are.equal("start", items[1].page)
        end)

        it("collapses duplicate records onto one item", function()
            local blob = { b = { { xp = "same" }, { xp = "same" } } }
            local items = Annotations.build(blob.b, fakeDoc(), nil, DT)
            assert.are.equal(1, #items)
        end)

        it("labels the chapter from the table of contents", function()
            local toc = { getTocTitleByPage = function(_, _) return "Chapter Three" end }
            local blob = { b = { { xp = "x" } } }
            local items = Annotations.build(blob.b, fakeDoc(), toc, DT)
            assert.are.equal("Chapter Three", items[1].chapter)
        end)

        it("tolerates an empty or absent record list", function()
            local items, byKey, skipped = Annotations.build(nil, fakeDoc(), nil, DT)
            assert.are.equal(0, #items)
            assert.are.equal(0, skipped)
            assert.is_nil(next(byKey))
        end)
    end)

    describe("existingKeys", function()
        it("keys a highlight by its pair and a bookmark by its page anchor", function()
            local keys = Annotations.existingKeys({
                { pos0 = "a", pos1 = "b", drawer = "lighten" },
                { page = "c" },
                { page = 12 },  -- a page number, not an anchor
            })
            local n = 0
            for _ in pairs(keys) do n = n + 1 end
            assert.is_true(keys["a|b"])
            assert.is_true(keys["c|"])
            assert.are.equal(2, n)  -- the numeric page contributes no key
        end)
    end)

    describe("localChanges", function()
        local function anns(...) return { ... } end

        it("reports an annotation the plugin has never seen as made here", function()
            local created = Annotations.localChanges({}, {}, anns({ page = "/new" }))
            assert.are.equal(1, #created)
            assert.are.equal("/new|", created[1].key)
        end)

        it("does not report one it is already tracking", function()
            local created = Annotations.localChanges({ ["/known|"] = { v = 1 } }, {}, anns({ page = "/known" }))
            assert.are.equal(0, #created)
        end)

        it("does not report one carried over from a pull-only release", function()
            local created = Annotations.localChanges({}, { ["/old|"] = true }, anns({ page = "/old" }))
            assert.are.equal(0, #created)
        end)

        it("reports a tracked annotation that is gone as deleted", function()
            local _, deleted = Annotations.localChanges({ ["/gone|"] = { v = 3 } }, {}, anns())
            assert.are.same({ "/gone|" }, deleted)
        end)

        it("separates a deleted stub, which has no identity to tombstone", function()
            local _, deleted, orphaned = Annotations.localChanges(
                { ["/a|"] = { v = 1 } }, { ["/b|"] = true }, anns())
            assert.are.same({ "/a|" }, deleted)
            assert.are.same({ "/b|" }, orphaned)
        end)

        it("orders what it reports, since this decides what goes on the wire", function()
            local _, deleted = Annotations.localChanges(
                { ["/c|"] = { v = 1 }, ["/a|"] = { v = 1 }, ["/b|"] = { v = 1 } }, {}, anns())
            assert.are.same({ "/a|", "/b|", "/c|" }, deleted)
        end)

        it("ignores an annotation with no anchor at all", function()
            local created = Annotations.localChanges({}, {}, anns({ page = 12 }))
            assert.are.equal(0, #created)
        end)

        it("treats a highlight and a bookmark at the same start as different marks", function()
            local created = Annotations.localChanges({ ["/x|"] = { v = 1 } }, {},
                anns({ page = "/x" }, { pos0 = "/x", pos1 = "/y" }))
            assert.are.equal(1, #created)
            assert.are.equal("/x|/y", created[1].key)
        end)
    end)

    describe("indicesToRemove", function()
        it("returns matching indices highest first, so removal does not shift them", function()
            local anns = {
                { page = "keep" },
                { pos0 = "drop1", pos1 = "x" },
                { page = "drop2" },
                { page = "keep2" },
            }
            local idx = Annotations.indicesToRemove(anns, { ["drop1|x"] = true, ["drop2|"] = true })
            assert.are.same({ 3, 2 }, idx)
        end)

        it("never touches an annotation the plugin did not insert", function()
            local anns = { { pos0 = "mine", pos1 = "m" }, { pos0 = "theirs", pos1 = "t" } }
            local idx = Annotations.indicesToRemove(anns, { ["mine|m"] = true })
            assert.are.same({ 1 }, idx)
        end)
    end)
end)
