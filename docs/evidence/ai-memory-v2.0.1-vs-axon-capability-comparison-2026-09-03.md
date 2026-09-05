# ai-memory v2.0.1 vs Axon — comparativo honesto de capacidades

**Date:** 2026-09-03

**Axon baseline:** `f776d73a95628e14328ed4b9f66d89318c437832` (`main`, release instalada v1.3.1 build `abcaf33`)

**ai-memory baseline:** tag `v2.0.1`, HEAD `38ded9a` (`akitaonrails/ai-memory`)

**Sources:** `/opt/hideakisolutions/axon` (via `mcp__axon__*` + leitura direta) e `/opt/references/ai-memory`
(leitura direta de código-fonte, sem indexação Axon — repositório de referência externo)

**Natureza deste documento:** comparativo de capacidades, não uma decisão de absorção/arquitetura. Não há
proposta de mudança de escopo do Axon aqui — apenas inventário factual e leitura honesta de equivalência,
cobertura e divergência.

---

## 1. Conclusão executiva

Axon e ai-memory **não são o mesmo tipo de produto**, apesar de ambos serem infraestrutura de memória/contexto
para agentes de IA consumida via MCP. Comparar "capacidade por capacidade" só é honesto se a diferença de
propósito central for dita primeiro:

- **Axon é um motor de inteligência de código**: grafo estrutural (DuckDB) de 18 linguagens via tree-sitter,
  com recuperação híbrida (capsule assembly + busca semântica local) cujo objetivo é responder "o que este
  código faz, o que ele afeta, o que chama o quê" com o menor custo de tokens possível. A camada de
  diálogo/sessão existe, mas é auxiliar ao problema de código.
- **ai-memory é um sistema de memória de conhecimento para agentes**: uma wiki markdown versionada (OKF v0.2)
  com um pipeline de retenção/decaimento, consolidação, lint de contradições e — desde 2.0.0 — recuperação
  híbrida madura (FTS5 + entidade + grafo RRF + vetor RRF), cujo objetivo é responder "o que sabemos, o que
  decidimos, o que este agente deveria lembrar entre sessões e entre harnesses".

Onde os dois **de fato competem** é numa fatia relativamente estreita mas real: sessão/diálogo, handoffs entre
agentes, hooks de integração com harnesses, e exposição via MCP. Nessa fatia, ai-memory é sensivelmente mais
maduro (retenção com decaimento, consolidação, lint de contradição, segurança nativa multiusuário) e Axon é
mais leve mas também mais "puro" (zero chamada de LLM em qualquer caminho, incluindo diálogo — nem para
digest de sessão). Fora dessa fatia, cada um cobre um espaço que o outro simplesmente não tenta: grafo de
código estrutural e portfólio cross-repo (só Axon); tiers de memória com decaimento, workstreams cross-harness
com resume, e segurança/multi-tenant nativa (só ai-memory).

**Escala:** ai-memory tem ~189K LOC de produção (~217K incluindo testes) contra ~14.9K (~3.5K de testes) do
Axon — uma diferença de ordem de grandeza que reflete tanto o escopo mais amplo quanto uma base de usuários e
ciclo de release mais longos (137 commits só entre v1.35.0→v2.0.1, migrações de schema até `V99`).

## 2. Método e limitações

Dois agentes independentes (forks desta sessão, mesmo modelo, sem troca de contexto entre si) auditaram cada
sistema separadamente contra as mesmas 11 categorias, com instrução explícita de citar arquivo/linha como
evidência e declarar "não confirmado" em vez de inferir. A auditoria do Axon usou as próprias tools
`mcp__axon__*` (fonte primária: `get_overview`, `capability_list`) complementada por leitura de código; a do
ai-memory usou leitura direta de código-fonte (Grep/Read), pois o repositório não tem índice Axon.

Limitações reais, não escondidas:

- A auditoria do Axon **não conseguiu confirmar** a integração Atlas Portfolio + OIDC/Keycloak mencionada na
  continuidade desta sessão dentro do código core indexado (`src/`) — evidência aponta para um projeto
  downstream (`jinx-ai`), não para o binário `axon` em si. Isso é relevante para a seção de segurança abaixo:
  **o core do Axon não tem autenticação HTTP nativa confirmada**, e a experiência OIDC vem de fora.
- Nenhuma das duas tools de retrieval híbrida foi exercitada com carga real neste comparativo — os números
  reportados (ex.: hit@5 do ai-memory) vêm do próprio CHANGELOG do projeto, não de um benchmark rodado aqui.
- O catálogo de capacidades do Axon (`capability_list`/`capability_search`/`capability_drift`) estava
  **degradado** (`degraded=true`, 0 capacidades) no momento da auditoria — a existência do mecanismo está
  confirmada pelo CLI e pelos schemas MCP, não a qualidade do output com dados populados.
