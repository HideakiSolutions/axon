#include "core/shell_filter.hpp"

#include <gtest/gtest.h>
#include <string>

static std::string make_large_diff() {
    std::string diff = "diff --git a/a.cpp b/a.cpp\n"
                       "--- a/a.cpp\n"
                       "+++ b/a.cpp\n"
                       "@@ -1,200 +1,200 @@\n";
    for (int i = 0; i < 220; ++i) {
        diff += "-old line " + std::to_string(i) + " with verbose payload payload payload\n";
        diff += "+new line " + std::to_string(i) + " with verbose payload payload payload\n";
    }
    return diff;
}

static std::string make_large_log() {
    std::string log;
    for (int i = 0; i < 160; ++i) {
        log += "2026-06-30T10:00:" + std::to_string(i % 60) +
               "Z INFO worker processed item " + std::to_string(i) + "\n";
    }
    log += "2026-06-30T10:02:41Z ERROR failed to persist final item\n";
    return log;
}

static std::string make_rich_log_output() {
    std::string log;
    for (int i = 0; i < 70; ++i) {
        log += "Jul 01 10:" + std::to_string(10 + (i % 40)) +
               ":01 host systemd[1234]: Started app.slice worker scope " +
               std::to_string(i) + ".\n";
    }
    for (int i = 0; i < 30; ++i) {
        log += "Jul 01 10:40:" + std::to_string(10 + (i % 30)) +
               " host gpg-agent[900]: can't connect to the keyboxd: IPC connect call failed\n";
    }
    for (int i = 0; i < 18; ++i) {
        log += "2026-07-01T10:45:" + std::to_string(10 + (i % 30)) +
               "Z WARN cache refill delayed for shard " + std::to_string(i) + "\n";
    }
    log += "2026-07-01T10:47:00Z FATAL unable to open artifact store\n";
    log += "Jul 01 10:48:01 host app[500]: consumed 14ms CPU time\n";
    return log;
}

static std::string make_large_grep() {
    std::string out;
    for (int file = 0; file < 8; ++file) {
        for (int line = 0; line < 12; ++line) {
            out += "src/module" + std::to_string(file) + ".cpp:" +
                   std::to_string(10 + line) +
                   ": matched symbol with very long implementation detail payload payload payload payload payload payload payload\n";
        }
    }
    return out;
}

static std::string make_large_json() {
    std::string out =
        "{\n"
        "  \"project\": \"axon\",\n"
        "  \"secret\": \"do-not-repeat-this-value\",\n"
        "  \"packages\": [\n";
    for (int i = 0; i < 80; ++i) {
        out += "    {\"name\":\"pkg-" + std::to_string(i) +
               "\",\"version\":\"1.2." + std::to_string(i) +
               "\",\"dependencies\":{\"duckdb\":\"^1.0.0\",\"tree-sitter\":\"^0.22.0\"}}";
        out += i == 79 ? "\n" : ",\n";
    }
    out +=
        "  ],\n"
        "  \"metadata\": {\"build\":\"debug\",\"platform\":\"linux\",\"retries\":3}\n"
        "}\n";
    return out;
}

static std::string make_large_tsc_output() {
    std::string out = "src/extension.ts: starting compilation\n";
    for (int file = 0; file < 12; ++file) {
        for (int i = 0; i < 9; ++i) {
            int line = 20 + i;
            int col = 5 + (i % 4);
            std::string code = i % 3 == 0 ? "TS2322" : (i % 3 == 1 ? "TS2304" : "TS7006");
            out += "editors/vscode/src/module" + std::to_string(file) + ".ts(" +
                   std::to_string(line) + "," + std::to_string(col) + "): error " +
                   code + ": ";
            if (file == 0 && i == 0) {
                out += "Short mismatch\n";
                out += "  Type 'string' is not assignable to type 'number'.\n";
            } else {
                out += "Type mismatch in generated fixture with a long explanation payload payload payload payload " +
                       std::to_string(i) + "\n";
            }
        }
    }
    out += "Found 108 errors in 12 files.\n";
    return out;
}

