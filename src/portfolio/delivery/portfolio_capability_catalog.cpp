#include "portfolio_capability_catalog.hpp"

#include "core/registry.hpp"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace axon::portfolio {
namespace {
using json = nlohmann::json;

std::string quote(const std::string& value) {
    std::string result{"'"};
    for (const char c : value) { if (c == '\'') result += '\''; result += c; }
    return result + "'";
}
template <typename Result>
void require_ok(const duckdb::unique_ptr<Result>& result, const char* action) {
    if (!result || result->HasError()) throw std::runtime_error(std::string(action) + ": " + (result ? result->GetError() : "no result"));
}
void migrate(duckdb::Connection& connection) {
    require_ok(connection.Query("CREATE TABLE IF NOT EXISTS portfolio_capability_streams(repository_id VARCHAR NOT NULL,index_stream_id VARCHAR NOT NULL,index_epoch VARCHAR NOT NULL,manifest_hash VARCHAR NOT NULL,stale BOOLEAN NOT NULL DEFAULT false,detail VARCHAR NOT NULL DEFAULT '',PRIMARY KEY(repository_id,index_stream_id))"), "create capability streams");
    try { require_ok(connection.Query("ALTER TABLE portfolio_capability_streams ADD COLUMN stale BOOLEAN NOT NULL DEFAULT false"), "add stale stream state"); } catch (...) {}
    try { require_ok(connection.Query("ALTER TABLE portfolio_capability_streams ADD COLUMN detail VARCHAR NOT NULL DEFAULT ''"), "add stream detail"); } catch (...) {}
    require_ok(connection.Query("CREATE TABLE IF NOT EXISTS portfolio_capability_signatures(repository_id VARCHAR NOT NULL,index_stream_id VARCHAR NOT NULL,signature_id VARCHAR NOT NULL,normalized_name VARCHAR NOT NULL,signature_json VARCHAR NOT NULL,PRIMARY KEY(repository_id,index_stream_id,signature_id))"), "create capability signatures");
}
json signature_json(const CapabilitySignature& signature) {
    return {{"signature_id",signature.signature_id},{"repository_id",signature.stream.repository_id},{"index_stream_id",signature.stream.index_stream_id},{"source_sequence",signature.source_sequence},{"index_epoch",signature.index_epoch},{"manifest_hash",signature.manifest_hash},{"bounded_context",signature.bounded_context.value_or("")},{"normalized_name",signature.normalized_name},{"summary",signature.deterministic_summary},{"path",signature.path.value_or("")},{"symbols",signature.public_symbols},{"routes",signature.routes},{"handlers",signature.handlers},{"contracts",signature.contracts},{"events",signature.events},{"tests",signature.tests},{"ast_fingerprints",signature.ast_fingerprints},{"internal_dependencies",signature.internal_dependencies},{"external_dependencies",signature.external_dependencies},{"technologies",signature.technologies},{"evidence",json::array()}};
}
std::vector<std::string> strings(const json& value, const char* field) { return value.contains(field) && value.at(field).is_array() ? value.at(field).get<std::vector<std::string>>() : std::vector<std::string>{}; }
CapabilitySignature decode(const std::string& text) {
    const auto data = json::parse(text); CapabilitySignature signature;
    signature.signature_id=data.value("signature_id",""); signature.stream={data.value("repository_id", ""),data.value("index_stream_id","")}; signature.source_sequence=data.value("source_sequence",0ULL); signature.index_epoch=data.value("index_epoch",""); signature.manifest_hash=data.value("manifest_hash",""); const auto context=data.value("bounded_context",""); if(!context.empty()) signature.bounded_context=context; signature.normalized_name=data.value("normalized_name",""); signature.deterministic_summary=data.value("summary",""); const auto path=data.value("path",""); if(!path.empty()) signature.path=path; signature.public_symbols=strings(data,"symbols"); signature.routes=strings(data,"routes"); signature.handlers=strings(data,"handlers"); signature.contracts=strings(data,"contracts"); signature.events=strings(data,"events"); signature.tests=strings(data,"tests"); signature.ast_fingerprints=strings(data,"ast_fingerprints"); signature.internal_dependencies=strings(data,"internal_dependencies"); signature.external_dependencies=strings(data,"external_dependencies"); signature.technologies=strings(data,"technologies"); return signature;
}
void optional_strings(duckdb::Connection& source,const std::string& query,std::vector<std::string>& values) {
    auto rows=source.Query(query);
    if(!rows || rows->HasError()) return;
    for(duckdb::idx_t i=0;i<rows->RowCount();++i) values.push_back(rows->GetValue(0,i).ToString());
}
void optional_context(duckdb::Connection& source,const std::string& query,std::optional<std::string>& context) {
    auto rows=source.Query(query);
    if(!rows || rows->HasError() || rows->RowCount()==0) return;
    context=rows->GetValue(0,0).ToString();
}
std::vector<CapabilitySignature> extract(duckdb::Connection& source, const axon::RepoEntry& repo) {
    auto meta=source.Query("SELECT repository_id,index_stream_id,current_epoch,current_manifest FROM index_metadata WHERE singleton=true"); require_ok(meta,"read source identity"); if(meta->RowCount()!=1) throw std::runtime_error("source index identity unavailable");
    const auto repository_id=meta->GetValue(0,0).ToString(); const auto stream_id=meta->GetValue(1,0).ToString(); const auto epoch=meta->GetValue(2,0).ToString(); const auto manifest=meta->GetValue(3,0).ToString();
    if(repository_id!=repo.repository_id || stream_id!=repo.index_stream_id) throw std::runtime_error("registered source identity diverges from index metadata");
    auto sequence=source.Query("SELECT COALESCE(MAX(sequence),0) FROM index_events"); require_ok(sequence,"read source cursor"); const auto cursor=sequence->GetValue<std::uint64_t>(0,0);
    auto files=source.Query("SELECT id,path,language,hash FROM files ORDER BY path LIMIT 10000"); require_ok(files,"read source files"); if(files->RowCount()>=10000) throw std::runtime_error("source capability fan-out exceeds 10000 files");
    std::vector<CapabilitySignature> result;
    for(duckdb::idx_t row=0;row<files->RowCount();++row) {
        const auto id=files->GetValue<int64_t>(0,row); const auto path=files->GetValue(1,row).ToString(); CapabilitySignature signature;
        signature.stream={repository_id,stream_id}; signature.source_sequence=cursor; signature.index_epoch=epoch; signature.manifest_hash=manifest; signature.path=path; signature.normalized_name=normalize_capability_name(std::filesystem::path(path).stem().string()); signature.technologies={files->GetValue(2,row).ToString()};
        auto symbols=source.Query("SELECT name FROM symbols WHERE file_id="+std::to_string(id)+" ORDER BY name LIMIT 2000"); require_ok(symbols,"read source symbols"); for(duckdb::idx_t i=0;i<symbols->RowCount();++i) signature.public_symbols.push_back(symbols->GetValue(0,i).ToString());
        auto routes=source.Query("SELECT method,path FROM routes WHERE handler_file="+quote(path)+" ORDER BY method,path LIMIT 1000"); require_ok(routes,"read source routes"); for(duckdb::idx_t i=0;i<routes->RowCount();++i) signature.routes.push_back(routes->GetValue(0,i).ToString()+" "+routes->GetValue(1,i).ToString());
        auto dependencies=source.Query("SELECT DISTINCT target.path FROM edges AS edge JOIN files AS target ON target.id=edge.to_file WHERE edge.from_file="+std::to_string(id)+" ORDER BY target.path LIMIT 5000"); require_ok(dependencies,"read source dependencies"); for(duckdb::idx_t i=0;i<dependencies->RowCount();++i) signature.internal_dependencies.push_back(dependencies->GetValue(0,i).ToString());
        auto external=source.Query("SELECT specifier FROM external_dependencies WHERE from_file="+std::to_string(id)+" ORDER BY specifier LIMIT 5000"); if(external&&!external->HasError()) for(duckdb::idx_t i=0;i<external->RowCount();++i) signature.external_dependencies.push_back(external->GetValue(0,i).ToString());
        optional_context(source,"SELECT bounded_context FROM capability_contexts WHERE file_id="+std::to_string(id)+" LIMIT 1",signature.bounded_context);
        optional_strings(source,"SELECT value FROM capability_ast_fingerprints WHERE file_id="+std::to_string(id)+" ORDER BY value LIMIT 2000",signature.ast_fingerprints);
        signature.deterministic_summary=signature.normalized_name+" ["+signature.technologies.front()+"] symbols="+std::to_string(signature.public_symbols.size())+" routes="+std::to_string(signature.routes.size()); signature.signature_id=capability_fingerprint(signature); result.push_back(std::move(signature));
    }
    return result;
}
}

