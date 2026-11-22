// session_fileops_tests.inc — stage 8's file operations from a tree over the
// host (SessionFiles.h's doFileOp, SessionHost.h's onFileOp, Welcome flag
// 64, Client::requestFileOp): rename, move, delete, new file and new folder
// in the session's folder, the open documents following, every refusal.
// Included after session_disk_tests.inc, whose realScratch it uses, and
// session_host_tests.inc's Net and TestMirror.

namespace {

// One file operation, asked and answered.
Json fileOpAsk(Net::Link& l, const char* op, const std::string& path, const std::string& arg = "",
               const char* argName = "name") {
    Json q = Json::object();
    q.set("op", Json(op));
    q.set("path", Json(path));
    if (!arg.empty() || std::string(argName) == "to") q.set(argName, Json(arg));
    if (std::string(op) == "delete") q.set("permanent", Json(true));
    const std::uint64_t req = l.client->requestFileOp(0, std::move(q));
    l.client->dispatch();
    Json a;
    if (!req || !l.client->readAnswer(req, &a)) return Json();
    return a;
}

std::string docPathOf(Net& net, Shared::DocId doc) {
    const Json st = net.host.state();
    for (const Json& d : st["documents"].elements())
        if ((Shared::DocId)d["doc"].asInt() == doc) return d["path"].asString();
    return "?";
}

}  // namespace

