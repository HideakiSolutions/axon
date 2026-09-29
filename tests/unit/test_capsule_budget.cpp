#include <gtest/gtest.h>
#include "core/db.hpp"
#include "core/capsule.hpp"
#include "core/embeddings.hpp"
#include "core/graph.hpp"
#include "core/skeleton.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ── Fixture: real embedding model (skips when unavailable, same idiom as
//    test_semantic.cpp) + on-disk project with a small pivot and a large
//    signature-heavy support file whose skeleton alone exceeds the budget. ───

static fs::path make_temp_dir(const char* tag) {
    static int counter = 0;
    auto p = fs::temp_directory_path() / (std::string(tag) + "_" + std::to_string(::getpid()) +
                                          "_" + std::to_string(++counter));
    fs::create_directories(p);
    return p;
}

class CapsuleBudgetTest : public ::testing::Test {
protected:
    fs::path proj;
    fs::path db_path;
    std::unique_ptr<axon::Database> db;
    std::unique_ptr<axon::EmbeddingModel> model;

    void SetUp() override {
        const char* env = std::getenv("AXON_EMBEDDING_MODEL");
        if (!env || std::string(env).empty()) {
            GTEST_SKIP() << "Set AXON_EMBEDDING_MODEL to a GGUF embedding model "
                         << "to enable capsule budget tests";
        }
        fs::path mp = env;
        if (!fs::exists(mp)) {
            GTEST_SKIP() << "Embedding model not found at " << mp.string();
        }
        try {
            model = std::make_unique<axon::EmbeddingModel>(mp);
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Failed to load embedding model: " << e.what();
        }
        if (model->dims() != 768) {
            GTEST_SKIP() << "Schema pins FLOAT[768]; model dims=" << model->dims();
        }

        proj = make_temp_dir("axon_capsule_budget");
        db_path = proj / "index.duckdb";
        db = std::make_unique<axon::Database>(db_path);
    }

    void TearDown() override {
        model.reset();
        db.reset();
        fs::remove_all(proj);
    }

    int64_t insert_file(const std::string& rel_path, const std::string& content) {
        std::ofstream f(proj / rel_path, std::ios::binary);
        f << content;
        f.close();
        db->conn().Query("INSERT INTO files (id, path, language, hash, byte_size) VALUES "
                         "(nextval('seq_id'), '" +
                         rel_path + "', 'typescript', 'h', " + std::to_string(content.size()) +
                         ")");
        auto r = db->conn().Query("SELECT id FROM files WHERE path = '" + rel_path + "'");
        return r->GetValue<int64_t>(0, 0);
    }

    int64_t insert_file_with_skeleton(const std::string& rel_path, const std::string& content,
                                      const std::string& skeleton) {
        int64_t fid = insert_file(rel_path, content);
        db->conn().Query("UPDATE files SET skeleton = '" + skeleton +
                         "' WHERE id = " + std::to_string(fid));
        return fid;
    }

    int64_t insert_symbol_with_embedding(int64_t file_id, const std::string& name,
                                         const std::string& embed_text) {
        auto vec = model->embed(embed_text);
        std::ostringstream vs;
        vs << "[";
        for (size_t i = 0; i < vec.size(); i++) {
            if (i) vs << ",";
            vs << vec[i];
        }
        vs << "]";
        db->conn().Query(
            "INSERT INTO symbols (id, file_id, name, kind, start_line, end_line, signature, "
            "embedding) VALUES (nextval('seq_id'), " +
            std::to_string(file_id) + ", '" + name + "', 'function', 1, 3, 'export function " +
            name + "(): void', " + vs.str() + "::FLOAT[768])");
        auto r = db->conn().Query("SELECT id FROM symbols WHERE name = '" + name + "'");
        return r->GetValue<int64_t>(0, 0);
    }
};

// Regression for the AIEO-scale finding: a support file whose SKELETON alone
// exceeds the remaining budget must not be appended by the symbol-mode
// file-level augmentation (observed live: budget=8000, capsule=11693 because
// a 44 KB signatures-only types.ts skeletonizes to ~10.9k tokens).
TEST_F(CapsuleBudgetTest, SymbolModeAugmentRespectsBudgetWithOversizedSupportSkeleton) {
    const int budget = 2000;

    // Small pivot the query will match semantically.
    std::string pivot_src = "export function dispatchTask(job: string): void {\n"
                            "  // route the job to a worker\n"
                            "}\n";
    int64_t pivot_fid = insert_file("pivot.ts", pivot_src);
    insert_symbol_with_embedding(pivot_fid, "dispatchTask", "dispatch a task to a worker");

    // Signature-heavy support file: its skeleton barely compresses, so it can
    // never fit the remaining budget.
    std::ostringstream big;
    for (int i = 0; i < 3000; i++)
        big << "export interface Gen" << i << " { field" << i << ": string }\n";
    std::string big_src = big.str();
    int64_t big_fid = insert_file("big_types.ts", big_src);

    // Fixture guard: the skeleton itself must exceed the whole budget,
    // otherwise this test would not exercise the overflow path.
    int skel_tokens = axon::estimate_tokens(axon::skeletonize(big_src, axon::Language::TypeScript));
    ASSERT_GT(skel_tokens, budget) << "fixture too small to exercise the budget overflow path";

    // Hand-built graph: big_types.ts imports pivot.ts (one BFS hop), and no
    // symbol-level edges so the file-level augmentation path runs.
    axon::DependencyGraph graph;
    graph.id_to_path[pivot_fid] = "pivot.ts";
    graph.id_to_path[big_fid] = "big_types.ts";
    graph.path_to_id["pivot.ts"] = pivot_fid;
    graph.path_to_id["big_types.ts"] = big_fid;
    graph.incoming[pivot_fid] = {big_fid};
    graph.outgoing[big_fid] = {pivot_fid};
    graph.file_byte_size[pivot_fid] = (int64_t)pivot_src.size();
    graph.file_byte_size[big_fid] = (int64_t)big_src.size();

    auto capsule = axon::assemble_capsule("dispatch a task to a worker", {}, *db, *model, graph,
                                          proj, budget, 0, axon::CapsuleCompression::Off);

    ASSERT_FALSE(capsule.pivot_files.empty());
    EXPECT_LE(capsule.token_estimate, budget)
        << "capsule exceeded its token budget: support files must be fit-checked "
        << "before being appended";
}