- Não houve auditoria de segurança adversarial de nenhum dos dois sistemas — apenas leitura do que está
  implementado, não teste de bypass.

## 3. Matriz de equivalência

Legenda: **✅ equivalente** (ambos resolvem o mesmo problema de forma comparável) · **◐ parcial** (ambos têm
algo na área, mas com forma/profundidade distintas) · **Axon-only** · **ai-memory-only** · **N/A** (fora do
domínio de um dos dois por design, não por lacuna)

| Área | Axon | ai-memory | Veredito |
|---|---|---|---|
| Grafo estrutural de código (impacto, callers, testes, rotas) | `get_impact_graph`, `get_callers`, `get_tests_for`, `route_map`, `api_impact`, `detect_changes` | inexistente | **Axon-only** |
| Skeletonização / assinaturas sem corpo | `get_skeleton` (tree-sitter, 18 linguagens) | inexistente | **Axon-only** |
| Context capsule com orçamento de tokens + cache | `get_context_capsule` (BFS + skeleton + cache por epoch) | inexistente (ai-memory não monta "capsule" de código) | **Axon-only** |
| Busca semântica sobre memória/observações | `search_memory` (RRF híbrido sobre `observations`) | `memory_query` (FTS5+entidade+grafo RRF+vetor RRF) | **◐ parcial** — ai-memory é mais rico (multi-stream + `explain` + `as_of`), Axon é mais barato (sem chat-LLM em nenhum ponto) |
| Time-travel / consulta histórica | inexistente (não há versionamento de conteúdo, só `capsule_cache` por epoch de build) | `as_of` ISO-8601 sobre índice de entidades bi-temporal-lite | **ai-memory-only** |
| Sessões/threads/turns | `thread_*`, `session_*`, `turn_*`, `dialogue_context`, `anchor_link` | sessões + `memory_read_session_observations` + consolidação | **◐ parcial** — Axon modela thread→session→turn explicitamente com auto-anchor; ai-memory trata sessão como unidade de captura que alimenta a wiki, sem "turns" navegáveis como entidade de 1ª classe |
| Handoffs entre agentes | `handoff_create/claim/complete/cancel/get/list` (típado, idempotente, project-scoped) | `memory_handoff_begin/accept/cancel` + `handoffs --expire-all` admin | **✅ equivalente** — mesmo conceito, Axon tem mais estados (claim/complete separados), ai-memory tem faxina administrativa em lote |
| Consolidação/digest de sessão | `session_end(compute_digest=true)` — **determinístico, zero-LLM** (ADF) | `memory_consolidate` — **LLM-driven**, single/multi-page | **◐ parcial** — mesmo objetivo, filosofias opostas: Axon nunca usa LLM para isso, ai-memory usa LLM quando configurado (e tem fallback zero-LLM só para captura/busca, não para consolidação) |
| Decaimento / esquecimento / retenção | inexistente (confirmado: sem TTL, sem salience, sem sweep automático) | `memory_forget_sweep` (TTL hard-delete + decay por fórmula salience/idade/acesso), `memory_feedback` | **ai-memory-only** |
| Tiers de memória (working/episodic/semantic/procedural) | inexistente | 4 tiers explícitos (`page.rs`), inspirado em "agentmemory" | **ai-memory-only** |
| Lint de conteúdo / contradições | inexistente (Axon audita *capacidades de código*, não conteúdo de memória) | `memory_lint` (stale, duplicado, refs quebradas, contradição) + typed edges `causes/fixes/contradicts` | **ai-memory-only** |
| Coordenação cross-repo / portfólio | `group_impact`, `portfolio_sync/status`, catálogo de capacidades observadas vs. declaradas (`capability_*`) | inexistente (workspaces/projects são namespaces lógicos num único deployment, não um grafo de dependência entre repositórios) | **Axon-only** |
| Execução/retomada de agente cross-harness | fallback CLI para harnesses sem MCP (`axon capsule/skeleton/filter`) — não gerencia execução do agente em si | `run`/`show`/`continue`/`resume`/`workstreams` — lança e retoma o harness dentro de um workstream rastreado, suporta >10 harnesses (Claude Code, Codex, OpenCode, Pi, Crush, Kimi, Command Code, Kiro, OMP, Grok Build, Antigravity) | **ai-memory-only** |
| Auto-melhoria contínua da base de conhecimento | inexistente | `memory_auto_improve` (por sessão) + "experience pass" cross-sessão opcional (2.0.0) | **ai-memory-only** |
| Embeddings — geração | local, llama.cpp, nomic-embed-text-v1.5, 768-dim, obrigatório (parte do pipeline estrutural) | local (candle, all-MiniLM-L6-v2, 384-dim) OU 4 providers externos (OpenAI/Voyage/Gemini/compat), opcional (`none` desliga) | **◐ parcial** — Axon: um único caminho local sempre ligado; ai-memory: hybrid-by-default mas configurável, mais opções de provider |
| Índice vetorial (ANN) | não confirmado no código lido (campo `embedding FLOAT[768]` em `symbols`, mecanismo de busca não auditado em profundidade) | **confirmado sem ANN** — cosine exaustivo em Rust puro sobre vetores armazenados | **não comparável com confiança** — ambos podem ser exaustivos na prática, mas isso não foi confirmado para o Axon |
| Chamada a LLM de terceiros (chat) | **nenhuma, em nenhum caminho** (nem para digest, nem para consolidação) | opcional, 8 providers (Anthropic/OpenAI/OpenAI OAuth/Copilot/Gemini/OpenCode Zen/compat/OIDC device), com `reasoning_effort` tipado por provider; caminho zero-LLM cobre captura/busca/handoffs mas não consolidação/auto-improve/explore | **divergência de filosofia**, não uma lacuna — Axon nunca depende de LLM externo; ai-memory tem "zero-LLM por padrão" mas LLM é parte real do produto quando configurado |
| MCP tools expostas | ~39 (código+grafo+diálogo+capability+portfolio, contadas nesta sessão) | 18 (`memory_*`) | **ai-memory mais enxuto na superfície MCP**, Axon mais amplo (reflete domínios distintos, não "mais capacidade") |
| CLI | ~20 comandos + subcomandos `portfolio`/`capability` | ~40 subcomandos (lifecycle, admin, user/api-key, backup/restore, reorg) | **ai-memory tem CLI de operação/administração muito mais extensa** |
| HTTP API própria | confirmada (`/api/graph`, `/api/search`, `/api/capsule`, etc.) + UI de grafo (`axon web`) + **LSP server** (`axon lsp`) | confirmada, 40 endpoints `/admin/*` + wiki humana navegável + SPA | **◐ parcial** — superfícies não comparáveis 1:1 (Axon expõe grafo de código + LSP; ai-memory expõe administração + wiki humana) |
| Autenticação HTTP nativa | **não confirmada no core** (peer-lock local via token é lock de escrita, não auth de usuário; `http_server.cpp` não mostrou checagem de bearer) | **confirmada e madura**: bind não-loopback sem auth falha fechado por padrão, bearer token, cookie `HttpOnly`+CSRF para humanos, API keys `aim_`, OIDC device auth, rate limiting por token bucket | **ai-memory-only** — divergência de segurança relevante, ver §4 |
| Multi-tenant / RBAC real | não aplicável por design (ferramenta local por-repositório, um operador por vez, peer-proxy só para concorrência do mesmo operador) | `ai-memory user`/`api-key`, atribuição por pessoa, audit log nativo ("não é tier pago") | **ai-memory-only**, e é diferença de propósito, não só de maturidade |
| Isolamento de escrita concorrente no mesmo store | peer-ownership: primeiro processo vira *owner*, demais fazem proxy HTTP loopback | `.serve.lock` exclusivo + writer actor único (canal mpsc) | **✅ equivalente em intenção** — mecanismos diferentes (proxy vs. lock+fila), mesma garantia de escritor único |
| Formato em disco / rebuildable from source-of-truth | índice DuckDB é derivado do **código-fonte** (re-parseável) | índice SQLite é derivado dos **arquivos markdown da wiki** (`README.md`: "the database is a derived index that can always be rebuilt from the files") | **✅ equivalente em princípio**, fonte de verdade diferente por natureza do domínio |
| Backup/migração de schema | não documentado como fluxo de produto (sem `axon backup`/`restore` confirmado) | `backup`/`restore`, migração OKF backup-gated e verificada, recusa de binário mais antigo abrir dado migrado por versão mais nova | **ai-memory-only** |
| Distribuição/instaladores | repo de release separado, tarballs Linux/macOS, Homebrew, VS Code extension, instalador Windows (`install.ps1`) | Docker (multi-arch), pacotes Arch/AUR com systemd, launchd macOS, Windows nativo experimental + wrapper Docker | **◐ parcial** — ambos multiplataforma com abordagens distintas; ai-memory tem mais opções de "serviço gerenciado" nativas (systemd/launchd empacotados), Axon depende de integração manual (o `jinx-axon-http.service` observado é ad-hoc do consumidor, não parte do produto) |

