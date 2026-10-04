// sp-scene: scene utilities for the editor's development server and tests.
//
//   sp-scene analyze   [--duration s] [--dt s] [--base dir] < scene.json   -> analysis JSON (what the editor draws)
//   sp-scene normalize [--base dir] < scene.json                           -> the scene as the engine reads it
//
// --base resolves relative files inside the scene (a mesh room's OBJ).
//
// Errors are printed as {"error": "..."} with exit code 1.
#include <iostream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "sp/SceneAnalysis.h"
#include "sp/SceneJson.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sp-scene analyze|normalize [--duration s] [--dt s] < scene.json\n";
        return 2;
    }
    const std::string cmd = argv[1];
    double duration = 0, dt = 0;
    std::string base;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--duration") duration = std::stod(argv[i + 1]);
        else if (a == "--dt") dt = std::stod(argv[i + 1]);
        else if (a == "--base") base = argv[i + 1];
    }
    const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    try {
        const sp::Scene scene = sp::sceneFromJson(input, base);
        if (cmd == "analyze") std::cout << sp::analysisToJson(sp::analyzeScene(scene, duration, dt)) << "\n";
        else if (cmd == "normalize") std::cout << sp::sceneToJson(scene) << "\n";
        else { std::cerr << "unknown command " << cmd << "\n"; return 2; }
        return 0;
    } catch (const std::exception& e) {
        std::cout << nlohmann::json{{"error", e.what()}}.dump() << "\n";
        return 1;
    }
}
