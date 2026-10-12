// tools/phxstudio/new_main.cpp — `phxnew DIR [NAME]`: Phosphorus Studio's File > New project on the
// command line. Writes the same template (projectdoc.h: create_project): src/main.cpp, a hero
// sprite, a tileset, a level and phxproject.json, ready for `make play PROJECT=DIR`. Headless;
// `make project-check` and CI use it to build the template for every target.
#include "projectdoc.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]).rfind("--project-", 0) == 0) {
        phxstudio::ProjectDoc p;
        std::string err;
        if (!phxstudio::ProjectDoc::load(argv[2], p, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        const std::string op = argv[1];
        auto shell_quote = [](const std::string& s) {
            std::string q = "'";
            for (char c : s) q += c == '\'' ? "'\\''" : std::string(1, c);
            return q + "'";
        };
        if (op == "--project-json") std::printf("%s", p.to_json().c_str());
        else if (op == "--project-id") std::printf("%s\n", p.id.c_str());
        else if (op == "--project-title") std::printf("%s\n", p.name.substr(0, 64).c_str());
        else if (op == "--project-check") {
            if (p.build != "generic") {
                std::fprintf(stderr, "custom-build project: use its configured launches instead of generic game commands\n"); return 1;
            }
        } else if (op == "--project-sources") {
            for (const auto& f : p.source_files()) std::printf("%s ", shell_quote(f).c_str());
        } else if (op == "--project-includes") {
            for (const auto& r : p.source) std::printf("-I%s ", shell_quote(p.dir + "/" + r).c_str());
        } else return 2;
        return 0;
    }
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: phxnew DIR [NAME]   (NAME defaults to the folder's name)\n");
        return 2;
    }
    std::string err;
    if (!phxstudio::create_project(argv[1], argc > 2 ? argv[2] : "", &err)) {
        std::fprintf(stderr, "phxnew: %s\n", err.c_str());
        return 1;
    }
    std::printf("phxnew: created %s\n", argv[1]);
    return 0;
}