## 4. Divergências que importam (não só listar, avaliar)

**Segurança é a divergência mais séria, não a mais óbvia.** O comparativo confirma algo que já era um
sintoma vivido nesta mesma sessão (continuidade: "a API continua recusando requisições sem bearer... login
web ainda não pode iniciar"): o Axon core não tem uma história de autenticação HTTP própria. A exposição
`jinx-axon-http` em `0.0.0.0:7070` depende inteiramente de uma camada externa (Keycloak Shared, ainda em
config) para não ser um endpoint aberto. O ai-memory resolve esse mesmo problema — expor um servidor de
memória para múltiplos clientes/harnesses — com um modelo nativo fail-closed (bind não-loopback sem auth é
recusado por padrão) e bearer/OIDC/rate-limit como parte do produto, não como responsabilidade do operador.
Isso não é um detalhe: é a diferença entre "seguro por padrão" e "seguro se o operador lembrar de configurar
por fora" — e o próprio histórico desta sessão do Axon é evidência viva do segundo caso.

**Zero-LLM é um princípio de design do Axon, não uma opção — e isso é uma força real, não só uma limitação.**
Toda a camada estrutural e de diálogo do Axon (digest de sessão, capsule, skeleton, grafo) é determinística.
Isso dá reprodutibilidade e custo previsível que o ai-memory não pode prometer na mesma extensão: mesmo com
caminho zero-LLM real para captura/busca/handoffs, o valor mais citado do ai-memory 2.0 (hybrid retrieval,
consolidação, auto-improve, explore) é opcionalmente mas materialmente dependente de LLM externo. Um
comparativo honesto não deve tratar isso como "ai-memory tem mais recursos, logo é melhor" — é uma escolha de
trade-off que cada produto fez deliberadamente para seu domínio.

**Decaimento é uma lacuna real do Axon, não uma escolha de design documentada.** A auditoria não encontrou
nenhum mecanismo de TTL, salience ou sweep no Axon — nem para `observations`, nem para sessões antigas. Isso é
coerente com o domínio (código não "expira" da mesma forma que uma observação de conversa), mas
`save_observation`/`search_memory` do Axon acumula indefinidamente sem um mecanismo equivalente ao
`memory_forget_sweep`. Se o volume de observações crescer sem bound, isso é uma lacuna operacional real, não
apenas uma diferença filosófica.

**Cross-repo é uma força real e não-trivial do Axon que o ai-memory não tenta.** `group_impact` e o catálogo
de capacidades (`capability_*`) resolvem um problema que ai-memory não endereça: "este símbolo/módulo é
consumido por quais outros repositórios do portfólio, e existe uma capacidade duplicada em outro lugar do
fleet". É um problema de engenharia de portfólio multi-repo, ai-memory é single-deployment (workspaces lógicos
dentro de um mesmo store, não um grafo de repositórios). Vale registrar, porém, que esse subsistema estava
**degradado (catálogo vazio)** no momento desta auditoria — a capacidade existe no código mas não estava
demonstrada com dados reais.