// ── F12-C1: support skeletons come from the index, not a live re-parse ──────

namespace {
// Shared shape for the two skeleton-source tests: one semantic pivot, one
// support file reachable in a single BFS hop, no symbol-level edges (so the
// file-level augmentation path runs).
struct SkeletonSourceFixture {
    axon::DependencyGraph graph;
    std::string support_src = "export function helperFn(x: number): number {\n"
                              "  return x * 2;\n"
                              "}\n";
};
} // namespace

TEST_F(CapsuleBudgetTest, SupportSkeletonServedFromIndex) {
    // The stored skeleton is deliberately different from what a live
    // skeletonize of the on-disk content would produce — the capsule content
    // proves which source was used. This is also the contract: skeletons
    // reflect the INDEX (like every other capsule ingredient), not the disk.
    SkeletonSourceFixture fx;
    const std::string sentinel = "// SERVED_FROM_INDEX\n";

    std::string pivot_src = "export function dispatchTask(job: string): void {}\n";
    int64_t pivot_fid = insert_file("pivot.ts", pivot_src);
    insert_symbol_with_embedding(pivot_fid, "dispatchTask", "dispatch a task to a worker");
    int64_t sup_fid = insert_file_with_skeleton("helper.ts", fx.support_src, sentinel);

    fx.graph.id_to_path[pivot_fid] = "pivot.ts";
    fx.graph.id_to_path[sup_fid] = "helper.ts";
    fx.graph.path_to_id["pivot.ts"] = pivot_fid;
    fx.graph.path_to_id["helper.ts"] = sup_fid;
    fx.graph.incoming[pivot_fid] = {sup_fid};
    fx.graph.outgoing[sup_fid] = {pivot_fid};
    fx.graph.file_byte_size[pivot_fid] = (int64_t)pivot_src.size();
    fx.graph.file_byte_size[sup_fid] = (int64_t)fx.support_src.size();

    auto capsule = axon::assemble_capsule("dispatch a task to a worker", {}, *db, *model, fx.graph,
                                          proj, 2000, 0, axon::CapsuleCompression::Off);

    const axon::CapsuleFile* sup = nullptr;
    for (const auto& f : capsule.support_files)
        if (f.path == "helper.ts") sup = &f;
    ASSERT_NE(sup, nullptr) << "support file missing from the capsule";
    EXPECT_EQ(sup->content, sentinel)
        << "support skeleton must be served from files.skeleton, not re-parsed from disk";
}

TEST_F(CapsuleBudgetTest, SupportSkeletonFallsBackWhenIndexEmpty) {
    // NULL skeleton column (pre-column DB, swallowed index-time exception):
    // the old live path must run unchanged.
    SkeletonSourceFixture fx;

    std::string pivot_src = "export function dispatchTask(job: string): void {}\n";
    int64_t pivot_fid = insert_file("pivot.ts", pivot_src);
    insert_symbol_with_embedding(pivot_fid, "dispatchTask", "dispatch a task to a worker");
    int64_t sup_fid = insert_file("helper.ts", fx.support_src); // skeleton stays NULL

    fx.graph.id_to_path[pivot_fid] = "pivot.ts";
    fx.graph.id_to_path[sup_fid] = "helper.ts";
    fx.graph.path_to_id["pivot.ts"] = pivot_fid;
    fx.graph.path_to_id["helper.ts"] = sup_fid;
    fx.graph.incoming[pivot_fid] = {sup_fid};
    fx.graph.outgoing[sup_fid] = {pivot_fid};
    fx.graph.file_byte_size[pivot_fid] = (int64_t)pivot_src.size();
    fx.graph.file_byte_size[sup_fid] = (int64_t)fx.support_src.size();

    auto capsule = axon::assemble_capsule("dispatch a task to a worker", {}, *db, *model, fx.graph,
                                          proj, 2000, 0, axon::CapsuleCompression::Off);

    const axon::CapsuleFile* sup = nullptr;
    for (const auto& f : capsule.support_files)
        if (f.path == "helper.ts") sup = &f;
    ASSERT_NE(sup, nullptr) << "support file missing from the capsule";
    EXPECT_EQ(sup->content, axon::skeletonize(fx.support_src, axon::Language::TypeScript))
        << "empty index skeleton must fall back to the live skeletonize";
}
