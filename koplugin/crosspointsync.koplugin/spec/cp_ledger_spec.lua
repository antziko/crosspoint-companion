local Ledger = require("cp_ledger")

local function fakeSettings(seed)
    local store = { data = seed or {} }
    function store:readSetting(key) return self.data[key] end
    function store:saveSetting(key, value) self.data[key] = value end
    function store:delSetting(key) self.data[key] = nil end
    return store
end

describe("cp_ledger", function()
    it("starts empty for a book that has never synced", function()
        local l = Ledger.load(fakeSettings())
        assert.are.equal(0, l.clock)
        assert.is_nil(next(l.marks))
        assert.is_nil(next(l.stubs))
    end)

    it("tolerates no settings store at all", function()
        assert.are.equal(0, Ledger.load(nil).clock)
    end)

    describe("deletions owed to the server", function()
        it("round-trips the whole record, which is all a tombstone can be built from", function()
            local s = fakeSettings()
            local rec = { v = 4, s = 1, pi = 9, xp = "/a" }
            Ledger.save(s, { marks = {}, pending = { ["/a|"] = rec }, clock = 4 })
            local l = Ledger.load(s)
            assert.are.same(rec, l.pending["/a|"])
        end)

        it("leaves no key behind once nothing is owed", function()
            local s = fakeSettings()
            Ledger.save(s, { marks = {}, pending = { ["/a|"] = { v = 1 } }, clock = 1 })
            assert.is_true(s.data[Ledger.PENDING_KEY] ~= nil)
            Ledger.save(s, { marks = {}, pending = {}, clock = 2 })
            assert.is_nil(s.data[Ledger.PENDING_KEY])
        end)

        it("is dropped by forget, along with everything else", function()
            local s = fakeSettings()
            Ledger.save(s, { marks = {}, pending = { ["/a|"] = { v = 1 } }, clock = 1 })
            Ledger.forget(s)
            assert.is_nil(s.data[Ledger.PENDING_KEY])
            assert.is_nil(next(Ledger.load(s).pending))
        end)

        it("reads back empty from a sidecar written before it existed", function()
            local s = fakeSettings({ [Ledger.MARKS_KEY] = { ["/a|"] = { v = 4 } } })
            assert.is_nil(next(Ledger.load(s).pending))
        end)
    end)

    it("carries a pull-only release's key set over as stubs", function()
        local s = fakeSettings({ [Ledger.LEGACY_KEY] = { ["/a|"] = true, ["/b|/c"] = true } })
        local l = Ledger.load(s)
        assert.is_true(l.stubs["/a|"])
        assert.is_true(l.stubs["/b|/c"])
        assert.is_nil(next(l.marks))
        -- A stub is still enough to know the annotation is this plugin's.
        assert.is_true(Ledger.manages(l, "/a|"))
    end)

    it("prefers a real record over a stub for the same key", function()
        local s = fakeSettings({
            [Ledger.LEGACY_KEY] = { ["/a|"] = true },
            [Ledger.MARKS_KEY] = { ["/a|"] = { v = 4, s = 1, pi = 9 } },
        })
        local l = Ledger.load(s)
        assert.is_nil(l.stubs["/a|"])
        assert.are.equal(4, l.marks["/a|"].v)
    end)

    it("keeps the legacy set until the last stub is absorbed", function()
        local s = fakeSettings({ [Ledger.LEGACY_KEY] = { ["/a|"] = true, ["/b|"] = true } })
        local l = Ledger.load(s)
        l.stubs["/a|"] = nil
        Ledger.save(s, l)
        assert.is_true(s.data[Ledger.LEGACY_KEY] ~= nil)
        l.stubs = {}
        Ledger.save(s, l)
        assert.is_nil(s.data[Ledger.LEGACY_KEY])
    end)

    it("round-trips records and the clock", function()
        local s = fakeSettings()
        Ledger.save(s, { marks = { ["/a|"] = { v = 12, s = 3, sn = "hi" } }, stubs = {}, clock = 12 })
        local back = Ledger.load(s)
        assert.are.equal(12, back.clock)
        assert.are.equal("hi", back.marks["/a|"].sn)
    end)

    it("offers every record as a live merge entry, and no stub", function()
        local s = fakeSettings({
            [Ledger.MARKS_KEY] = { ["/a|"] = { v = 3 }, ["/b|"] = { v = 8 } },
            [Ledger.LEGACY_KEY] = { ["/c|"] = true },
        })
        local entries = Ledger.entries(Ledger.load(s))
        assert.are.equal(2, #entries)
        for _, e in ipairs(entries) do assert.is_true(e.live) end
    end)

    it("forgets everything, legacy set included", function()
        local s = fakeSettings({
            [Ledger.MARKS_KEY] = { ["/a|"] = { v = 1 } },
            [Ledger.CLOCK_KEY] = 5,
            [Ledger.LEGACY_KEY] = { ["/b|"] = true },
        })
        Ledger.forget(s)
        local l = Ledger.load(s)
        assert.are.equal(0, l.clock)
        assert.is_nil(next(l.marks))
        assert.is_nil(next(l.stubs))
    end)
end)
