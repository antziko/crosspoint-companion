--- The menu shows one mode where the settings hold two booleans, so what matters is that
--- the round trip is lossless and that a configuration written before the modes existed
--- keeps doing what it did.
local Modes = require("cp_modes")

describe("cp_modes", function()
    describe("annotations", function()
        it("reads a settings table written before the off switch existed as on", function()
            assert.are.equal(Modes.RECEIVE, Modes.annotations({}))
            assert.are.equal(Modes.TWO_WAY, Modes.annotations({ push_changes = true }))
        end)

        it("is off only when turned off", function()
            assert.are.equal(Modes.OFF, Modes.annotations({ sync_annotations = false }))
            assert.are.equal(Modes.OFF, Modes.annotations({ sync_annotations = false, push_changes = true }))
        end)

        it("round-trips every mode", function()
            for _, mode in ipairs({ Modes.OFF, Modes.RECEIVE, Modes.TWO_WAY }) do
                local s = Modes.setAnnotations({}, mode)
                assert.are.equal(mode, Modes.annotations(s))
            end
        end)

        it("stops uploading when turned off, so turning it back on does not resume sending", function()
            local s = Modes.setAnnotations({}, Modes.TWO_WAY)
            Modes.setAnnotations(s, Modes.OFF)
            assert.is_false(s.push_changes)
        end)
    end)

    describe("progress", function()
        it("is off by default", function()
            assert.are.equal(Modes.OFF, Modes.progress({}))
        end)

        it("reads every one of the four stored combinations", function()
            assert.are.equal(Modes.RECEIVE, Modes.progress({ sync_progress = true }))
            assert.are.equal(Modes.SEND, Modes.progress({ push_progress = true }))
            assert.are.equal(Modes.TWO_WAY, Modes.progress({ sync_progress = true, push_progress = true }))
        end)

        it("round-trips every mode", function()
            for _, mode in ipairs({ Modes.OFF, Modes.RECEIVE, Modes.SEND, Modes.TWO_WAY }) do
                local s = Modes.setProgress({}, mode)
                assert.are.equal(mode, Modes.progress(s))
            end
        end)

        it("sends without receiving, which is what send-only has to mean", function()
            local s = Modes.setProgress({}, Modes.SEND)
            assert.is_true(s.push_progress)
            assert.is_false(s.sync_progress)
        end)

        it("stops sending when set back to receive", function()
            local s = Modes.setProgress({}, Modes.TWO_WAY)
            Modes.setProgress(s, Modes.RECEIVE)
            assert.is_false(s.push_progress)
            assert.is_true(s.sync_progress)
        end)
    end)
end)
