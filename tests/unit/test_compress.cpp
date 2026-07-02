// Unit tests for compress.hpp / compress.cpp (Balde A).
// Covers: small input passthrough, large input compression invariants,
// invalid UTF-8 safety, and compression_from_string mapping.

#include "core/compress.hpp"

#include <gtest/gtest.h>
#include <string>
#include <optional>

// Local mirror of capsule.hpp's inline estimate_tokens — avoids pulling in
// DuckDB / llama.cpp headers in a pure compress unit test.
static int est_tokens(const std::string& s) {
    return (int)((s.size() + 3) / 4);
}

// ── compression_from_string ───────────────────────────────────────────────────

TEST(CompressionFromString, KnownValues) {
    EXPECT_EQ(axon::compression_from_string("off"),  axon::CapsuleCompression::Off);
    EXPECT_EQ(axon::compression_from_string("body"), axon::CapsuleCompression::Body);
}

TEST(CompressionFromString, UnknownDefaultsOff) {
    EXPECT_EQ(axon::compression_from_string(""),        axon::CapsuleCompression::Off);
    EXPECT_EQ(axon::compression_from_string("BODY"),    axon::CapsuleCompression::Off);
    EXPECT_EQ(axon::compression_from_string("unknown"), axon::CapsuleCompression::Off);
}

// ── classify_output: type-aware routing before lossy compression ─────────────

TEST(ClassifyOutput, LanguageHintMeansSourceCode) {
    std::string text = "plain words without obvious syntax\n";
    EXPECT_EQ(axon::classify_output(text, axon::Language::Python),
              axon::OutputKind::SourceCode);
}

TEST(ClassifyOutput, DetectsCoreTextShapes) {
    EXPECT_EQ(axon::classify_output("{\"ok\":true}\n"), axon::OutputKind::Json);

    std::string diff =
        "diff --git a/a.cpp b/a.cpp\n"
        "--- a/a.cpp\n"
        "+++ b/a.cpp\n"
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n";
    EXPECT_EQ(axon::classify_output(diff), axon::OutputKind::Diff);

    std::string log =
        "2026-06-30T10:00:00Z INFO boot\n"
        "2026-06-30T10:00:01Z WARN slow path\n";
    EXPECT_EQ(axon::classify_output(log), axon::OutputKind::Log);

    std::string md =
        "# Title\n"
        "## Section\n"
        "body\n";
    EXPECT_EQ(axon::classify_output(md), axon::OutputKind::Markdown);
}

TEST(ClassifyOutput, DetectsBinaryUnsafeStreams) {
    std::string blob = "abc";
    blob.push_back('\0');
    blob += "def";
    EXPECT_EQ(axon::classify_output(blob), axon::OutputKind::Binary);
    EXPECT_EQ(axon::output_kind_to_string(axon::OutputKind::Binary), "binary");
}

// ── compress_body: small input passthrough ────────────────────────────────────

TEST(CompressBody, SmallInputReturnedUnchanged) {
    std::string src = "int foo() { return 42; }\n";
    // Budget well above the token count of `src`
    std::string result = axon::compress_body(src, std::nullopt, 10000);
    EXPECT_EQ(result, src) << "Small input must be byte-identical to source";
}

TEST(CompressBody, ExactlyAtBudgetReturnedUnchanged) {
    // 4 chars = 1 token; craft a string that sits exactly at the limit.
    std::string src(400, 'x');  // 400 chars → 100 tokens
    std::string result = axon::compress_body(src, std::nullopt, 100);
    EXPECT_EQ(result, src) << "At-budget input must be byte-identical to source";
}

// ── compress_body: large input compression ────────────────────────────────────

