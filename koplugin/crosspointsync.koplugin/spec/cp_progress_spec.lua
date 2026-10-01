--- The reading position moves the reader, so the cases that must NOT move it are the ones
--- worth pinning down: a record from where we already are, and one this book cannot use.
local Progress = require("cp_progress")

local XP = "/body/DocFragment[8]/body/div[2]/p[4]/text()[1].96"

describe("cp_progress", function()
    describe("roundPercent", function()
        it("keeps four decimals, as kosync compares at", function()
            assert.are.equal(0.1235, Progress.roundPercent(0.12345))
            assert.are.equal(0.5, Progress.roundPercent(0.5))
        end)

        it("returns nil for anything that is not a number", function()
            assert.is_nil(Progress.roundPercent(nil))
            assert.is_nil(Progress.roundPercent("later"))
        end)
    end)

    describe("decide", function()
        it("does nothing without a reply", function()
            assert.are.equal(Progress.NO_REPLY, Progress.decide(nil, {}).reason)
        end)

        it("does nothing when the server holds no position", function()
            assert.are.equal(Progress.NO_RECORD, Progress.decide({}, {}).reason)
            assert.are.equal(Progress.NO_RECORD, Progress.decide({ progress = "" }, {}).reason)
        end)

        it("jumps to an anchor this document does not already sit on", function()
            local v = Progress.decide({ progress = XP, percentage = 0.42, device = "X4 Pro" },
                                      { progress = "/body/DocFragment[2]/body/p[1]/text()[1].0",
                                        percentage = 0.1 })
            assert.are.equal("jump", v.action)
            assert.are.equal(XP, v.target)
            assert.are.equal(0.42, v.percentage)
            assert.are.equal("X4 Pro", v.device)
            assert.is_true(v.forward)
        end)

        it("offers a backwards jump too", function()
            local v = Progress.decide({ progress = XP, percentage = 0.1 },
                                      { progress = "elsewhere", percentage = 0.42 })
            assert.are.equal("jump", v.action)
            assert.is_false(v.forward)
        end)

        it("stays put when the anchor is the one we are on", function()
            local v = Progress.decide({ progress = XP, percentage = 0.9 },
                                      { progress = XP, percentage = 0.1 })
            assert.are.equal(Progress.SAME_PLACE, v.reason)
        end)

        -- Two devices at the same place disagree in the last bits of a float, so an
        -- equal-to-four-decimals percentage is the same position, not a jump.
        it("stays put when the percentages agree to four decimals", function()
            local v = Progress.decide({ progress = XP, percentage = 0.123456 },
                                      { progress = "elsewhere", percentage = 0.1234561 })
            assert.are.equal(Progress.SAME_PLACE, v.reason)
        end)

        it("moves when they differ beyond four decimals", function()
            local v = Progress.decide({ progress = XP, percentage = 0.1234 },
                                      { progress = "elsewhere", percentage = 0.1236 })
            assert.are.equal("jump", v.action)
        end)

        -- A CrossPoint device only reads EPUBs, so its anchor is always an XPointer; a
        -- paged copy of the same title has nowhere to put it.
        it("refuses an EPUB anchor in a paged document", function()
            local v = Progress.decide({ progress = XP, percentage = 0.42 }, { has_pages = true })
            assert.are.equal(Progress.NOT_USABLE, v.reason)
        end)

        it("takes a page number in a paged document", function()
            local v = Progress.decide({ progress = "57", percentage = 0.42 }, { has_pages = true })
            assert.are.equal("jump", v.action)
            assert.are.equal(57, v.target)
        end)

        it("refuses a non-string anchor in a reflow document", function()
            assert.are.equal(Progress.NOT_USABLE, Progress.decide({ progress = 57 }, {}).reason)
        end)

        it("jumps when this document has no position of its own yet", function()
            local v = Progress.decide({ progress = XP, percentage = 0.42 }, {})
            assert.are.equal("jump", v.action)
        end)
    end)

    describe("shouldPush", function()
        it("sends a position nothing has agreed on yet", function()
            assert.is_true(Progress.shouldPush({ progress = XP, percentage = 0.42 }, nil))
        end)

        it("sends once the reader has moved on", function()
            assert.is_true(Progress.shouldPush({ progress = XP, percentage = 0.42 }, "elsewhere"))
        end)

        it("stays quiet at a position already agreed on", function()
            local ok, why = Progress.shouldPush({ progress = XP, percentage = 0.42 }, XP)
            assert.is_false(ok)
            assert.are.equal(Progress.SAME_PLACE, why)
        end)

        it("has nothing to send without a position", function()
            assert.is_false(Progress.shouldPush({ percentage = 0.42 }, nil))
            assert.is_false(Progress.shouldPush({ progress = "", percentage = 0.42 }, nil))
        end)

        -- The server refuses a record with no percentage, so sending one is a wasted
        -- round trip that reports success to nobody.
        it("has nothing to send without a percentage", function()
            local ok, why = Progress.shouldPush({ progress = XP }, nil)
            assert.is_false(ok)
            assert.are.equal(Progress.NOT_USABLE, why)
        end)
    end)
end)