**Workstreams (ai-memory) não têm equivalente no Axon, e resolvem um problema adjacente ao handoff.** Handoff
é "passar contexto para o próximo agente decidir o que fazer"; workstream é "lançar e retomar a mesma
execução, no mesmo harness ou em outro, sem perder o fio". São complementares, não substitutos — o Axon cobre
bem o primeiro (handoffs típados com claim/complete) e não tem nada para o segundo.

## 5. O que não é comparável com confiança

- **Qualidade de ranking do retrieval híbrido.** Nenhum dos dois foi benchmarcado nesta sessão. O número
  hit@5 0.779 do ai-memory vem do próprio changelog do projeto (LongMemEval-S), não de uma medição
  independente; o Axon não publica benchmark equivalente de qualidade de busca.
- **Existência de índice ANN no Axon.** A auditoria não confirmou como a busca sobre `symbols.embedding
  FLOAT[768]` é executada (exaustiva vs. indexada) — informação necessária para julgar se a comparação com o
  "cosine exaustivo em Rust puro" do ai-memory é uma equivalência real ou não.
- **Postura de segurança sob adversário real.** Ambos os levantamentos leram código, nenhum tentou explorar
  bypass. "ai-memory tem mais mecanismos de auth" é diferente de "ai-memory é mais seguro na prática" — isso
  exigiria revisão de segurança dedicada, não uma auditoria de capacidades.

## 6. Resumo em uma frase por sistema

- **Axon** é a ferramenta certa quando a pergunta é "o que este código faz e o que quebra se eu mudar isto" —
  e cobre diálogo/handoff como capacidade auxiliar, deliberadamente sem LLM e sem decaimento.
- **ai-memory** é a ferramenta certa quando a pergunta é "o que este agente (ou fleet de agentes/harnesses)
  deveria lembrar entre sessões, com segurança e higiene de conteúdo nativas" — e não tenta, em nenhum ponto,
  entender a estrutura do código em si.

Não há sobreposição de substituição direta a propor aqui; a fatia onde competem (sessão/handoff/hooks) é
pequena frente ao que cada um faz que o outro não faz.
