#include "portfolio/application/declarations/capability_declarations.hpp"
#include "portfolio/infrastructure/git/git_process.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
namespace {
using namespace axon::portfolio;
CapabilitySignature observed(const std::string& id, const std::string& name,
                             const std::string& context) {
    CapabilitySignature s;
    s.signature_id = id;
    s.stream = {"repo-observed", "stream-observed"};
    s.normalized_name = name;
    s.bounded_context = context;
    s.contracts = {"AuthorizeRequest"};
    return s;
}
void git_ok(const std::filesystem::path& root, std::vector<std::string> arguments) {
    const auto result = git::run(root, arguments);
    ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
    ASSERT_FALSE(result.output_truncated);
}
std::filesystem::path fixture_root() {
    const auto root = std::filesystem::temp_directory_path() / "axon_g12_declaration_fixture";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    git_ok(root, {"init", "--quiet"});
    git_ok(root, {"config", "user.email", "axon-tests@example.invalid"});
    git_ok(root, {"config", "user.name", "Axon Tests"});
    std::filesystem::copy_file(std::filesystem::path(AXON_SOURCE_DIR) /
                                   "tests/fixtures/capability-graph/repo/capabilities.json",
                               root / "capabilities.json");
    git_ok(root, {"add", "capabilities.json"});
    git_ok(root, {"commit", "--quiet", "-m", "capability graph fixture"});
    return root;
}
TEST(CapabilityDeclarations, ImportsImmutableGitBlobWithProvenanceAndTypedDrift) {
    const auto root = fixture_root();
    const auto imported = CapabilityDeclarationImporter().import(root, "capabilities.json");
    ASSERT_EQ(imported.declarations.size(), 2U);
    EXPECT_EQ(imported.declarations[0].source_commit.size(), 40U);
    EXPECT_EQ(imported.declarations[0].source_path, "capabilities.json");
    std::ofstream(root / "capabilities.json")
        << R"({"schema_version":"axon/capability-graph/v1","capabilities":[]})";
    const auto still_committed = CapabilityDeclarationImporter().import(root, "capabilities.json");
    ASSERT_EQ(still_committed.declarations.size(), 2U);
    EXPECT_EQ(still_committed.declarations[0].source_commit,
              imported.declarations[0].source_commit);
    const auto compared =
        compare_declarations({observed("signature-one", "payment authorize", "billing"),
                              observed("signature-two", "catalog search", "catalog")},
                             imported.declarations);
    ASSERT_EQ(compared.matches.size(), 1U);
    EXPECT_EQ(compared.matches[0].declared_id, "payments.authorize");
    EXPECT_EQ(compared.matches[0].evidence[0], "normalized_name match");
    EXPECT_EQ(compared.drift.size(), 2U);
}
TEST(CapabilityDeclarations, RejectsEscapingWorktreeSymlinkAndGitSymlink) {
    const auto root = fixture_root();
    EXPECT_THROW(CapabilityDeclarationImporter().import(root, "../capabilities.json"),
                 std::invalid_argument);
    const auto outside = std::filesystem::temp_directory_path() / "axon_g12_outside";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::filesystem::copy_file(root / "capabilities.json", outside / "capabilities.json");
    std::filesystem::create_directory_symlink(outside, root / "linked");
    EXPECT_THROW(CapabilityDeclarationImporter().import(root, "linked/capabilities.json"),
                 std::invalid_argument);
    std::filesystem::create_symlink("capabilities.json", root / "git-link.json");
    git_ok(root, {"add", "git-link.json"});
    git_ok(root, {"commit", "--quiet", "-m", "symlink fixture"});
    EXPECT_THROW(CapabilityDeclarationImporter().import(root, "git-link.json"),
                 std::invalid_argument);
}
TEST(CapabilityDeclarations, SupportsGitWorktreeMetadataWithoutReadingItsWorktreeFile) {
    const auto root = fixture_root();
    const auto worktree = std::filesystem::temp_directory_path() / "axon_g12_declaration_worktree";
    std::filesystem::remove_all(worktree);
    git_ok(root, {"worktree", "add", "--detach", worktree.string(), "HEAD"});
    ASSERT_TRUE(std::filesystem::is_regular_file(worktree / ".git"));
    std::ofstream(worktree / "capabilities.json") << "not the committed declaration";
    const auto imported = CapabilityDeclarationImporter().import(worktree, "capabilities.json");
    EXPECT_EQ(imported.declarations.size(), 2U);
}
TEST(CapabilityDeclarations, IgnoresLocalGitReplacementRefsForImmutableProvenance) {
    const auto root = fixture_root();
    const auto original = git::run(root, {"rev-parse", "HEAD"});
    ASSERT_EQ(original.exit_code, 0);
    std::string original_id = original.stdout_text;
    while (!original_id.empty() && (original_id.back() == '\n' || original_id.back() == '\r'))
        original_id.pop_back();
    std::ofstream(root / "capabilities.json")
        << R"({"schema_version":"axon/capability-graph/v1","capabilities":[{"id":"injected","name":"injected","contracts":[]}]})";
    git_ok(root, {"add", "capabilities.json"});
    git_ok(root, {"commit", "--quiet", "-m", "replacement content"});
    git_ok(root, {"replace", original_id, "HEAD"});
    git_ok(root, {"reset", "--hard", original_id});
    const auto imported = CapabilityDeclarationImporter().import(root, "capabilities.json");
    EXPECT_EQ(imported.declarations.size(), 2U);
    EXPECT_EQ(imported.declarations[0].source_commit, original_id);
}
TEST(CapabilityDeclarations, AmbiguityDoesNotHideOtherMissingDeclarations) {
    const auto first = observed("one", "shared capability", "platform");
    DeclaredCapability a{"shared.a", "shared capability", "platform"};
    DeclaredCapability b{"shared.b", "shared capability", "platform"};
    DeclaredCapability orphan{"orphan", "unimplemented capability", "platform"};
    const auto result = compare_declarations({first}, {a, b, orphan});
    EXPECT_EQ(result.matches.size(), 0U);
    EXPECT_EQ(result.drift.size(), 2U);
    EXPECT_EQ(result.drift[0].kind, CapabilityDriftKind::ambiguous_match);
    EXPECT_EQ(result.drift[1].kind, CapabilityDriftKind::declaration_without_observed);
    EXPECT_EQ(result.drift[1].subject_id, "orphan");
}
} // namespace