static std::string make_large_pytest_output() {
    std::string out = "\n==================================== ERRORS ====================================\n";
    for (int i = 0; i < 30; ++i) {
        out += "________ ERROR collecting tests/unit/test_module_" + std::to_string(i) + ".py ________\n";
        out += "ImportError while importing test module '/repo/tests/unit/test_module_" +
               std::to_string(i) + ".py'.\n";
        out += "Hint: make sure your test modules/packages have valid Python names.\n";
        out += "Traceback:\n";
        out += "/usr/lib/python3.12/importlib/__init__.py:90: in import_module\n";
        out += "tests/unit/test_module_" + std::to_string(i) + ".py:6: in <module>\n";
        out += "    from agent_runtime.domain.missing import MissingThing\n";
        out += "E   ModuleNotFoundError: No module named 'agent_runtime'\n";
    }
    out += "=========================== short test summary info ============================\n";
    for (int i = 0; i < 30; ++i) {
        out += "ERROR tests/unit/test_module_" + std::to_string(i) + ".py\n";
    }
    out += "!!!!!!!!!!!!!!!!!!! Interrupted: 30 errors during collection !!!!!!!!!!!!!!!!!!!\n";
    out += "30 errors in 1.89s\n";
    return out;
}

static std::string make_large_ctest_failure_output() {
    std::string out = "Internal ctest changing into directory: /repo/build\n";
    out += "Test project /repo/build\n";
    for (int i = 0; i < 40; ++i) {
        out += "    Start " + std::to_string(i + 1) + ": test_case_" + std::to_string(i) + "\n";
        if (i % 10 == 0) {
            out += std::to_string(i + 1) + "/40 Test #" + std::to_string(i + 1) +
                   ": test_case_" + std::to_string(i) + " .................***Failed    0.01 sec\n";
            out += "/repo/tests/unit/test_case.cpp:" + std::to_string(40 + i) + ": Failure\n";
            out += "Expected equality of these values:\n";
            out += "  actual_value\n";
            out += "  expected_value\n";
            out += "[  FAILED  ] Fixture.Case" + std::to_string(i) + "\n";
        } else {
            out += std::to_string(i + 1) + "/40 Test #" + std::to_string(i + 1) +
                   ": test_case_" + std::to_string(i) + " ................   Passed    0.01 sec\n";
        }
    }
    out += "90% tests passed, 4 tests failed out of 40\n";
    out += "The following tests FAILED:\n";
    out += "\t 1 - test_case_0 (Failed)\n";
    out += "\t 11 - test_case_10 (Failed)\n";
    out += "\t 21 - test_case_20 (Failed)\n";
    out += "\t 31 - test_case_30 (Failed)\n";
    return out;
}

static std::string make_large_package_output() {
    std::string out;
    for (int i = 0; i < 160; ++i) {
        out += "add package-" + std::to_string(i) + " 1.2." + std::to_string(i) + "\n";
    }
    out += "\nadded 160 packages in 318ms\n";
    out += "\n50 packages are looking for funding\n";
    out += "  run `npm fund` for details\n";
    out += "found 0 vulnerabilities\n";
    return out;
}

static std::string make_package_error_output() {
    std::string out =
        "\n> axon-vscode@1.2.0 typecheck\n"
        "> tsc -p ./ --noEmit\n"
        "\n"
        "npm ERR! code 127\n"
        "npm ERR! path /repo/editors/vscode\n"
        "sh: 1: tsc: not found\n";
    for (int i = 0; i < 60; ++i) {
        out += "npm timing idealTree:node_modules/package-" + std::to_string(i) +
               " Completed in " + std::to_string(i) + "ms\n";
    }
    return out;
}

static std::string make_large_ruff_output() {
    std::string out;
    for (int file = 0; file < 12; ++file) {
        for (int i = 0; i < 8; ++i) {
            std::string code = i % 3 == 0 ? "F401" : (i % 3 == 1 ? "E501" : "B006");
            out += "src/module_" + std::to_string(file) + ".py:" +
                   std::to_string(10 + i) + ":" + std::to_string(5 + i) + ": " +
                   code + " lint message with repeated explanatory payload payload payload " +
                   std::to_string(i) + "\n";
        }
    }
    out += "Found 96 errors.\n";
    return out;
}

static std::string make_eslint_output() {
    std::string out =
        "/repo/src/app.ts\n"
        "  10:5  error    Unexpected console statement  no-console\n"
        "  11:7  warning  'value' is assigned a value but never used  @typescript-eslint/no-unused-vars\n";
    for (int i = 0; i < 40; ++i) {
        out += "  " + std::to_string(20 + i) + ":3  error  Repeated lint failure payload payload payload  no-console\n";
    }
    out += "\n"
           "/repo/src/other.ts\n"
           "  3:1  error  Missing semicolon  semi\n"
           "\n"
           "✖ 43 problems (42 errors, 1 warning)\n";
    return out;
}

