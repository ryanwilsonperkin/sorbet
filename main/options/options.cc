// have to go first as they violate our poisons
#include "rang.hpp"
#include "yaml-cpp/yaml.h"
#include <cxxopts.hpp>

#include "FileFlatMapper.h"
#include "core/Error.h"
#include "core/errors/infer.h"
#include "main/options/FileFlatMapper.h"
#include "main/options/options.h"
#include "options.h"
#include "version/version.h"

namespace spd = spdlog;
using namespace std;

namespace sorbet::realmain::options {
struct PrintOptions {
    string option;
    bool Printers::*flag;
    bool supportsCaching = false;
};

const vector<PrintOptions> print_options({
    {"parse-tree", &Printers::ParseTree},
    {"parse-tree-json", &Printers::ParseTreeJSON},
    {"ast", &Printers::Desugared},
    {"ast-raw", &Printers::DesugaredRaw},
    {"dsl-tree", &Printers::DSLTree, true},
    {"dsl-tree-raw", &Printers::DSLTreeRaw, true},
    {"symbol-table", &Printers::SymbolTable, true},
    {"symbol-table-raw", &Printers::SymbolTableRaw, true},
    {"symbol-table-json", &Printers::SymbolTableJson, true},
    {"symbol-table-full", &Printers::SymbolTableFullRaw, true},
    {"symbol-table-full-raw", &Printers::SymbolTableFull, true},
    {"name-tree", &Printers::NameTree, true},
    {"name-tree-raw", &Printers::NameTreeRaw, true},
    {"file-table-json", &Printers::FileTableJson, true},
    {"resolve-tree", &Printers::ResolveTree, true},
    {"resolve-tree-raw", &Printers::ResolveTreeRaw, true},
    {"cfg", &Printers::CFG, true},
    {"autogen", &Printers::Autogen, true},
    {"autogen-msgpack", &Printers::AutogenMsgPack, true},
});

struct StopAfterOptions {
    string option;
    Phase flag;
};

const vector<StopAfterOptions> stop_after_options({
    {"init", Phase::INIT},
    {"parser", Phase::PARSER},
    {"desugarer", Phase::DESUGARER},
    {"dsl", Phase::DSL},
    {"namer", Phase::NAMER},
    {"resolver", Phase::RESOLVER},
    {"cfg", Phase::CFG},
    {"inferencer", Phase::INFERENCER},
});

core::StrictLevel text2StrictLevel(string_view key, shared_ptr<spdlog::logger> logger) {
    if (key == "ignore") {
        return core::StrictLevel::Ignore;
    } else if (key == "false") {
        return core::StrictLevel::Stripe;
    } else if (key == "true") {
        return core::StrictLevel::Typed;
    } else if (key == "strict") {
        return core::StrictLevel::Strict;
    } else if (key == "strong") {
        return core::StrictLevel::Strong;
    } else if (key == "autogenerated") {
        return core::StrictLevel::Autogenerated;
    } else {
        logger->error("Unknown strictness level: `{}`", key);
        throw EarlyReturnWithCode(1);
    }
}

UnorderedMap<string, core::StrictLevel> extractStricnessOverrides(string fileName, shared_ptr<spdlog::logger> logger) {
    UnorderedMap<string, core::StrictLevel> result;
    YAML::Node config = YAML::LoadFile(fileName);
    switch (config.Type()) {
        case YAML::NodeType::Map:
            for (const auto &child : config) {
                auto key = child.first.as<string>();
                core::StrictLevel level = text2StrictLevel(key, logger);
                switch (child.second.Type()) {
                    case YAML::NodeType::Sequence:
                        for (const auto &file : child.second) {
                            if (file.IsScalar()) {
                                result[file.as<string>()] = level;
                            } else {
                                logger->error("Cannot parse strictness override format. Invalid file name.");
                                throw EarlyReturnWithCode(1);
                            }
                        }
                        break;
                    default:
                        logger->error(
                            "Cannot parse strictness override format. File names should be specified as a sequence.");
                        throw EarlyReturnWithCode(1);
                }
            }
            break;
        default:
            logger->error("Cannot parse strictness override format. Map is expected on top level.");
            throw EarlyReturnWithCode(1);
    }
    return result;
}

cxxopts::Options buildOptions() {
    cxxopts::Options options("sorbet", "Typechecker for Ruby");

    // Common user options in order of use
    options.add_options()("e", "Parse an inline ruby string", cxxopts::value<string>()->default_value(""), "string");
    options.add_options()("files", "Input files", cxxopts::value<vector<string>>());
    options.add_options()("q,quiet", "Silence all non-critical errors");
    options.add_options()("P,progress", "Draw progressbar");
    options.add_options()("v,verbose", "Verbosity level [0-3]");
    options.add_options()("h,help", "Show long help");
    options.add_options()("version", "Show version");
    options.add_options()("color", "Use color output", cxxopts::value<string>()->default_value("auto"),
                          "{always,never,[auto]}");

    fmt::memory_buffer all_prints;
    fmt::memory_buffer all_stop_after;

    fmt::format_to(all_prints, "Print: [{}]",
                   fmt::map_join(print_options, ", ", [](const auto &pr) -> auto { return pr.option; }));
    fmt::format_to(all_stop_after, "Stop After: [{}]",
                   fmt::map_join(stop_after_options, ", ", [](const auto &pr) -> auto { return pr.option; }));

    // Advanced options
    options.add_options("advanced")("configatron-dir", "Path to configatron yaml folders",
                                    cxxopts::value<vector<string>>(), "path");
    options.add_options("advanced")("configatron-file", "Path to configatron yaml files",
                                    cxxopts::value<vector<string>>(), "path");
    options.add_options("advanced")("debug-log-file", "Path to debug log file",
                                    cxxopts::value<string>()->default_value(""), "file");
    options.add_options("advanced")("reserve-mem-kb",
                                    "Preallocate the specified amount of memory for symbol+name tables",
                                    cxxopts::value<u8>()->default_value("0"));
    options.add_options("advanced")("stdout-hup-hack", "Monitor STDERR for HUP and exit on hangup");
    options.add_options("advanced")("a,autocorrect", "Auto-correct source files with suggested fixes");
    options.add_options("advanced")(
        "suggest-runtime-profiled",
        "When suggesting signatures in `typed: strict` mode, suggest `::T::Utils::RuntimeProfiled`");
    options.add_options("advanced")("lsp", "Start in language-server-protocol mode");
    options.add_options("advanced")("no-error-count", "Do not print the error count summary line");
    options.add_options("advanced")("autogen-version", "Autogen version to output", cxxopts::value<int>());
    // Developer options
    options.add_options("dev")("p,print", to_string(all_prints), cxxopts::value<vector<string>>(), "type");
    options.add_options("dev")("stop-after", to_string(all_stop_after),
                               cxxopts::value<string>()->default_value("inferencer"), "phase");
    options.add_options("dev")("no-stdlib", "Do not load included rbi files for stdlib");
    options.add_options("dev")("skip-dsl-passes", "Do not run DSL passess");
    options.add_options("dev")("wait-for-dbg", "Wait for debugger on start");
    options.add_options("dev")("stress-incremental-resolver",
                               "Force incremental updates to discover resolver & namer bugs");
    options.add_options("dev")("simulate-crash", "Crash on start");
    options.add_options("dev")("silence-dev-message", "Silence \"You are running a development build\" message");
    options.add_options("dev")("error-white-list", "Whitelist of errors to be reported", cxxopts::value<vector<int>>(),
                               "errorCodes");
    options.add_options("dev")("error-black-list", "Blacklist of errors to be reported", cxxopts::value<vector<int>>(),
                               "errorCodes");
    options.add_options("dev")("typed", "Force all code to specified strictness level",
                               cxxopts::value<string>()->default_value("auto"), "{ruby,true,strict,strong,[auto]}");
    options.add_options("dev")("typed-override", "Yaml config that overrides strictness levels on files",
                               cxxopts::value<string>()->default_value(""), "filepath.yaml");
    options.add_options("dev")("store-state", "Store state into file", cxxopts::value<string>()->default_value(""),
                               "file");
    options.add_options("dev")("cache-dir", "Use the specified folder to cache data",
                               cxxopts::value<string>()->default_value(""), "dir");
    options.add_options("dev")("suppress-non-critical", "Exit 0 unless there was a critical error");

    int defaultThreads = thread::hardware_concurrency();
    if (defaultThreads == 0) {
        defaultThreads = 2;
    }

    options.add_options("dev")("max-threads", "Set number of threads",
                               cxxopts::value<int>()->default_value(to_string(defaultThreads)), "int");
    options.add_options("dev")("counter", "Print internal counter", cxxopts::value<vector<string>>(), "counter");
    options.add_options("dev")("statsd-host", "StatsD sever hostname", cxxopts::value<string>()->default_value(""),
                               "host");
    options.add_options("dev")("counters", "Print all internal counters");
    if (sorbet::debug_mode) {
        options.add_options("dev")("suggest-sig", "Report typing candidates. Only supported in debug builds");
    }
    options.add_options("dev")("suggest-typed", "Suggest which typed: sigils to add or upgrade");
    options.add_options("dev")("statsd-prefix", "StatsD prefix",
                               cxxopts::value<string>()->default_value("ruby_typer.unknown"), "prefix");
    options.add_options("dev")("statsd-port", "StatsD sever port", cxxopts::value<int>()->default_value("8200"),
                               "port");
    options.add_options("dev")("metrics-file", "File to export metrics to", cxxopts::value<string>()->default_value(""),
                               "file");
    options.add_options("dev")("metrics-prefix", "Prefix to use in metrics",
                               cxxopts::value<string>()->default_value("ruby_typer.unknown."), "file");
    options.add_options("dev")("metrics-branch", "Branch to report in metrics export",
                               cxxopts::value<string>()->default_value("none"), "branch");
    options.add_options("dev")("metrics-sha", "Sha1 to report in metrics export",
                               cxxopts::value<string>()->default_value("none"), "sha1");
    options.add_options("dev")("metrics-repo", "Repo to report in metrics export",
                               cxxopts::value<string>()->default_value("none"), "repo");

    // Positional params
    options.parse_positional("files");
    options.positional_help("<file1.rb> <file2.rb> ...");
    return options;
}

bool extractPrinters(cxxopts::ParseResult &raw, Options &opts, shared_ptr<spdlog::logger> logger) {
    if (raw.count("print") == 0) {
        return true;
    }
    vector<string> printOpts = raw["print"].as<vector<string>>();
    for (auto opt : printOpts) {
        bool found = false;
        for (auto &known : print_options) {
            if (known.option == opt) {
                opts.print.*(known.flag) = true;
                if (!known.supportsCaching) {
                    if (!opts.cacheDir.empty()) {
                        logger->error("--print={} is incompatible with --cacheDir. Ignoring cache", opt);
                        opts.cacheDir = "";
                    }
                }
                found = true;
                break;
            }
        }
        if (!found) {
            vector<string_view> allOptions;
            for (auto &known : print_options) {
                allOptions.emplace_back(known.option);
            }
            logger->error("Unknown --print option: {}\nValid values: {}", opt, fmt::join(allOptions, ", "));
            return false;
        }
    }
    return true;
}

Phase extractStopAfter(cxxopts::ParseResult &raw, shared_ptr<spdlog::logger> logger) {
    string opt = raw["stop-after"].as<string>();
    for (auto &known : stop_after_options) {
        if (known.option == opt) {
            return known.flag;
        }
    }
    vector<string_view> allOptions;
    for (auto &known : stop_after_options) {
        allOptions.emplace_back(known.option);
    }

    logger->error("Unknown --stop-after option: {}\nValid values: {}", opt, fmt::join(allOptions, ", "));
    return Phase::INIT;
}

void readOptions(Options &opts, int argc, char *argv[],
                 shared_ptr<spdlog::logger> logger) noexcept(false) { // throw(EarlyReturnWithCode)
    FileFlatMapper flatMapper(argc, argv, logger);

    cxxopts::Options options = buildOptions();
    try {
        cxxopts::ParseResult raw = options.parse(argc, argv);
        if (raw["simulate-crash"].as<bool>()) {
            Exception::raise("simulated crash");
        }

        if (raw.count("files") > 0) {
            opts.inputFileNames = raw["files"].as<vector<string>>();
        }

        opts.cacheDir = raw["cache-dir"].as<string>();
        if (!extractPrinters(raw, opts, logger)) {
            throw EarlyReturnWithCode(1);
        }
        opts.stopAfterPhase = extractStopAfter(raw, logger);

        opts.autocorrect = raw["autocorrect"].as<bool>();
        opts.skipDSLPasses = raw["skip-dsl-passes"].as<bool>();

        opts.runLSP = raw["lsp"].as<bool>();
        if (opts.runLSP && !opts.cacheDir.empty()) {
            logger->info("lsp mode does not yet support caching.");
            throw EarlyReturnWithCode(1);
        }
        if ((opts.print.Autogen || opts.print.AutogenMsgPack) &&
            (opts.stopAfterPhase != Phase::NAMER || !opts.skipDSLPasses)) {
            logger->info("-p autogen{} requires --stop-after=namer --skip-dsl-passes",
                         opts.print.AutogenMsgPack ? "-msgpack" : "");
            throw EarlyReturnWithCode(1);
        }

        if (opts.skipDSLPasses && !opts.cacheDir.empty()) {
            logger->info("--skip-dsl-passes does not support caching");
            throw EarlyReturnWithCode(1);
        }

        opts.noErrorCount = raw["no-error-count"].as<bool>();
        opts.noStdlib = raw["no-stdlib"].as<bool>();
        opts.stdoutHUPHack = raw["stdout-hup-hack"].as<bool>();

        opts.threads = opts.runLSP ? raw["max-threads"].as<int>()
                                   : min(raw["max-threads"].as<int>(), int(opts.inputFileNames.size() / 2));

        if (raw["h"].as<bool>()) {
            logger->info("{}", options.help({"", "advanced", "dev"}));
            throw EarlyReturnWithCode(0);
        }
        if (raw["version"].as<bool>()) {
            fmt::print("Ruby Typer {}\n", Version::full_version_string);
            throw EarlyReturnWithCode(0);
        }

        string typed = raw["typed"].as<string>();
        opts.logLevel = raw.count("v");
        if (typed != "auto") {
            opts.forceMinStrict = opts.forceMaxStrict = text2StrictLevel(typed, logger);
        }

        opts.showProgress = raw["P"].as<bool>();
        if (raw.count("configatron-dir") > 0) {
            opts.configatronDirs = raw["configatron-dir"].as<vector<string>>();
        }
        if (raw.count("configatron-file")) {
            opts.configatronFiles = raw["configatron-file"].as<vector<string>>();
        }
        opts.storeState = raw["store-state"].as<string>();
        opts.suggestTyped = raw["suggest-typed"].as<bool>();
        opts.waitForDebugger = raw["wait-for-dbg"].as<bool>();
        opts.stressIncrementalResolver = raw["stress-incremental-resolver"].as<bool>();
        opts.silenceErrors = raw["q"].as<bool>();
        opts.suggestRuntimeProfiledType = raw["suggest-runtime-profiled"].as<bool>();
        opts.enableCounters = raw["counters"].as<bool>();
        opts.silenceDevMessage = raw["silence-dev-message"].as<bool>();
        opts.statsdHost = raw["statsd-host"].as<string>();
        opts.statsdPort = raw["statsd-port"].as<int>();
        opts.statsdPrefix = raw["statsd-prefix"].as<string>();
        opts.metricsSha = raw["metrics-sha"].as<string>();
        opts.metricsFile = raw["metrics-file"].as<string>();
        opts.metricsRepo = raw["metrics-repo"].as<string>();
        opts.metricsBranch = raw["metrics-branch"].as<string>();
        opts.metricsPrefix = raw["metrics-prefix"].as<string>();
        opts.debugLogFile = raw["debug-log-file"].as<string>();
        opts.reserveMemKiB = raw["reserve-mem-kb"].as<u8>();
        if (raw.count("autogen-version") > 0) {
            if (!opts.print.AutogenMsgPack) {
                logger->info("--autogen-version requires -p autogen-msgpack");
                throw EarlyReturnWithCode(1);
            }
            opts.autogenVersion = raw["autogen-version"].as<int>();
        }
        if (raw.count("error-white-list") > 0) {
            opts.errorCodeWhiteList = raw["error-white-list"].as<vector<int>>();
        }
        if (raw.count("error-black-list") > 0) {
            if (raw.count("error-white-list") > 0) {
                logger->info("You can't pass both --error-black-list and --error-white-list");
                throw EarlyReturnWithCode(1);
            }
            opts.errorCodeBlackList = raw["error-black-list"].as<vector<int>>();
        }
        if (sorbet::debug_mode) {
            opts.suggestSig = raw["suggest-sig"].as<bool>();
        }

        if (raw.count("e") == 0 && opts.inputFileNames.empty() && !opts.runLSP && opts.storeState.empty()) {
            logger->info("You must pass either `-e` or at least one ruby file.\n\n{}", options.help({""}));
            throw EarlyReturnWithCode(1);
        }

        if ((raw["color"].as<string>() == "never") || opts.runLSP) {
            core::ErrorColors::disableColors();
        } else if (raw["color"].as<string>() == "auto") {
            if (rang::rang_implementation::isTerminal(cerr.rdbuf())) {
                core::ErrorColors::enableColors();
            }
        } else if (raw["color"].as<string>() == "always") {
            core::ErrorColors::enableColors();
        }

        if (opts.suggestTyped) {
            if (!opts.errorCodeWhiteList.empty()) {
                logger->error("--suggest-typed can't use --error-white-list");
                throw EarlyReturnWithCode(1);
            }
            if (!opts.errorCodeBlackList.empty()) {
                logger->error("--suggest-typed can't use --error-black-list");
                throw EarlyReturnWithCode(1);
            }
            if (opts.forceMinStrict != core::StrictLevel::Ignore) {
                logger->error("--suggest-typed can't use --typed");
                throw EarlyReturnWithCode(1);
            }
            opts.errorCodeWhiteList.emplace_back(core::errors::Infer::SuggestTyped.code);
            opts.forceMinStrict = opts.forceMaxStrict = core::StrictLevel::Strong;
        }

        opts.inlineInput = raw["e"].as<string>();
        opts.supressNonCriticalErrors = raw["suppress-non-critical"].as<bool>();
        if (!raw["typed-override"].as<string>().empty()) {
            opts.strictnessOverrides = extractStricnessOverrides(raw["typed-override"].as<string>(), logger);
        }
    } catch (cxxopts::OptionParseException &e) {
        logger->info("{}\n\n{}", e.what(), options.help({"", "advanced", "dev"}));
        throw EarlyReturnWithCode(1);
    }
}

EarlyReturnWithCode::EarlyReturnWithCode(int returnCode)
    : SorbetException("early return with code " + to_string(returnCode)), returnCode(returnCode){};

} // namespace sorbet::realmain::options