PortfolioCapabilityCatalog::PortfolioCapabilityCatalog(std::filesystem::path path) : path_(std::move(path)) { if(path_.empty()) path_=axon::registry_path().parent_path()/"portfolio-capability-catalog.duckdb"; }
std::filesystem::path PortfolioCapabilityCatalog::path() const { return path_; }
PortfolioSyncReport PortfolioCapabilityCatalog::sync(const std::optional<std::string>& group, bool force_rebuild) {
    axon::RegistryData registry=axon::load_registry(); auto selection=axon::aggregation_repos(registry,group); PortfolioSyncReport report; report.degraded=!selection.issues.empty();
    duckdb::DuckDB target(path_.string()); duckdb::Connection central(target); migrate(central);
    for(const auto& repo:selection.repos) { PortfolioSyncItem item{repo.repository_id,repo.index_stream_id,"synced","",0}; try {
        auto opened=axon::open_secondary_read_only(repo); if(!opened) throw std::runtime_error(opened.error_code+": "+opened.error); duckdb::Connection source(*opened.db); auto signatures=extract(source,repo);
        auto metadata=source.Query("SELECT current_epoch,current_manifest FROM index_metadata WHERE singleton=true"); require_ok(metadata,"read source freshness"); if(metadata->RowCount()!=1) throw std::runtime_error("source freshness is unavailable"); const auto epoch=metadata->GetValue(0,0).ToString(); const auto manifest=metadata->GetValue(1,0).ToString();
        auto prior=central.Query("SELECT index_epoch,manifest_hash,stale,(SELECT COALESCE(MAX(CAST(json_extract(signature_json,'$.source_sequence') AS UBIGINT)),0) FROM portfolio_capability_signatures WHERE repository_id="+quote(repo.repository_id)+" AND index_stream_id="+quote(repo.index_stream_id)+") FROM portfolio_capability_streams WHERE repository_id="+quote(repo.repository_id)+" AND index_stream_id="+quote(repo.index_stream_id)); require_ok(prior,"read capability stream");
        if(!force_rebuild && prior->RowCount()==1 && prior->GetValue(0,0).ToString()==epoch && prior->GetValue(1,0).ToString()==manifest && prior->GetValue<std::uint64_t>(3,0)==(signatures.empty()?0:signatures.front().source_sequence)) {
            if (prior->GetValue<bool>(2,0)) { require_ok(central.Query("BEGIN TRANSACTION"),"begin stale recovery"); require_ok(central.Query("UPDATE portfolio_capability_streams SET stale=false,detail='' WHERE repository_id="+quote(repo.repository_id)+" AND index_stream_id="+quote(repo.index_stream_id)),"clear recovered stale state"); require_ok(central.Query("COMMIT"),"commit stale recovery"); item.status="recovered"; }
            else item.status="unchanged";
            item.signatures=signatures.size(); report.repositories.push_back(std::move(item)); continue;
        }
        require_ok(central.Query("BEGIN TRANSACTION"),"begin capability projection"); require_ok(central.Query("DELETE FROM portfolio_capability_signatures WHERE repository_id="+quote(repo.repository_id)+" AND index_stream_id="+quote(repo.index_stream_id)),"clear capability partition");
        for(const auto& signature:signatures) require_ok(central.Query("INSERT INTO portfolio_capability_signatures VALUES("+quote(repo.repository_id)+","+quote(repo.index_stream_id)+","+quote(signature.signature_id)+","+quote(signature.normalized_name)+","+quote(signature_json(signature).dump())+")"),"insert capability signature");
        require_ok(central.Query("INSERT INTO portfolio_capability_streams VALUES("+quote(repo.repository_id)+","+quote(repo.index_stream_id)+","+quote(epoch)+","+quote(manifest)+",false,'') ON CONFLICT(repository_id,index_stream_id) DO UPDATE SET index_epoch=excluded.index_epoch,manifest_hash=excluded.manifest_hash,stale=false,detail=''"),"save capability stream"); require_ok(central.Query("COMMIT"),"commit capability projection"); item.signatures=signatures.size();
    } catch(const std::exception& error) { item.status="stale"; item.detail=error.what(); report.degraded=true; (void)central.Query("ROLLBACK"); try { require_ok(central.Query("INSERT INTO portfolio_capability_streams VALUES("+quote(repo.repository_id)+","+quote(repo.index_stream_id)+",'', '',true,"+quote(item.detail)+") ON CONFLICT(repository_id,index_stream_id) DO UPDATE SET stale=true,detail=excluded.detail"),"persist stale stream"); } catch (...) {} } report.repositories.push_back(std::move(item)); }
    return report;
}
PortfolioSyncReport PortfolioCapabilityCatalog::status() const { duckdb::DuckDB db(path_.string()); duckdb::Connection c(db); migrate(c); auto rows=c.Query("SELECT repository_id,index_stream_id,stale,detail FROM portfolio_capability_streams ORDER BY repository_id,index_stream_id"); require_ok(rows,"read capability status"); PortfolioSyncReport report; for(duckdb::idx_t i=0;i<rows->RowCount();++i) { const bool stale=rows->GetValue<bool>(2,i); report.repositories.push_back({rows->GetValue(0,i).ToString(),rows->GetValue(1,i).ToString(),stale?"stale":"healthy",rows->GetValue(3,i).ToString(),0}); report.degraded=report.degraded||stale; } return report; }
std::vector<CapabilitySignature> PortfolioCapabilityCatalog::list(const std::optional<std::string>& repository_id,std::size_t limit) const { if(limit==0||limit>10000) throw std::invalid_argument("invalid capability list limit"); duckdb::DuckDB db(path_.string()); duckdb::Connection c(db); migrate(c); auto query="SELECT signature_json FROM portfolio_capability_signatures"+(repository_id?" WHERE repository_id="+quote(*repository_id):"")+" ORDER BY repository_id,signature_id LIMIT "+std::to_string(limit); auto rows=c.Query(query); require_ok(rows,"list capabilities"); std::vector<CapabilitySignature> out; for(duckdb::idx_t i=0;i<rows->RowCount();++i) out.push_back(decode(rows->GetValue(0,i).ToString())); return out; }
std::vector<CapabilitySignature> PortfolioCapabilityCatalog::search(const std::string& query,std::size_t limit) const { if(query.empty()||query.size()>256||limit==0||limit>10000) throw std::invalid_argument("invalid capability query"); auto all=list({},10000); std::vector<CapabilitySignature> out; const auto normalized=normalize_capability_name(query); if(normalized.empty()) throw std::invalid_argument("invalid capability query"); for(const auto& signature:all) if(signature.normalized_name.find(normalized)!=std::string::npos || signature.deterministic_summary.find(query)!=std::string::npos) { out.push_back(signature); if(out.size()==limit) break; } return out; }
std::vector<CapabilityCandidate> PortfolioCapabilityCatalog::duplicates(double threshold,std::size_t limit) const { if(threshold<0||threshold>1||limit==0||limit>10000) throw std::invalid_argument("invalid candidate query"); auto out=CapabilityCandidateGenerator().generate(list({},2000)); out.erase(std::remove_if(out.begin(),out.end(),[&](const auto& candidate){return candidate.final_score<threshold;}),out.end()); if(out.size()>limit) out.resize(limit); return out; }
DeclarationComparison PortfolioCapabilityCatalog::drift(const std::filesystem::path& root,const std::filesystem::path& fragment,std::size_t limit) const { if(limit==0||limit>10000) throw std::invalid_argument("invalid drift limit"); std::error_code error; const auto canonical=std::filesystem::canonical(root,error); if(error) throw std::invalid_argument("registered declaration root is unavailable"); bool registered=false; for(const auto& repo:axon::load_registry().repos) { const auto candidate=std::filesystem::canonical(repo.root,error); if(!error&&candidate==canonical) { registered=true; break; } error.clear(); } if(!registered) throw std::invalid_argument("declaration root is not registered"); auto observed=list({},limit); const auto imported=CapabilityDeclarationImporter().import(canonical,fragment); return compare_declarations(observed,imported.declarations); }
} // namespace axon::portfolio
