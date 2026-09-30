# Typed contract pilot corpus

`corpus-v1.json` labels producer/consumer pairs across three repositories. Each
surface has five positive and at least two negative pairs. HTTP labels include
method and route; OpenAPI labels include `operationId`; RPC labels include
package, service and method; topic labels include protocol, broker and address.
The two dynamic calls are explicitly unknown. Files under each repository are
fixtures, not production services.

The standalone evaluator uses the production extractor and resolver:

```sh
XDG_RUNTIME_DIR=/tmp/axon-heavy-validation heavy-run --mem 3G -- \
  c++ -std=c++20 -O0 -D_GLIBCXX_USE_CXX11_ABI=0 \
  -I src -I third_party/duckdb/lib -I third_party/nlohmann-json/single_include \
  evals/contracts/contract_eval.cpp src/core/contracts.cpp \
  -L third_party/duckdb/lib -lduckdb \
  -Wl,-rpath,"$PWD/third_party/duckdb/lib" \
  -o /tmp/axon-contract-eval
/tmp/axon-contract-eval evals/contracts evals/contracts/corpus-v1.json
```

The evaluator reports TP/FP/FN/TN and unknown by surface, every labeled case,
and all confirmed links absent from the positive labels. Its gate requires
precision at least 95%, recall at least 85%, five positive and two negative
labels per surface, and zero unlabelled links. This small static corpus is a
pilot gate only; broad production use also needs repository-scale evaluation.

Dynamic references and contracts with unresolved servers or channels remain
unknown. A missing confirmed link does not prove that no consumer exists.
