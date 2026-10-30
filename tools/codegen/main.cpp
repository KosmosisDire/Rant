// rant_codegen --standard [--root DIR] [--check]
// Writes every language's copy of the standard types from types/stdtypes.schema. --check writes
// nothing and fails when a copy is stale, which ctest runs as codegen.current.
#include "backends.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace codegen;

// A checkout may hold CRLF, so a comparison and the schema text go by LF alone.
static std::string lf(std::string s) {
    size_t out = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] != '\r') s[out++] = s[i];
    s.resize(out);
    return s;
}

static bool read_file(const std::string &path, std::string &text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    text = lf(all.str());
    return true;
}

static std::string must_read(const std::string &path) {
    std::string text;
    if (!read_file(path, text)) throw std::runtime_error("cannot read " + path);
    return text;
}

// The copies spell each definition anew, so they must read back as the very same model.
static void check_round_trip(const Model &model) {
    std::string text;
    for (const Def &d : model.defs) text += definition_of(d);
    const Model back = load_model(text, true);
    if (back.defs.size() != model.defs.size()) throw std::runtime_error("the spelled roster lost a type");
    for (size_t i = 0; i < back.defs.size(); i++) {
        const Def &a = model.defs[i], &b = back.defs[i];
        bool same = a.name == b.name && a.hash == b.hash && a.about == b.about && a.members.size() == b.members.size();
        for (size_t m = 0; same && m < a.members.size(); m++)
            same = a.members[m].unit == b.members[m].unit && a.members[m].note == b.members[m].note;
        if (!same) throw std::runtime_error(a.name + " does not read back as written");
    }
}

static int run(const std::string &root, bool check) {
    const std::string schema = root + "/types/stdtypes.schema";
    Model model;
    try {
        model = load_model(must_read(schema), true);
    } catch (const std::exception &e) {
        throw std::runtime_error(schema + ":" + e.what());
    }
    check_round_trip(model);

    std::vector<Output> outputs = write_c(model, must_read(root + "/types/extras.h"));
    for (Output &o : write_cpp(model, must_read(root + "/types/extras.hpp"))) outputs.push_back(o);
    for (Output &o : write_csharp(model, must_read(root + "/types/extras.cs"))) outputs.push_back(o);
    for (Output &o : write_python(model, must_read(root + "/types/extras.py"), must_read(root + "/types/extras.pyi")))
        outputs.push_back(o);
    for (Output &o : write_json(model)) outputs.push_back(o);

    int stale = 0;
    for (const Output &o : outputs) {
        const std::string path = root + "/" + o.path;
        std::string now;
        if (read_file(path, now) && now == o.text) continue;
        if (check) {
            fprintf(stderr, "rant_codegen: %s is stale, run rant_codegen --standard\n", o.path.c_str());
            stale++;
            continue;
        }
        std::ofstream out(path, std::ios::binary);
        out << o.text;
        if (!out) throw std::runtime_error("cannot write " + path);
        printf("rant_codegen: wrote %s\n", o.path.c_str());
    }
    return stale ? 1 : 0;
}

static int usage() {
    fprintf(stderr, "usage: rant_codegen --standard [--root DIR] [--check]\n");
    return 2;
}

int main(int argc, char **argv) {
    std::string root = ".";
    bool standard = false, check = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--standard")) standard = true;
        else if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else return usage();
    }
    if (!standard) return usage();
    try {
        return run(root, check);
    } catch (const std::exception &e) {
        fprintf(stderr, "rant_codegen: %s\n", e.what());
        return 1;
    }
}