static std::string make_large_cpp_function() {
    // Simulate a ~200-line function body: declaration, lots of padding, return.
    std::string src;
    src += "int compute_answer(int n) {\n";
    src += "    // header comment\n";
    for (int i = 0; i < 180; ++i) {
        src += "    int var" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    src += "    return n * 2;\n";
    src += "}\n";
    return src;
}

TEST(CompressBody, LargeInputFitsInBudget) {
    std::string src = make_large_cpp_function();
    int full_tokens = est_tokens(src);
    ASSERT_GT(full_tokens, 50) << "Fixture must be large enough to exercise compression";

    int budget = 50;
    std::string result = axon::compress_body(src, std::nullopt, budget);
    int result_tokens = est_tokens(result);

    EXPECT_LE(result_tokens, budget + 4)  // small overshoot tolerance for marker line
        << "Compressed output should fit within budget (got " << result_tokens
        << " tokens, budget " << budget << ")";
    EXPECT_LT(result_tokens, full_tokens)
        << "Compressed output must be smaller than original";
}

TEST(CompressBody, DeclarationLinePreserved) {
    std::string src = make_large_cpp_function();
    std::string result = axon::compress_body(src, std::nullopt, 50);
    EXPECT_NE(result.find("int compute_answer(int n)"), std::string::npos)
        << "Declaration line must survive compression";
}

TEST(CompressBody, ReturnLinePreserved) {
    std::string src = make_large_cpp_function();
    std::string result = axon::compress_body(src, std::nullopt, 50);
    EXPECT_NE(result.find("return n * 2;"), std::string::npos)
        << "return statement must survive as a significant line";
}

TEST(CompressBody, ElisionMarkerPresent) {
    std::string src = make_large_cpp_function();
    std::string result = axon::compress_body(src, std::nullopt, 50);
    EXPECT_NE(result.find("lines elided"), std::string::npos)
        << "Elision marker must appear in compressed output";
}

// With Language hint — algorithm is language-light in v1 so output should
// still satisfy the same invariants.
TEST(CompressBody, WithLanguageHint) {
    std::string src = make_large_cpp_function();
    int budget = 50;
    std::string result = axon::compress_body(src, axon::Language::Cpp, budget);
    EXPECT_LE(est_tokens(result), budget + 4);
    EXPECT_NE(result.find("int compute_answer"), std::string::npos);
}

// ── compress_body: robustness / safety ───────────────────────────────────────

TEST(CompressBody, InvalidUtf8DoesNotThrow) {
    // Inject raw invalid UTF-8 bytes into a large-ish buffer so compression
    // is actually attempted (not the early-return path).
    std::string src;
    src += "void broken() {\n";
    src += std::string(400, 'a');   // padding to force compression
    src += '\xFF';                   // invalid UTF-8 leader
    src += '\xFE';                   // invalid continuation
    src += "\n    return;\n}\n";
    EXPECT_NO_THROW({
        std::string r = axon::compress_body(src, std::nullopt, 20);
        // Must return something non-empty and safe.
        EXPECT_FALSE(r.empty());
    });
}

TEST(CompressBody, BinaryLikeBlobDoesNotThrow) {
    // Simulate a binary file accidentally fed to compress_body.
    std::string blob;
    blob.resize(2000);
    for (size_t i = 0; i < blob.size(); ++i)
        blob[i] = (char)(i & 0xFF);  // full 0-255 byte cycle
    EXPECT_NO_THROW({
        std::string r = axon::compress_body(blob, std::nullopt, 30);
        EXPECT_FALSE(r.empty());
    });
}

TEST(CompressBody, BinaryLikeBlobPassesThroughUnchanged) {
    std::string blob = "prefix";
    blob.push_back('\0');
    blob += std::string(1200, 'x');
    std::string r = axon::compress_body(blob, std::nullopt, 30);
    EXPECT_EQ(r, blob) << "Unsafe byte streams must pass through without silent loss";
}

TEST(CompressBody, ImpossibleBudgetPassesThroughUnchanged) {
    std::string src = make_large_cpp_function();
    std::string r = axon::compress_body(src, axon::Language::Cpp, 0);
    EXPECT_EQ(r, src);
}

TEST(CompressBody, LossyCompressionMustSaveTokens) {
    std::string src = make_large_cpp_function();
    std::string r = axon::compress_body(src, axon::Language::Cpp, 80);
    if (r != src) {
        EXPECT_LT(est_tokens(r), est_tokens(src));
    }
}

TEST(CompressBody, EmptyInputReturnedEmpty) {
    std::string result = axon::compress_body("", std::nullopt, 100);
    EXPECT_EQ(result, "");
}

TEST(CompressBody, SingleLineFitsUnchanged) {
    std::string src = "int x = 42;\n";
    std::string result = axon::compress_body(src, std::nullopt, 1000);
    EXPECT_EQ(result, src);
}
