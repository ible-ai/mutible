// remote_cache_tests.inc — RemoteCache.h: a window over SSH showing files
// in the viewers through copies read from its host (the copies' folder
// names, where a copy goes, how a file is shown, the size limit's
// sentence, a notice that may have changed a copy, the fingerprint, and
// the remote file a Markdown picture names).
#include "RemoteCache.h"

static void testRemoteCache() {
    using RemoteCache::Show;

    GROUP("remotecache:folders");
    {
        CHECK(RemoteCache::windowFolderName(4321, 7) == "w4321-7");
        long pid = 0;
        CHECK(RemoteCache::ownerOfFolder("w4321-7", &pid) && pid == 4321);
        CHECK(RemoteCache::ownerOfFolder(RemoteCache::windowFolderName(1, 0), &pid) && pid == 1);
        // Anything else is never taken for a window's folder (never removed).
        for (const char* bad : {"", "w", "w-1", "w12", "w12-", "x12-3", "w12-3a", "w1a-3", "w0-3", "W12-3",
                                "w12-3/x", "w1234567890123-1", ".DS_Store"})
            CHECK(!RemoteCache::ownerOfFolder(bad, &pid));
    }

    GROUP("remotecache:place");
    {
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj/pic.png") == "pic.png");
        CHECK(RemoteCache::placeOf("/home/eric/proj/", "/home/eric/proj/src/a/pic.png") == "src/a/pic.png");
        CHECK(RemoteCache::placeOf("/", "/etc/x.pdf") == "etc/x.pdf");
        // Two files of one name in two folders: two places.
        CHECK(RemoteCache::placeOf("/p", "/p/a/x.png") != RemoteCache::placeOf("/p", "/p/b/x.png"));
        // Not inside, the folder itself, or a name that is none.
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/project/x.png").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj/").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj/../x.png").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj/a/./x.png").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", "/home/eric/proj/a//x.png").empty());
        CHECK(RemoteCache::placeOf("/home/eric/proj", std::string("/home/eric/proj/a\0b", 19)).empty());
        CHECK(RemoteCache::placeOf("", "/x.png").empty());
        CHECK(RemoteCache::placeOf("proj", "proj/x.png").empty());
    }

    GROUP("remotecache:show");
    {
        const std::uint64_t table = 50ull << 20;
        const std::string png = "\x89PNG\r\n\x1a\n" + std::string("\0\0\0\rIHDR", 8);
        const std::string sqlite = std::string("SQLite format 3\0", 16) + std::string(84, '\0');
        // By name first: pictures, PDFs, players, whatever the bytes.
        CHECK(RemoteCache::showFor("/p/a.png", false, png, false, table) == Show::Image);
        CHECK(RemoteCache::showFor("/p/a.JPG", false, "x", true, table) == Show::Image);
        CHECK(RemoteCache::showFor("/p/doc.pdf", false, "%PDF-1.7", true, table) == Show::Pdf);
        CHECK(RemoteCache::showFor("/p/clip.mov", false, "", false, table) == Show::Media);
        CHECK(RemoteCache::showFor("/p/song.mp3", false, "", false, table) == Show::Media);
        // Over the limit: the placeholder, whatever it is.
        CHECK(RemoteCache::showFor("/p/a.png", true, "", false, table) == Show::TooLarge);
        CHECK(RemoteCache::showFor("/p/a.txt", true, "", false, table) == Show::TooLarge);
        // By the head: a database and a document only Quick Look shows.
        CHECK(RemoteCache::showFor("/p/app.db", false, sqlite, false, table) == Show::Viewer);
        CHECK(RemoteCache::showFor("/p/data", false, sqlite, false, table) == Show::Viewer);
        CHECK(RemoteCache::showFor("/p/report.docx", false, "PK\x03\x04", false, table) == Show::Viewer);
        // Text, and the viewers over and beside text, stay the editor's.
        CHECK(RemoteCache::showFor("/p/a.c", false, "int x;\n", true, table) == Show::Text);
        CHECK(RemoteCache::showFor("/p/a.csv", false, "a,b\n1,2\n", true, table) == Show::Text);
        CHECK(RemoteCache::showFor("/p/a.json", false, "{}", true, table) == Show::Text);
        CHECK(RemoteCache::showFor("/p/a.ipynb", false, "{\"cells\": []}", true, table) == Show::Text);
        CHECK(RemoteCache::showFor("/p/README.md", false, "# hi\n", true, table) == Show::Text);
        // Binary by content, or bytes that do not decode: Quick Look's.
        CHECK(RemoteCache::showFor("/p/a.bin", false, std::string("ab\0cd", 5), false, table) == Show::Viewer);
        CHECK(RemoteCache::showFor("/p/a.txt", false, "\xff\xfe\xfd", false, table) == Show::Viewer);
        // A table over the editable size reads from its file; a UTF-16 one
        // that size is text (large-file mode), as on this machine.
        const std::string big = "a,b\n" + std::string(64, 'x') + ",1\n";
        CHECK(RemoteCache::showFor("/p/big.csv", false, big, true, 16) == Show::Viewer);
        CHECK(RemoteCache::showFor("/p/big.csv", false, big, true, table) == Show::Text);
        const std::string utf16 = std::string("\xff\xfe" "a\0,\0b\0\n\0", 10) + std::string("x\0", 2) + std::string("y\0", 2) + std::string("z\0", 2);
        CHECK(RemoteCache::showFor("/p/big.tsv", false, utf16, true, 16) == Show::Text);
        CHECK(std::string(RemoteCache::showName(Show::TooLarge)) == "too-large");
        CHECK(std::string(RemoteCache::showName(Show::Image)) == "image");
    }

    GROUP("remotecache:message");
    {
        const std::string m = RemoteCache::tooLargeMessage("movie.mp4", "thinkpad:~/proj");
        CHECK(m.find("“movie.mp4” is too large to open over SSH.") == 0);
        CHECK(m.find("64 MB") != std::string::npos);
        CHECK(m.find("thinkpad:~/proj") != std::string::npos);
        CHECK(RemoteCache::kMaxBytes == (std::size_t(64) << 20));
    }

    GROUP("remotecache:refresh");
    {
        CHECK(RemoteCache::mayHaveChanged({"/p", "/p/src"}, "/p/pic.png"));
        CHECK(RemoteCache::mayHaveChanged({"/p/src/"}, "/p/src/pic.png"));
        CHECK(!RemoteCache::mayHaveChanged({"/p"}, "/p/src/pic.png"));     // only its own folder
        CHECK(!RemoteCache::mayHaveChanged({"/p/src"}, "/p/pic.png"));
        CHECK(!RemoteCache::mayHaveChanged({"/p/sr"}, "/p/src/pic.png"));
        CHECK(!RemoteCache::mayHaveChanged({}, "/p/pic.png"));
        CHECK(!RemoteCache::mayHaveChanged({"/p"}, "pic.png"));
        CHECK(RemoteCache::mayHaveChanged({"/"}, "/pic.png"));
        // The fingerprint: the same bytes the same, any change another.
        const std::string a(1000, 'a');
        std::string b = a;
        b[999] = 'b';
        CHECK(RemoteCache::fingerprint(a) == RemoteCache::fingerprint(std::string(1000, 'a')));
        CHECK(RemoteCache::fingerprint(a) != RemoteCache::fingerprint(b));
        CHECK(RemoteCache::fingerprint("") != RemoteCache::fingerprint(std::string(1, '\0')));
        CHECK(RemoteCache::fingerprint("") == 14695981039346656037ull);
    }

    GROUP("remotecache:markdown");
    {
        const std::string root = "/home/eric/proj", doc = "/home/eric/proj/docs";
        CHECK(RemoteCache::markdownPicture("pic.png", doc, root) == "/home/eric/proj/docs/pic.png");
        CHECK(RemoteCache::markdownPicture("./img/a.png", doc, root) == "/home/eric/proj/docs/img/a.png");
        CHECK(RemoteCache::markdownPicture("../shots/b.png", doc, root) == "/home/eric/proj/shots/b.png");
        CHECK(RemoteCache::markdownPicture("my%20pic.png?raw=1#top", doc, root) == "/home/eric/proj/docs/my pic.png");
        CHECK(RemoteCache::markdownPicture("/home/eric/proj/x.png", doc, root) == "/home/eric/proj/x.png");
        CHECK(RemoteCache::markdownPicture("pic.png", doc + "/", root + "/") == "/home/eric/proj/docs/pic.png");
        // Nothing outside the folder, and nothing of this machine's.
        CHECK(RemoteCache::markdownPicture("../../secret.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("../../../../../../etc/x.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("/etc/x.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("~/x.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("file:///home/eric/proj/x.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("https://example.com/x.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("data:image/png;base64,AAAA", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("DATA:image/png;base64,AAAA", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("#top", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("..", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("a%00b.png", doc, root).empty());
        CHECK(RemoteCache::markdownPicture("pic.png", "", root).empty());
        // A % not followed by two hex digits stays as written.
        CHECK(RemoteCache::markdownPicture("100%.png", doc, root) == "/home/eric/proj/docs/100%.png");
    }
}