static void testSessionFileOps() {
    namespace fs = std::filesystem;

    GROUP("hostfiles:op");
    {
        const std::string root = realScratch(), elsewhere = realScratch();
        fs::create_directories(root + "/src/deep");
        fs::create_directories(root + "/dest");
        fsWrite(root + "/a.txt", "a\n");
        fsWrite(root + "/src/deep/x.py", "x\n");
        fsWrite(root + "/src/y.py", "y\n");
        fsWrite(root + "/taken.txt", "taken\n");
        fsWrite(elsewhere + "/out.txt", "out\n");
        symlink(elsewhere.c_str(), (root + "/outdir").c_str());
        symlink((elsewhere + "/out.txt").c_str(), (root + "/outlink.txt").c_str());
        Net net(true, true);
        net.host.setFolder(root);
        net.host.setWatching(true);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8 | 32);
        Net::Link& reader = net.add(Unit::Utf16, "reader", 1);
        guest.client->dispatch();
        reader.client->dispatch();
        CHECK(guest.client->hostDoesFileOps());

        // Rename: a file, a folder; nothing replaced.
        Json a = fileOpAsk(guest, "rename", "a.txt", "b.txt");
        CHECK(a["ok"].asBool() && a["to"].asString() == "b.txt" && !a["dir"].asBool());
        CHECK(!pathThere(root + "/a.txt") && readFile((root + "/b.txt").c_str()) == "a\n");
        a = fileOpAsk(guest, "rename", "src/deep", "deeper");
        CHECK(a["ok"].asBool() && a["to"].asString() == "src/deeper" && a["dir"].asBool());
        CHECK(readFile((root + "/src/deeper/x.py").c_str()) == "x\n");
        a = fileOpAsk(guest, "rename", "b.txt", "taken.txt");
        CHECK(!a["ok"].asBool() && a["reason"].asString() == "exists" && a["error"].asString() == "taken.txt already exists.");
        CHECK(readFile((root + "/taken.txt").c_str()) == "taken\n" && pathThere(root + "/b.txt"));

        // Move: a file into a folder, a folder into the folder, to the top.
        a = fileOpAsk(guest, "move", "b.txt", "dest", "to");
        CHECK(a["ok"].asBool() && a["to"].asString() == "dest/b.txt" && pathThere(root + "/dest/b.txt") &&
              !pathThere(root + "/b.txt"));
        a = fileOpAsk(guest, "move", "src/deeper", "dest", "to");
        CHECK(a["ok"].asBool() && a["to"].asString() == "dest/deeper" && a["dir"].asBool() &&
              pathThere(root + "/dest/deeper/x.py"));
        a = fileOpAsk(guest, "move", "dest/b.txt", "", "to");
        CHECK(a["ok"].asBool() && a["to"].asString() == "b.txt" && pathThere(root + "/b.txt"));
        a = fileOpAsk(guest, "move", "dest", "dest/deeper", "to");
        CHECK(!a["ok"].asBool() && a["reason"].asString() == "into-itself" && pathThere(root + "/dest/deeper"));

        // New file, new folder.
        a = fileOpAsk(guest, "newFile", "src/new.txt");
        CHECK(a["ok"].asBool() && !a["dir"].asBool() && readFile((root + "/src/new.txt").c_str()).empty() &&
              pathThere(root + "/src/new.txt"));
        a = fileOpAsk(guest, "newFolder", "src/sub");
        CHECK(a["ok"].asBool() && a["dir"].asBool() && fs::is_directory(root + "/src/sub"));
        a = fileOpAsk(guest, "newFile", "src/new.txt");
        CHECK(!a["ok"].asBool() && a["reason"].asString() == "exists");
        a = fileOpAsk(guest, "newFolder", "nowhere/sub");
        CHECK(!a["ok"].asBool() && a["reason"].asString() == "missing" && !pathThere(root + "/nowhere"));

        // Delete: not without "permanent"; a file, a folder with its insides,
        // a link (never its target).
        Json ask = Json::object();
        ask.set("op", Json("delete"));
        ask.set("path", Json("taken.txt"));
        std::uint64_t req = guest.client->requestFileOp(0, ask);
        guest.client->dispatch();
        CHECK(req && guest.client->readAnswer(req, &a) && a["reason"].asString() == "notrash" && pathThere(root + "/taken.txt"));
        a = fileOpAsk(guest, "delete", "taken.txt");
        CHECK(a["ok"].asBool() && !pathThere(root + "/taken.txt"));
        a = fileOpAsk(guest, "delete", "src");
        CHECK(a["ok"].asBool() && a["dir"].asBool() && !pathThere(root + "/src"));
        a = fileOpAsk(guest, "delete", "outlink.txt");
        CHECK(a["ok"].asBool() && !pathThere(root + "/outlink.txt") && readFile((elsewhere + "/out.txt").c_str()) == "out\n");

        // The tree hears: a listing again with the new names (the guest's tree
        // had the top folder open).
        guest.client->watchFolders(0, {""}, false);
        guest.client->dispatch();
        const std::size_t heard = guest.changed.size();
        a = fileOpAsk(guest, "newFile", "fresh.txt");
        guest.client->dispatch();
        CHECK(a["ok"].asBool() && guest.changed.size() > heard);

        // A reader changes nothing.
        a = fileOpAsk(reader, "newFile", "r.txt");
        CHECK(!a["ok"].asBool() && a["reason"].asString() == "readonly" && !pathThere(root + "/r.txt"));
        a = fileOpAsk(reader, "delete", "b.txt");
        CHECK(!a["ok"].asBool() && pathThere(root + "/b.txt"));
    }

    GROUP("hostfiles:refuse");
    {
        const std::string root = realScratch(), elsewhere = realScratch();
        fs::create_directories(root + "/dir");
        fsWrite(root + "/f.txt", "f\n");
        fsWrite(root + "/dir/g.txt", "g\n");
        fsWrite(elsewhere + "/out.txt", "out\n");
        fs::create_directories(elsewhere + "/odir");
        symlink(elsewhere.c_str(), (root + "/outdir").c_str());
        symlink((elsewhere + "/out.txt").c_str(), (root + "/outlink.txt").c_str());
        Net net(true, true);
        net.host.setFolder(root);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8);
        guest.client->dispatch();
        auto refused = [&](const Json& a, const char* reason) {
            const bool ok = !a["ok"].asBool() && a["reason"].asString() == reason && !a["error"].asString().empty();
            if (!ok) std::printf("  hostfiles:refuse: %s (wanted %s)\n", a.dump().c_str(), reason);
            return ok;
        };
        // Paths that are not the folder's own.
        CHECK(refused(fileOpAsk(guest, "rename", "../out.txt", "x"), "path"));
        CHECK(refused(fileOpAsk(guest, "rename", "dir/../f.txt", "x"), "path"));
        CHECK(refused(fileOpAsk(guest, "delete", "/etc/passwd"), "path"));
        CHECK(refused(fileOpAsk(guest, "delete", ""), "path"));
        CHECK(refused(fileOpAsk(guest, "move", "f.txt", "../", "to"), "path"));
        CHECK(refused(fileOpAsk(guest, "newFile", std::string("a\0b", 3)), "path"));
        // A link out of the folder: nothing made, moved or deleted through it.
        CHECK(refused(fileOpAsk(guest, "newFile", "outdir/x.txt"), "outside"));
        CHECK(refused(fileOpAsk(guest, "newFolder", "outdir/x"), "outside"));
        CHECK(refused(fileOpAsk(guest, "delete", "outdir/out.txt"), "outside"));
        CHECK(refused(fileOpAsk(guest, "rename", "outdir/out.txt", "z"), "outside"));
        CHECK(refused(fileOpAsk(guest, "move", "f.txt", "outdir", "to"), "outside"));
        CHECK(!pathThere(elsewhere + "/x.txt") && !pathThere(elsewhere + "/x") && readFile((elsewhere + "/out.txt").c_str()) == "out\n" &&
              pathThere(root + "/f.txt"));
        // Missing sources and folders.
        CHECK(refused(fileOpAsk(guest, "rename", "none.txt", "x"), "missing"));
        CHECK(refused(fileOpAsk(guest, "delete", "none.txt"), "missing"));
        CHECK(refused(fileOpAsk(guest, "move", "f.txt", "nodir", "to"), "missing"));
        CHECK(refused(fileOpAsk(guest, "move", "none.txt", "dir", "to"), "missing"));
        // Targets there already: never replaced.
        CHECK(fileOpAsk(guest, "move", "f.txt", "dir", "to")["ok"].asBool());   // dir/g.txt is not f.txt: it moves
        CHECK(pathThere(root + "/dir/f.txt") && !pathThere(root + "/f.txt"));
        fsWrite(root + "/f.txt", "f2\n");
        CHECK(refused(fileOpAsk(guest, "move", "f.txt", "dir", "to"), "exists"));
        CHECK(refused(fileOpAsk(guest, "rename", "f.txt", "dir"), "exists"));
        CHECK(refused(fileOpAsk(guest, "rename", "f.txt", "f.txt"), "exists"));
        CHECK(refused(fileOpAsk(guest, "newFolder", "dir"), "exists"));
        CHECK(readFile((root + "/f.txt").c_str()) == "f2\n" && readFile((root + "/dir/f.txt").c_str()) == "f\n");
        // Names.
        for (const char* bad : {"", ".", "..", "a/b", "/x"})
            CHECK(refused(fileOpAsk(guest, "rename", "f.txt", bad), "name"));
        CHECK(refused(fileOpAsk(guest, "rename", "f.txt", std::string("a\0b", 3)), "name"));
        CHECK(refused(fileOpAsk(guest, "rename", "f.txt", std::string(300, 'x')), "name"));
        CHECK(refused(fileOpAsk(guest, "newFile", std::string(300, 'x')), "name"));
        CHECK(pathThere(root + "/f.txt") && !pathThere(root + "/a"));
        // No permanence, no delete; an unknown op.
        Json ask = Json::object();
        ask.set("op", Json("delete"));
        ask.set("path", Json("f.txt"));
        const std::uint64_t req = guest.client->requestFileOp(0, ask);
        guest.client->dispatch();
        Json a;
        CHECK(req && guest.client->readAnswer(req, &a) && refused(a, "notrash") && pathThere(root + "/f.txt"));
        // The folder itself is not a source.
        CHECK(refused(fileOpAsk(guest, "rename", "", "x"), "path"));
        // An older host: no flag, no request.
        Net old(true, true);
        old.host.setFolder(root);
        Net::Link& ol = old.add(Unit::Utf16, "browser", 2);
        ol.client->dispatch();
        CHECK(ol.client->hostDoesFileOps());
        Message w;
        w.kind = Shared::Kind::Welcome;
        w.flag = 1 | 2 | 4 | 8 | 16 | 32;
        ol.client->deliver(std::move(w));
        ol.client->dispatch();
        CHECK(!ol.client->hostDoesFileOps() && ol.client->requestFileOp(0, ask) == 0 && pathThere(root + "/f.txt"));
    }

    GROUP("hostfiles:docs");
    {
        const std::string root = realScratch();
        fs::create_directories(root + "/sub/deep");
        fs::create_directories(root + "/other");
        fsWrite(root + "/one.txt", "one\n");
        fsWrite(root + "/sub/two.txt", "two\n");
        fsWrite(root + "/sub/deep/three.txt", "three\n");
        fsWrite(root + "/four.txt", "four\n");
        Net net(true, true);
        net.host.setFolder(root);
        net.host.setWatching(true);
        Net::Link& guest = net.add(Unit::Utf16, "browser", 2 | 8 | 32);
        Net::Link& app = net.add(Unit::Utf16, "app", 4 | 8 | 16);
        Net::Link& ed = net.add(Unit::Utf16, "editor", 8 | 32);
        for (Net::Link* l : {&guest, &app, &ed}) l->client->dispatch();
        TestMirror am, bm, cm, dm;
        for (auto* m : {&am, &bm, &cm, &dm}) m->client = nullptr;
        am.client = app.client.get();
        am.doc = 1;
        am.text = u"one\n";
        app.client->open(1, root + "/one.txt", "one\n", &am);
        bm.client = ed.client.get();
        bm.doc = 2;
        bm.text = u"two\n";
        ed.client->open(2, root + "/sub/two.txt", "two\n", &bm);
        cm.client = ed.client.get();
        cm.doc = 3;
        cm.text = u"three\n";
        ed.client->open(3, root + "/sub/deep/three.txt", "three\n", &cm);
        dm.client = ed.client.get();
        dm.doc = 4;
        dm.text = u"four\n";
        ed.client->open(4, root + "/four.txt", "four\n", &dm);
        for (Net::Link* l : {&guest, &app, &ed}) l->client->settle();
        guest.writes.clear();
        app.writes.clear();
        ed.writes.clear();

        // A file renamed: its document's path, every renderer that hears
        // DocDirty told, text and history as they were.
        Json a = fileOpAsk(guest, "rename", "one.txt", "uno.txt");
        CHECK(a["ok"].asBool() && a["docs"].size() == 1 && a["docs"][0]["doc"].asInt() == 1 &&
              a["docs"][0]["path"].asString() == root + "/uno.txt");
        for (Net::Link* l : {&guest, &app, &ed}) l->client->dispatch();
        CHECK(docPathOf(net, 1) == root + "/uno.txt" && net.host.text(1) == u"one\n" && !net.host.dirty(1));
        for (Net::Link* l : {&guest, &app, &ed}) {
            CHECK(l->writes.size() == 1);
            if (l->writes.size() == 1)
                CHECK(l->writes[0].doc == 1 && l->writes[0].json["path"].asString() == root + "/uno.txt" &&
                      l->writes[0].json["from"].asString() == root + "/one.txt" && l->writes[0].json["moved"].asBool());
        }
        // The host saves to the new path.
        am.caret = am.length();
        am.type(u"more\n");
        app.client->settle();
        Shared::Client::SaveAnswer s;
        const std::uint64_t sr = app.client->requestSave(1, 0, false);
        app.client->dispatch();
        CHECK(sr && app.client->saveAnswer(sr, &s) && s.ok && readFile((root + "/uno.txt").c_str()) == "one\nmore\n");

        // A folder moved: the documents under it, deeper ones too.
        guest.writes.clear();
        a = fileOpAsk(guest, "move", "sub", "other", "to");
        CHECK(a["ok"].asBool() && a["docs"].size() == 2);
        CHECK(docPathOf(net, 2) == root + "/other/sub/two.txt" && docPathOf(net, 3) == root + "/other/sub/deep/three.txt" &&
              docPathOf(net, 4) == root + "/four.txt");
        guest.client->dispatch();
        std::size_t moves = 0;
        for (const auto& w : guest.writes) moves += w.json["moved"].asBool() ? 1 : 0;
        CHECK(moves == 2);
        // Still clean and following its file at the new place.
        fsWrite(root + "/other/sub/two.txt", "two changed\n");
        net.host.diskChanged({{root + "/other/sub", false}});
        ed.client->dispatch();
        CHECK(net.host.text(2) == u"two changed\n" && bm.text == net.host.text(2));

        // A folder renamed over a document with unsaved changes keeps them.
        bm.caret = bm.length();
        bm.type(u"mine\n");
        ed.client->settle();
        a = fileOpAsk(guest, "rename", "other", "moved");
        CHECK(a["ok"].asBool() && docPathOf(net, 2) == root + "/moved/sub/two.txt" && net.host.dirty(2));

        // A delete: the documents under it are gone as with a file deleted on
        // disk (unsaved, marked gone), the dirty one kept with its text.
        ed.disks.clear();
        a = fileOpAsk(guest, "delete", "moved");
        CHECK(a["ok"].asBool() && a["gone"].size() == 2 && !pathThere(root + "/moved"));
        for (Net::Link* l : {&guest, &ed}) l->client->dispatch();
        CHECK(net.host.hasDocument(2) && net.host.hasDocument(3) && net.host.diskState(3) == 2 && net.host.diskState(2) == 2);
        CHECK(net.host.text(2) == u"two changed\nmine\n" && net.host.dirty(3));
        bool gone2 = false, gone3 = false;
        for (const auto& [d, st] : ed.disks) {
            gone2 = gone2 || (d == 2 && st == 2);
            gone3 = gone3 || (d == 3 && st == 2);
        }
        CHECK(gone2 && gone3);
        // A file open in an app is the app's to follow: the delete leaves the
        // app's document alone (the app's window reads its own file).
        a = fileOpAsk(guest, "delete", "uno.txt");
        CHECK(a["ok"].asBool() && net.host.hasDocument(1) && net.host.text(1) == u"one\nmore\n");
        // An unrelated document is untouched.
        CHECK(docPathOf(net, 4) == root + "/four.txt" && net.host.diskState(4) == 0);
    }
}
