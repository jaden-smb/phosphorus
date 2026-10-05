// tools/phxstudio/new_main.cpp — `phxnew DIR [NAME]`: Phosphorus Studio's File > New project on the
// command line. Writes the same template (projectdoc.h: create_project): src/main.cpp, a hero
// sprite, a tileset, a level and phxproject.json, ready for `make play PROJECT=DIR`. Headless;
// `make project-check` and CI use it to build the template for every target.
#include "projectdoc.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
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
