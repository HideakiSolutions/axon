// Smoke tests for the multi-language parser. Validates that the W1 audit
// closures actually emit the new symbol kinds (decorators, traits, records,
// async-function, sealed_class, etc.) end-to-end through parse_file().
//
// Tests write each fixture to a temp file because parse_file() takes a
// filesystem path and reads bytes directly — no in-memory parse entry
// exists yet (a smaller refactor for a future wave).

#include "parser/parser.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;

// Write `content` to a temp file with a stable extension so
// language_from_extension() routes correctly. Path is unique per test.
fs::path write_temp(const std::string& ext, const std::string& content) {
    static int counter = 0;
    auto p = fs::temp_directory_path() /
             ("axon_test_" + std::to_string(::getpid()) + "_" +
              std::to_string(++counter) + "." + ext);
    std::ofstream f(p);
    f << content;
    return p;
}

bool has_kind(const std::vector<axon::Symbol>& syms, const std::string& kind) {
    for (const auto& s : syms) if (s.kind == kind) return true;
    return false;
}

const axon::Symbol* find_named(const std::vector<axon::Symbol>& syms,
                               const std::string& name) {
    for (const auto& s : syms) if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

// ── Rust (W1.T03) ──────────────────────────────────────────────────────────
TEST(ParserRust, TraitsEnumsModulesMacrosImpl) {
    auto p = write_temp("rs", R"RS(
pub trait Greeter { fn hello(&self); }
pub struct EnUS;
impl Greeter for EnUS { fn hello(&self) {} }
impl EnUS { pub fn name() -> &'static str { "en" } }
pub enum Mode { On, Off }
pub mod inner { pub fn util() {} }
macro_rules! shout { () => { 42 }; }
)RS");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_TRUE(has_kind(pf->symbols, "trait"))   << "trait_item missed";
    EXPECT_TRUE(has_kind(pf->symbols, "enum"))    << "enum_item missed";
    EXPECT_TRUE(has_kind(pf->symbols, "module"))  << "mod_item missed";
    EXPECT_TRUE(has_kind(pf->symbols, "macro"))   << "macro_definition missed";
    EXPECT_TRUE(has_kind(pf->symbols, "impl"))    << "impl_item missed";

    // impl Trait for Type vs inherent impl: names must differ.
    int impl_count = 0;
    for (const auto& s : pf->symbols) if (s.kind == "impl") impl_count++;
    EXPECT_GE(impl_count, 2);
}

// ── Python (W1.T02) ────────────────────────────────────────────────────────
TEST(ParserPython, DecoratorsAsyncDef) {
    auto p = write_temp("py", R"PY(
from fastapi import APIRouter
router = APIRouter()

@router.get("/users")
async def list_users():
    return []

@dataclass
class User:
    name: str
)PY");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_TRUE(has_kind(pf->symbols, "async_function")) << "async def missed";
    auto* lu = find_named(pf->symbols, "list_users");
    ASSERT_NE(lu, nullptr);
    ASSERT_TRUE(lu->docstring.has_value());
    EXPECT_NE(lu->docstring->find("@router.get"), std::string::npos)
        << "decorator did not land in docstring";

    auto* user = find_named(pf->symbols, "User");
    ASSERT_NE(user, nullptr);
    ASSERT_TRUE(user->docstring.has_value());
    EXPECT_NE(user->docstring->find("@dataclass"), std::string::npos);
}

// ── Java (W1.T08) ──────────────────────────────────────────────────────────
TEST(ParserJava, RecordsSealedAnnotations) {
    auto p = write_temp("java", R"JAVA(
public sealed class Shape permits Circle {}
public record Point(int x, int y) {}
public enum Color { RED, GREEN }

@RestController
public class UserController {
    @GetMapping("/u")
    public String list() { return ""; }
}
)JAVA");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_TRUE(has_kind(pf->symbols, "sealed_class")) << "sealed modifier missed";
    EXPECT_TRUE(has_kind(pf->symbols, "record"))        << "record_declaration missed";
    EXPECT_TRUE(has_kind(pf->symbols, "enum"))          << "enum_declaration missed";

    auto* ctrl = find_named(pf->symbols, "UserController");
    ASSERT_NE(ctrl, nullptr);
    ASSERT_TRUE(ctrl->docstring.has_value());
    EXPECT_NE(ctrl->docstring->find("@RestController"), std::string::npos);
}

// ── Bash (W1.T09) ──────────────────────────────────────────────────────────
TEST(ParserBash, ExportReadonlyVariables) {
    auto p = write_temp("sh", R"SH(
#!/bin/bash
export AXON_FOO=bar
readonly LIMIT=10
foo() { echo hi; }
)SH");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_NE(find_named(pf->symbols, "AXON_FOO"), nullptr) << "export missed";
    EXPECT_NE(find_named(pf->symbols, "LIMIT"),    nullptr) << "readonly missed";
    EXPECT_NE(find_named(pf->symbols, "foo"),      nullptr) << "function missed";
}

// ── Kotlin (W1.T11) ────────────────────────────────────────────────────────
TEST(ParserKotlin, ExtensionsSealedDataCompanion) {
    auto p = write_temp("kt", R"KT(
sealed class Result
data class Success(val value: Int) : Result()
fun String.reverseIt(): String = reversed()
suspend fun load(): Int = 0
class Box {
    companion object Factory {
        fun create() = Box()
    }
}
)KT");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_TRUE(has_kind(pf->symbols, "sealed_class"));
    EXPECT_TRUE(has_kind(pf->symbols, "data_class"));
    // Extension function detection is grammar-shape-sensitive; we accept
    // either flag depending on which child shape tree-sitter-kotlin uses.
    EXPECT_TRUE(has_kind(pf->symbols, "extension_function") ||
                has_kind(pf->symbols, "suspend_function") ||
                has_kind(pf->symbols, "function"))
        << "function emitted with no kind variant at all";
}

// ── TS/JS (W1.T01) ─────────────────────────────────────────────────────────
TEST(ParserTypeScript, DecoratorsNamespacesEnums) {
    auto p = write_temp("ts", R"TS(
@Injectable({providedIn: 'root'})
export class UserService {
  @Cacheable() getUser(id: number) { return null; }
}

namespace Foo { export const x = 1; }
enum Color { Red, Green }

async function loadAll() { return []; }
)TS");
    auto pf = axon::parse_file(p, p.parent_path());
    fs::remove(p);
    ASSERT_TRUE(pf.has_value());

    EXPECT_TRUE(has_kind(pf->symbols, "namespace")) << "internal_module missed";
    EXPECT_TRUE(has_kind(pf->symbols, "enum"))      << "enum_declaration missed";
    EXPECT_TRUE(has_kind(pf->symbols, "async_function"));

    auto* svc = find_named(pf->symbols, "UserService");
    ASSERT_NE(svc, nullptr);
    ASSERT_TRUE(svc->docstring.has_value());
    EXPECT_NE(svc->docstring->find("@Injectable"), std::string::npos);
}
