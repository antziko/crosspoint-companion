--- Filename normalisation must agree byte for byte with the device's, or the two sides
--- silently key the same book differently. The cases mirror KOReaderDocumentId.h's own.
local DocId = require("cp_docid")

describe("cp_docid", function()
    describe("stripDeviceTag", function()
        it("drops a leading device tag", function()
            assert.are.equal("Book.epub", DocId.stripDeviceTag("(X4) Book.epub"))
            assert.are.equal("Book.epub", DocId.stripDeviceTag("(X3) Book.epub"))
        end)

        it("drops a trailing device tag before the extension", function()
            assert.are.equal("Book.epub", DocId.stripDeviceTag("Book (X4).epub"))
        end)

        it("drops both when both are present", function()
            assert.are.equal("Bk.epub", DocId.stripDeviceTag("(X4) Bk (X4).epub"))
        end)

        it("keeps a tag that is not X3 or X4", function()
            assert.are.equal("(X12) Book.epub", DocId.stripDeviceTag("(X12) Book.epub"))
            assert.are.equal("Book (X11).epub", DocId.stripDeviceTag("Book (X11).epub"))
        end)

        it("requires the separating space", function()
            assert.are.equal("(X4)Book.epub", DocId.stripDeviceTag("(X4)Book.epub"))
            assert.are.equal("Book(X4).epub", DocId.stripDeviceTag("Book(X4).epub"))
        end)

        it("keeps an unrelated parenthesised suffix", function()
            assert.are.equal("My Bk (A).ep", DocId.stripDeviceTag("My Bk (A) (X4).ep"))
        end)

        it("leaves a plain name alone", function()
            assert.are.equal("Book.epub", DocId.stripDeviceTag("Book.epub"))
        end)
    end)

    describe("swapAuthorTitle", function()
        it("canonicalises either ordering to the same name", function()
            assert.are.equal("Dune - Smith.epub", DocId.swapAuthorTitle("Smith - Dune.epub"))
            assert.are.equal("Dune - Smith.epub", DocId.swapAuthorTitle("Dune - Smith.epub"))
        end)

        it("leaves a name with no separator alone", function()
            assert.are.equal("Dune.epub", DocId.swapAuthorTitle("Dune.epub"))
        end)

        it("leaves a name with more than one separator alone", function()
            assert.are.equal("A - B - C.epub", DocId.swapAuthorTitle("A - B - C.epub"))
        end)

        it("leaves a degenerate split alone", function()
            assert.are.equal(" - B.epub", DocId.swapAuthorTitle(" - B.epub"))
        end)
    end)

    describe("basename", function()
        it("takes the last path component", function()
            assert.are.equal("Book.epub", DocId.basename("/mnt/onboard/books/Book.epub"))
            assert.are.equal("Book.epub", DocId.basename("Book.epub"))
        end)
    end)

    describe("fromFilename", function()
        it("applies both rules, so a tagged swap keys like the plain original", function()
            assert.are.equal(DocId.fromFilename("/a/Dune - Smith.epub"),
                             DocId.fromFilename("/b/(X4) Smith - Dune.epub"))
        end)
    end)
end)

describe("cp_docid canonicalFilename", function()
    local canonical = DocId.canonicalFilename("Smith - Dune.epub")

    it("collapses every tag placement and name order onto one key", function()
        for _, name in ipairs({
            "Smith - Dune.epub",
            "Dune - Smith.epub",
            "(X4) Smith - Dune.epub",
            "(X3) Dune - Smith.epub",
            "Smith - Dune (X4).epub",
            "Dune - Smith (X3).epub",
            "(X4) Dune - Smith (X4).epub",
        }) do
            assert.are.equal(canonical, DocId.canonicalFilename(name))
        end
    end)

    -- A tag on the title rather than on the whole name is at neither end of the
    -- filename; canonicalising the order is what brings it to the front.
    it("collapses a tag leading either component", function()
        assert.are.equal(canonical, DocId.canonicalFilename("Smith - (X4) Dune.epub"))
        assert.are.equal(canonical, DocId.canonicalFilename("Dune - (X4) Smith.epub"))
        assert.are.equal(canonical, DocId.canonicalFilename("(X4) Smith - (X4) Dune.epub"))
    end)

    -- Removing the tag changes which half sorts first, so the order must be settled
    -- again afterwards. Without that, "Apple - (X4) Zed" would land on "Zed - Apple".
    it("re-sorts after the tag is removed", function()
        assert.are.equal("Apple - Zed.epub", DocId.canonicalFilename("Apple - (X4) Zed.epub"))
        assert.are.equal("Apple - Zed.epub", DocId.canonicalFilename("Zed - (X4) Apple.epub"))
        assert.are.equal("Apple - Zed.epub", DocId.canonicalFilename("Apple - Zed.epub"))
    end)

    it("does not widen what counts as a tag", function()
        assert.are.equal("(X12) Dune - Smith.epub", DocId.canonicalFilename("Smith - (X12) Dune.epub"))
        assert.are.equal("(x4) Smith - Dune.epub", DocId.canonicalFilename("(x4) Smith - Dune.epub"))
        assert.are.equal("Mac OS X (X11) - Smith.epub", DocId.canonicalFilename("Mac OS X (X11) - Smith.epub"))
        assert.are.equal("Smith - (X4) Dune - Extra.epub", DocId.canonicalFilename("Smith - (X4) Dune - Extra.epub"))
    end)

    -- Known gap, shared with the firmware: at neither end, and no reordering moves it.
    it("leaves a tag trailing the first component alone", function()
        assert.are.equal("Author (X4) - Title.epub", DocId.canonicalFilename("Author (X4) - Title.epub"))
    end)

    it("is idempotent", function()
        for _, name in ipairs({ "Smith - (X4) Dune.epub", "Apple - (X4) Zed.epub",
                                "(X4) Dune.epub", "Author (X4) - Title.epub", "Dune.epub" }) do
            local once = DocId.canonicalFilename(name)
            assert.are.equal(once, DocId.canonicalFilename(once))
        end
    end)
end)