TEST(ShellFilter, DiffOutputIsClassifiedAndReduced) {
    auto result = axon::filter_shell_output("diff", make_large_diff(), 300);
    EXPECT_EQ(result.command, "diff");
    EXPECT_EQ(result.kind, axon::OutputKind::Diff);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_LT(result.output_tokens, result.input_tokens);
    EXPECT_NE(result.output.find("diff --git"), std::string::npos);
}

TEST(ShellFilter, LogOutputKeepsImportantLines) {
    auto result = axon::filter_shell_output("log", make_large_log(), 220);
    EXPECT_EQ(result.kind, axon::OutputKind::Log);
    EXPECT_TRUE(result.changed);
    EXPECT_NE(result.output.find("# axon log summary"), std::string::npos);
    EXPECT_NE(result.output.find("ERROR failed"), std::string::npos);
    EXPECT_GT(result.tokens_saved, 0);
}

TEST(ShellFilter, LogOutputDeduplicatesAndCountsLevels) {
    auto result = axon::filter_shell_output("log", make_rich_log_output(), 420);
    EXPECT_EQ(result.command, "log");
    EXPECT_EQ(result.kind, axon::OutputKind::Log);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon log summary"), std::string::npos);
    EXPECT_NE(result.output.find("levels:"), std::string::npos);
    EXPECT_NE(result.output.find("fatal=1"), std::string::npos);
    EXPECT_NE(result.output.find("warn=18"), std::string::npos);
    EXPECT_NE(result.output.find("x [error] can't connect to the keyboxd"), std::string::npos);
    EXPECT_NE(result.output.find("[fatal] 2026-07-01T10:47:00Z FATAL"), std::string::npos);
    EXPECT_NE(result.output.find("first:"), std::string::npos);
    EXPECT_NE(result.output.find("last:"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, LogAliasesNormalize) {
    auto result = axon::filter_shell_output("logs", make_rich_log_output(), 420);
    EXPECT_EQ(result.command, "log");
    EXPECT_TRUE(result.changed);
}

TEST(ShellFilter, LogMalformedOutputPassesThroughSafely) {
    std::string input = "log runner started\n" + std::string(2200, 'z');
    auto result = axon::filter_shell_output("log", input, 120);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, LogOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("logs", make_rich_log_output(), 150);
    EXPECT_EQ(result.command, "log");
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 150);
    EXPECT_NE(result.output.find("# axon log summary"), std::string::npos);
}

TEST(ShellFilter, SmallInputPassesThroughUnchanged) {
    std::string input = "one short line\n";
    auto result = axon::filter_shell_output("auto", input, 100);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
    EXPECT_EQ(result.tokens_saved, 0);
}

TEST(ShellFilter, BinaryLikeInputPassesThroughUnchanged) {
    std::string input = "prefix";
    input.push_back('\0');
    input += std::string(2000, 'x');
    auto result = axon::filter_shell_output("auto", input, 10);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, CommandAliasesNormalize) {
    auto result = axon::filter_shell_output("rg", make_large_log(), 100);
    EXPECT_EQ(result.command, "grep");
}

TEST(ShellFilter, GrepOutputGroupsByFileAndSummarizesOmissions) {
    auto result = axon::filter_shell_output("grep", make_large_grep(), 500);
    EXPECT_EQ(result.command, "grep");
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon grep summary"), std::string::npos);
    EXPECT_NE(result.output.find("## src/module0.cpp"), std::string::npos);
    EXPECT_NE(result.output.find("more matches"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, GrepOutputTruncatesVeryLongLines) {
    std::string input =
        "src/a.cpp:10:" + std::string(500, 'x') + "\n" +
        "src/a.cpp:11:" + std::string(500, 'y') + "\n" +
        "src/a.cpp:12:" + std::string(500, 'z') + "\n" +
        "src/a.cpp:13:" + std::string(500, 'q') + "\n";
    auto result = axon::filter_shell_output("rg", input, 220);
    EXPECT_TRUE(result.changed);
    EXPECT_NE(result.output.find("[truncated]"), std::string::npos);
}

TEST(ShellFilter, GrepMalformedOutputFallsBackSafely) {
    std::string input = "this is not rg output\n" + std::string(2000, 'a');
    auto result = axon::filter_shell_output("grep", input, 100);
    EXPECT_TRUE(result.output == input || result.output_tokens < result.input_tokens);
}

TEST(ShellFilter, GrepOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("grep", make_large_grep(), 180);
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 180);
}

TEST(ShellFilter, JsonOutputSummarizesSchemaAndDropsValues) {
    auto result = axon::filter_shell_output("json", make_large_json(), 260);
    EXPECT_EQ(result.command, "json");
    EXPECT_EQ(result.kind, axon::OutputKind::Json);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon json summary"), std::string::npos);
    EXPECT_NE(result.output.find("packages: array"), std::string::npos);
    EXPECT_NE(result.output.find("dependencies: object"), std::string::npos);
    EXPECT_EQ(result.output.find("do-not-repeat-this-value"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, JsonMalformedOutputPassesThroughSafely) {
    std::string input = "{\"packages\": [\n" + std::string(2000, 'x');
    auto result = axon::filter_shell_output("json", input, 120);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, JsonOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("json", make_large_json(), 80);
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 80);
    EXPECT_NE(result.output.find("# axon json summary"), std::string::npos);
}

TEST(ShellFilter, TscOutputGroupsDiagnosticsByFileAndCode) {
    auto result = axon::filter_shell_output("tsc", make_large_tsc_output(), 500);
    EXPECT_EQ(result.command, "tsc");
    EXPECT_EQ(result.kind, axon::OutputKind::PlainText);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon tsc summary"), std::string::npos);
    EXPECT_NE(result.output.find("TS2322="), std::string::npos);
    EXPECT_NE(result.output.find("## editors/vscode/src/module0.ts"), std::string::npos);
    EXPECT_NE(result.output.find("not assignable"), std::string::npos);
    EXPECT_NE(result.output.find("more diagnostics"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, TscAliasesNormalize) {
    auto result = axon::filter_shell_output("typescript", make_large_tsc_output(), 500);
    EXPECT_EQ(result.command, "tsc");
    EXPECT_TRUE(result.changed);
}

TEST(ShellFilter, TscMalformedOutputPassesThroughSafely) {
    std::string input = "TypeScript compiler started\n" + std::string(2000, 'z');
    auto result = axon::filter_shell_output("tsc", input, 120);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, TscOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("compiler", make_large_tsc_output(), 160);
    EXPECT_EQ(result.command, "tsc");
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 160);
    EXPECT_NE(result.output.find("# axon tsc summary"), std::string::npos);
}

TEST(ShellFilter, TestOutputKeepsPytestFailuresAndSummary) {
    auto result = axon::filter_shell_output("pytest", make_large_pytest_output(), 700);
    EXPECT_EQ(result.command, "test");
    EXPECT_EQ(result.kind, axon::OutputKind::PlainText);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon test summary"), std::string::npos);
    EXPECT_NE(result.output.find("ERROR collecting tests/unit/test_module_0.py"), std::string::npos);
    EXPECT_NE(result.output.find("ModuleNotFoundError"), std::string::npos);
    EXPECT_NE(result.output.find("short test summary info"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, TestOutputKeepsCTestFailures) {
    auto result = axon::filter_shell_output("ctest", make_large_ctest_failure_output(), 500);
    EXPECT_EQ(result.command, "test");
    EXPECT_TRUE(result.changed);
    EXPECT_NE(result.output.find("[  FAILED  ] Fixture.Case0"), std::string::npos);
    EXPECT_NE(result.output.find("test_case.cpp"), std::string::npos);
    EXPECT_NE(result.output.find("tests failed out of"), std::string::npos);
}

TEST(ShellFilter, TestAliasesNormalize) {
    auto result = axon::filter_shell_output("gtest", make_large_ctest_failure_output(), 500);
    EXPECT_EQ(result.command, "test");
    EXPECT_TRUE(result.changed);
}

TEST(ShellFilter, TestMalformedOutputPassesThroughSafely) {
    std::string input = "running test harness\n" + std::string(2200, 'q');
    auto result = axon::filter_shell_output("test", input, 150);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, TestOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("vitest", make_large_pytest_output(), 180);
    EXPECT_EQ(result.command, "test");
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 180);
    EXPECT_NE(result.output.find("# axon test summary"), std::string::npos);
}

TEST(ShellFilter, PackageOutputSummarizesInstallOperations) {
    auto result = axon::filter_shell_output("npm", make_large_package_output(), 350);
    EXPECT_EQ(result.command, "package");
    EXPECT_EQ(result.kind, axon::OutputKind::PlainText);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon package summary"), std::string::npos);
    EXPECT_NE(result.output.find("counts: add=160"), std::string::npos);
    EXPECT_NE(result.output.find("added 160 packages"), std::string::npos);
    EXPECT_NE(result.output.find("found 0 vulnerabilities"), std::string::npos);
    EXPECT_NE(result.output.find("more package operation lines"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, PackageOutputKeepsScriptErrors) {
    auto result = axon::filter_shell_output("package", make_package_error_output(), 50);
    EXPECT_EQ(result.command, "package");
    EXPECT_TRUE(result.changed);
    EXPECT_NE(result.output.find("typecheck"), std::string::npos);
    EXPECT_NE(result.output.find("npm ERR! code 127"), std::string::npos);
    EXPECT_NE(result.output.find("tsc: not found"), std::string::npos);
}

TEST(ShellFilter, PackageAliasesNormalize) {
    auto result = axon::filter_shell_output("pnpm", make_large_package_output(), 350);
    EXPECT_EQ(result.command, "package");
    EXPECT_TRUE(result.changed);
    auto bun = axon::filter_shell_output("bun", make_large_package_output(), 350);
    EXPECT_EQ(bun.command, "package");
    EXPECT_TRUE(bun.changed);
}

TEST(ShellFilter, PackageMalformedOutputPassesThroughSafely) {
    std::string input = "package manager started\n" + std::string(2000, 'm');
    auto result = axon::filter_shell_output("npm", input, 120);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, PackageOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("yarn", make_large_package_output(), 120);
    EXPECT_EQ(result.command, "package");
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 120);
    EXPECT_NE(result.output.find("# axon package summary"), std::string::npos);
}

TEST(ShellFilter, LintOutputGroupsRuffDiagnostics) {
    auto result = axon::filter_shell_output("ruff", make_large_ruff_output(), 420);
    EXPECT_EQ(result.command, "lint");
    EXPECT_EQ(result.kind, axon::OutputKind::PlainText);
    EXPECT_TRUE(result.changed);
    EXPECT_GT(result.tokens_saved, 0);
    EXPECT_NE(result.output.find("# axon lint summary"), std::string::npos);
    EXPECT_NE(result.output.find("F401="), std::string::npos);
    EXPECT_NE(result.output.find("## src/module_0.py"), std::string::npos);
    EXPECT_NE(result.output.find("Found 96 errors"), std::string::npos);
    EXPECT_LT(result.output_tokens, result.input_tokens);
}

TEST(ShellFilter, LintOutputParsesEslintStylish) {
    auto result = axon::filter_shell_output("eslint", make_eslint_output(), 160);
    EXPECT_EQ(result.command, "lint");
    EXPECT_TRUE(result.changed);
    EXPECT_NE(result.output.find("no-console"), std::string::npos);
    EXPECT_NE(result.output.find("@typescript-eslint/no-unused-vars"), std::string::npos);
    EXPECT_NE(result.output.find("2 errors, 1 warning"), std::string::npos);
}

TEST(ShellFilter, LintAliasesNormalize) {
    auto result = axon::filter_shell_output("prettier", make_large_ruff_output(), 420);
    EXPECT_EQ(result.command, "lint");
    EXPECT_TRUE(result.changed);
}

TEST(ShellFilter, LintMalformedOutputPassesThroughSafely) {
    std::string input = "lint runner started\n" + std::string(1800, 'l');
    auto result = axon::filter_shell_output("lint", input, 100);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.output, input);
}

TEST(ShellFilter, LintOutputRespectsTightBudget) {
    auto result = axon::filter_shell_output("format", make_large_ruff_output(), 150);
    EXPECT_EQ(result.command, "lint");
    EXPECT_TRUE(result.changed);
    EXPECT_LE(result.output_tokens, 150);
    EXPECT_NE(result.output.find("# axon lint summary"), std::string::npos);
}
