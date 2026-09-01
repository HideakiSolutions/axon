#include "portfolio/application/candidates/capability_candidates.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace axon::portfolio;
using json=nlohmann::json;
struct Fixture { CapabilitySignature left,right; std::vector<SemanticCandidateHint> hints; };
CapabilitySignature sig(const std::string&r,const std::string&i,const std::string&n){CapabilitySignature x;x.stream={r,"stream_"+r};x.signature_id=i;x.normalized_name=n;x.bounded_context="billing";return x;}
Fixture fixture(const std::string&id){
 auto a=sig("repo_a",id+"_a","payment authorize"),b=sig("repo_b",id+"_b","payment authorize");
 auto same=[&]{a.contracts=b.contracts={"AuthorizeRequest"};a.routes=b.routes={"POST /authorize"};a.events=b.events={"payment.authorized"};};
 if(id=="exact-duplicate"){same();a.ast_fingerprints=b.ast_fingerprints={"ast_a"};a.tests=b.tests={"contract"};}
 else if(id=="convergent"){a.normalized_name=b.normalized_name="invoice create";a.contracts=b.contracts={"InvoiceRequest"};a.routes=b.routes={"POST /invoice"};a.events=b.events={"invoice.created"};}
 else if(id=="shared-primitive"){a.normalized_name="tenant key";b.normalized_name="workspace secret";a.contracts=b.contracts={"KeyMaterial"};a.routes=b.routes={"POST /key"};a.events=b.events={"key.rotated"};}
 else if(id=="local-specialization"){a.normalized_name="order policy";b.normalized_name="workflow policy";a.contracts=b.contracts={"Policy"};a.ast_fingerprints=b.ast_fingerprints={"policy"};a.routes={"POST /order"};b.routes={"POST /workflow"};}
 else if(id=="same-name-different-domain"){a.normalized_name=b.normalized_name="status check";b.bounded_context="identity";}
 else if(id=="semantic-only"){a.normalized_name="alpha";b.normalized_name="beta";}
 else throw std::invalid_argument("unknown truth case");
 Fixture out{a,b,{}};if(id=="semantic-only")out.hints.push_back({capability_reference_id(a),capability_reference_id(b),.99});return out;
}
json score(const std::vector<json>&cases,const CandidateGenerationConfig&config,const bool semantic){int tp=0,fp=0,fn=0;json positives=json::array(),negatives=json::array();for(const auto& item:cases){auto f=fixture(item.at("id"));auto candidates=CapabilityCandidateGenerator(config).generate({f.left,f.right},semantic?f.hints:std::vector<SemanticCandidateHint>{});const auto actual=candidates.empty()?std::string("insufficient_evidence"):std::string(to_string(candidates.front().classification)),expected=item.at("expected").get<std::string>();if(actual==expected)++tp;else {++fp;++fn;positives.push_back({{"id",item.at("id")},{"expected",expected},{"actual",actual}});negatives.push_back({{"id",item.at("id")},{"expected",expected},{"actual",actual}});}}const double precision=tp+fp?double(tp)/(tp+fp):0,recall=tp+fn?double(tp)/(tp+fn):0;return {{"tp",tp},{"fp",fp},{"fn",fn},{"precision_at_1",precision},{"recall_at_1",recall},{"false_positives",positives},{"false_negatives",negatives}};}
int main(int argc,char**argv){if(argc!=2)return 2;std::ifstream in(argv[1]);json truth;in>>truth;if(truth.value("schema_version","")!="axon/portfolio-candidate-truth-set/v1"||!truth.contains("cases")||truth["cases"].size()!=6)return 2;CandidateGenerationConfig name;name.weights={{CandidateSignal::name,1}};CandidateGenerationConfig semantic;semantic.weights={{CandidateSignal::semantic,1}};std::cout<<json{{"schema_version","axon/portfolio-eval/v1"},{"truth_cases",truth["cases"].size()},{"baselines",{{"name_only",score(truth["cases"],name,false)},{"semantic_only",score(truth["cases"],semantic,true)},{"multi_signal",score(truth["cases"],{},true)}}}}.dump()<<"\n";}
