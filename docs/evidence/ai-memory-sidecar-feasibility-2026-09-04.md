# ai-memory como sidecar opcional do Axon — análise de viabilidade (revisada)

**Date:** 2026-09-04 (revisão da versão original do mesmo dia)

**Axon baseline:** `fbcbe5e812b90a7f45e8f063d3c70653530a4540` (`origin/main`, após `git pull --ff-only`;
substitui `f776d73`, que era o HEAD local usado na primeira versão desta análise)

**ai-memory baseline:** tag `v2.0.1`, HEAD `38ded9a` (inalterado)

**Decisão:** ainda não adotar ai-memory como dependência de runtime do Axon — mas a razão muda, e uma parte
do que a versão anterior descartava como "fora de escopo" agora tem um lugar arquiteturalmente coerente.

**Architectural decision status:** Proposta; owner gate necessário antes de qualquer implementação.

---

## 0. Nota de correção — o que estava errado na versão anterior

A versão anterior deste documento (mesmo dia, mesmo arquivo) foi escrita contra o **HEAD local desatualizado**
do Axon (`f776d73`), que estava **seis commits/uma feature inteira atrás** de `origin/main`. Um `git pull
--ff-only` trouxe: PR #96 (`feature/portfolio-capability-intelligence`, G0-G16 completo), PR #97 (closeout),
PR #98 (`feature/portfolio-graph-explorer`) e PR #102 (`feature/portfolio-atlas-oidc`) — 136 arquivos, +16.163
linhas. Isso invalida três afirmações específicas da análise anterior, não apenas a moldura geral:

1. **"Axon é ferramenta local por-repositório, operador único"** — verdadeiro para a camada de código/diálogo
   (`.axon/index.duckdb` por repo continua sendo a autoridade local), **falso para a nova camada de
   Portfolio**: existe agora um **perfil de servidor** real, com PostgreSQL 16 governado como projeção central
   durável, Qdrant e FalkorDB como read-models opcionais, e autenticação Keycloak OIDC (RS256, fail-closed).
   Este era exatamente o objeção central que o usuário contestou, e ele estava certo.
2. **"Não confirmado que Keycloak/OIDC seja parte do produto Axon core"** — estava correto para o código lido
   em `f776d73`, mas **incorreto como estado atual**: `src/portfolio/infrastructure/http/keycloak_oidc_auth.cpp`
   existe, faz verificação RS256 completa (JWK, audiência, assinatura) e tem teste dedicado
   (`KeycloakRs256AuthenticationFailsClosed`, em `tests/integration/test_portfolio_reconcile.cpp`) confirmando
   comportamento fail-closed. Isso bate com o sintoma real desta sessão ("API continua recusando requisições
   sem bearer") — a API está corretamente recusando, o problema é o fluxo PKCE do navegador travado por
   `crypto.subtle` exigir HTTPS, não ausência de auth no binário.
3. **"Sem autenticação HTTP nativa no core"** — precisa ser qualificada: a autenticação nativa existe e é real
   para a superfície de ingest/reconciliação remota do Portfolio (verificada por teste), e há sign-in PKCE no
   Atlas web (`feat(web): add Keycloak PKCE sign-in to capability atlas`, commit `4a42a4f`). Não foi
   verificado nesta revisão se **toda** rota HTTP básica (`/api/graph`, `/api/search` etc. do `axon serve
   --http` genérico, fora do Portfolio) passa pelo mesmo authenticator — isso continua sendo um item para
   confirmar, não para assumir em qualquer direção.

O restante do comparativo de capacidades de 2026-09-03 (`ai-memory-v2.0.1-vs-axon-capability-comparison-...md`)
não é afetado por esta correção — ele tratava do produto ai-memory e da camada de código/diálogo do Axon, que
não mudou. O que muda é especificamente a análise de viabilidade do sidecar, porque ela dependia da premissa
"Axon não opera infraestrutura de servidor multiusuário" — premissa que a própria entrega de Portfolio
Capability Intelligence (G0-G16, já mergeada) derruba.

## 1. A arquitetura real da camada de Portfolio, agora confirmada

`docs/dev/SHARED-INFRA.md` e o código em `src/portfolio/` (domain/application/infrastructure) confirmam um
**seam de provider role-aware** — exatamente o que o estudo MemPalace de 2026-08-30 havia proposto, e que a
primeira versão desta análise (equivocadamente) tratava como "ainda não implementado":

- `PortfolioStore` (`src/portfolio/application/portfolio_store.hpp`) é uma interface abstrata
  (`reidentify_repository_stream`, mutações de projeção, manutenção) com **duas implementações concretas**:
  `DuckdbPortfolioStore` (perfil local, sempre disponível, default) e `PostgresqlPortfolioStore`
  (`src/portfolio/infrastructure/postgresql/postgresql_portfolio_store.{hpp,cpp}`, 776 linhas, via `libpq`,
  compilado condicionalmente quando `libpq-dev` está presente).
- `pgvector_semantic_store.{hpp,cpp}` e `qdrant_semantic_store.{hpp,cpp}` são read-models de busca semântica
  opcionais; `falkordb_capability_graph.{hpp,cpp}` é um read-model de grafo opcional. Todos **fail-soft**: o
  closeout de G16 declara explicitamente "no provider is required for local-first operation".
  `docs/dev/SHARED-INFRA.md` deixa claro que o Postgres usado é o **serviço governado compartilhado
  PostgreSQL 16** (`platform-shared-dev`, coerente com a diretriz de infra compartilhada §3a do AGENTS.md do
  usuário) — não uma instância local do Axon. A única exceção documentada é um contêiner Postgres/pgvector
  efêmero, sem volume, sem rede compartilhada, só para os testes de integração G6.
- `keycloak_oidc_auth.{hpp,cpp}` implementa `KeycloakOidcAuthenticator` com verificação RS256 completa contra
  JWKS, checagem de audiência, e falha fechada em token inválido/JWK fraco — testado
  (`KeycloakRs256AuthenticationFailsClosed`, `RejectsWeakOrUnsafeJwkRsaParameters`).
- Escala: 136 arquivos, ~16 mil linhas adicionadas nesta única feature (G0-G16), com suíte própria de testes
  de unidade/integração/smoke (41 testes na evidência de fechamento G15) e CI cross-platform (Linux/macOS/
  Windows) verde.

Isso não é um protótipo — é uma entrega fechada (G16), revertível de forma limpa (`docs` documenta o commit
de rollback), com risco residual documentado e aceito pelo próprio processo de entrega do Axon.

## 2. O que isso muda na pergunta "faz sentido ai-memory como sidecar"

### 2.1 A objeção "desproporcional para operador único" cai — mas não vira "sim, adote"

A versão anterior argumentava que a superfície madura do ai-memory (auth multiusuário, RBAC, audit log, rate
limiting) era desproporcional porque o Axon era "uma ferramenta local por-repositório, um operador por vez".
Isso não é mais uma descrição precisa do Axon como um todo: a camada de Portfolio já é, por design, um serviço
central autenticado, consumido por múltiplos repositórios/agentes do portfólio, com sign-in humano via
Keycloak. A pergunta "o Axon precisa de auth robusta e multiusuário" já foi respondida **sim** pelo próprio
projeto — só que a resposta que ele deu foi construir a própria (Keycloak OIDC + Postgres governado), não
importar a do ai-memory.

### 2.2 A objeção técnica que realmente importa agora: engine de armazenamento incompatível com a governança que o Axon acabou de adotar

Este é o ponto mais forte contra o sidecar, e ele **fica mais forte**, não mais fraco, com a nova informação:
o ai-memory é **SQLite-only** — nenhuma das duas auditorias (2026-09-03) encontrou um provider Postgres no
código dele, e ele não tem o conceito de "perfil de servidor" com projeção central governada. O Axon acabou de
investir uma feature inteira (G0-G16) para **centralizar a camada de servidor no PostgreSQL 16 compartilhado
governado**, evitando exatamente o padrão de "banco stateful local por projeto" que a diretriz de infra
compartilhada do usuário proíbe (§3a do AGENTS.md). Trazer o ai-memory como sidecar reintroduziria precisamente
esse padrão — um SQLite próprio, fora da governança, ao lado de um sistema que acabou de sair dele. A objeção
não é mais "é demais para o que o Axon precisa", é **"contradiz a decisão de arquitetura que o Axon já tomou
esta semana"**.

### 2.3 A conclusão certa não é "importar ai-memory", é "estender o padrão que o Axon já construiu, para a camada de diálogo/memória"

O Axon já provou, na prática, o padrão `LocalStore` (DuckDB, default) + `ServerStore` opcional (PostgreSQL
governado) + read-models de busca semântica opcionais (pgvector/Qdrant) + auth Keycloak OIDC — para
**capacidades de código**. As lacunas reais que o ai-memory expõe na **camada de diálogo/memória** (decaimento,
consolidação, contradição, ver o comparativo de 2026-09-03 §3) podem seguir exatamente esse mesmo seam, em vez
de um produto externo:

- Um `DialogueStore` (nome ilustrativo) espelhando `PortfolioStore`: `DuckdbDialogueStore` local (o que já
  existe hoje em `src/core/dialogue.cpp`) + `PostgresqlDialogueStore` opcional reaproveitando a mesma conexão
  governada e o mesmo `PostgresqlPortfolioStore` como referência de implementação — não um novo componente de
  infraestrutura, uma nova classe C++ sobre infraestrutura já aprovada e já paga (o Postgres governado já está
  em uso).
- Auth: reaproveitar `KeycloakOidcAuthenticator` tal como está — não há motivo para um segundo modelo de
  autenticação para diálogo quando o Portfolio já resolveu isso.
- Busca semântica sobre observações/turns em modo servidor: reaproveitar `pgvector_semantic_store` (já existe,
  já compila condicionalmente com `libpq-dev`) em vez do cosine exaustivo em Rust do ai-memory — o Axon já tem
  a peça certa para isso.

Isso muda a recomendação das três absorções nativas propostas na versão original (decaimento zero-LLM, typed
edges + lint estrutural, consolidação via LLM local opt-in): elas continuam válidas como descrito, mas agora
têm um **lugar concreto e não-hipotético** para viver em modo servidor — o mesmo seam de provider do Portfolio,
não uma extensão isolada do DuckDB local.

## 3. O que continua não fazendo sentido, e por quê (revalidado, não descartado)

- **Adotar o binário/produto ai-memory como processo dependente** continua desaconselhado — agora por
  incompatibilidade de armazenamento com a governança que o Axon já adotou (§2.2), não por desproporção de
  escopo.
- **Duas fontes de verdade de sessão/handoff** (Axon `dialogue.cpp` vs. ai-memory `sessions`) continua sendo
  um problema real e não resolvido por nenhuma quantidade de infraestrutura de servidor — é um problema de
  modelo de dado, não de rede/auth.
- **OKF/wiki markdown, workstreams cross-harness** continuam fora do domínio do Axon — nenhuma dessas duas
  capacidades tem relação com a existência ou não de um perfil de servidor Postgres.

## 4. Recomendação revisada

1. **Não** adotar ai-memory como dependência de runtime — mantido, mas pela razão correta (§2.2), não pela
   razão original (§2.1, retratada).
2. **Owner gate para:** desenhar `DialogueStore` como extensão do seam `PortfolioStore` já existente, com
   `PostgresqlDialogueStore` reaproveitando a infraestrutura Postgres/pgvector/Keycloak já entregue em G0-G16,
   em vez de tratar decaimento/consolidação como um apêndice isolado do DuckDB local. Isto é uma revisão de
   escopo do item 2 da recomendação original — mesmo objetivo, caminho de implementação diferente e mais barato
   porque reaproveita trabalho já pago.
3. **Confirmar, antes de mexer em qualquer coisa nova**, se `KeycloakOidcAuthenticator` já cobre as rotas
   genéricas de `axon serve --http` (`/api/graph`, `/api/search` etc.) fora do Portfolio, ou só a superfície de
   reconciliação remota do Portfolio e o sign-in do Atlas web. Isso decide se o gap de HTTPS/PKCE ainda em
   aberto nesta sessão (`_knowledge/projects/axon/state.md`) é só um problema de borda de rede, ou também um
   problema de cobertura de auth em rotas que hoje ficam de fora do Portfolio.

## 5. E se portarmos (reimplementarmos nativamente, não dependermos) o conjunto completo de capacidades de
   memória agêntica do ai-memory — auth multiusuário, RBAC, API keys, admin API de 40 endpoints, decaimento,
   tiers, consolidação, `as_of`, workstreams?

Pergunta levantada explicitamente pelo owner em 2026-09-04, distinta de §2.3 (que propõe portar só o
necessário para fechar as lacunas confirmadas, reaproveitando o seam já pago). Aqui a pergunta é sobre portar
**o produto inteiro**, nativamente, em C++. Resposta: **não faz sentido como bloco único — faz sentido como
subconjunto priorizado**, pelas razões abaixo, não por relutância genérica a escopo grande.

### 5.1 A escala não é retórica, é um custo real

Antes do Portfolio, o core do Axon tinha ~14,9K LOC de produção + ~3,5K de testes. A entrega de Portfolio
(G0-G16) sozinha adicionou ~16,2K linhas. O core do Axon hoje gira em torno de ~31K linhas totais. O ai-memory
tem ~189K linhas de produção (~217K com testes) — construídas ao longo de 137+ commits só entre duas versões
menores (v1.35.0→v2.0.1), com migrações de schema até `V99`. Portar "tudo" não é adicionar uma feature ao
Axon — é multiplicar o tamanho do código-fonte do Axon por um fator de 6-7x, mantido pela mesma operação que
hoje já trata build paralelo com `-j2` como precaução deliberada neste host (§3f do AGENTS.md do usuário). Isso
é um argumento de custo de manutenção contínua, não de esforço de implementação único.

### 5.2 A pergunta real por trás de "portar tudo" é uma pergunta de identidade de produto, não só técnica

O Axon hoje é "motor de inteligência de código, com camada de diálogo auxiliar, zero-LLM por padrão, zero
dependência de nuvem". Portar auth multiusuário completo + RBAC + API keys + admin API de 40 endpoints +
tiers de memória + workstreams cross-harness não é "adicionar memória agêntica ao Axon" — é fazer o Axon
**se tornar também** um produto de memória de conhecimento geral, concorrendo de fato com o ai-memory, além de
continuar sendo motor de código. Produtos que tentam ser as duas coisas ao mesmo tempo tendem a diluir foco: o
próprio ai-memory não tenta entender estrutura de código, e essa é uma escolha deliberada dele, não uma
lacuna. Isso não é uma objeção técnica — é uma decisão de escopo que só o owner pode tomar conscientemente, não
algo que deveria emergir de portar features uma a uma.

### 5.3 O que portar — dividido por proporção custo/valor, não por "sim ou não" binário

| Camada | Capacidades | Fit | Por quê |
|---|---|---|---|
| **Tier 1 — reaproveita infraestrutura já paga (G0-G16), recomendado com owner gate** | Auth por identidade real (Keycloak, já existe) estendida a `observations`/`turns`/`sessions` com atribuição por usuário; API keys de serviço para agentes/CI reaproveitando a mesma verificação RS256; decaimento/GC zero-LLM; typed edges + lint de contradição estrutural; busca semântica sobre diálogo via `pgvector_semantic_store` (já existe); consolidação opt-in via LLM local (exige wrapper de completion novo sobre o `llama.cpp` já vendorizado — trabalho real, não grátis) | Alto valor, custo incremental — cada peça reaproveita algo que o Axon já construiu e já paga operacionalmente | Não é "importar o ai-memory", é "terminar de construir o que o seam `PortfolioStore` já provou" |
| **Tier 2 — valor real, mas decisão de escopo própria, não decorrência automática do Tier 1** | Audit log de quem fez o quê (relevante assim que há multiusuário real); rate limiting nativo na superfície HTTP; `as_of`/bi-temporal (só faz sentido depois que decaimento+typed edges existirem); backup/restore da camada de diálogo/portfolio (gap real do Axon hoje, independente do ai-memory) | Justificável, mas cada um merece seu próprio gate — não é consequência obrigatória de ter Postgres+Keycloak | Adicionar porque "já que temos auth, vamos ter audit log" é como escopo cresce sem decisão explícita |
| **Tier 3 — não portar; é outro produto, não uma lacuna do Axon** | Formato OKF / wiki markdown navegável por humanos; workstreams cross-harness (`run`/`resume`/`continue` de execução de agente); a maior parte do admin API de 40 endpoints (CRUD de páginas, merge/move de workspace, export-okf) — específicos do modelo de dado "wiki" que o Axon não tem e não deveria construir só para paridade | Fora do domínio, custo alto, sem sinergia com o que o Axon já é | Portar isso é começar um produto novo dentro do Axon, não fechar uma lacuna |

### 5.4 Recomendação

Portar o Tier 1, com owner gate item a item (mesma disciplina de §4). Tratar Tier 2 como backlog separado,
cada item justificado por si — não "vem de graça" por já termos Postgres/Keycloak. Não portar o Tier 3: se o
owner quiser as capacidades dele (wiki humana, workstreams), a resposta certa é co-instalar o próprio
ai-memory ao lado do Axon para isso — qualquer harness já pode ter `mcp__axon__*` e `mcp__ai-memory__*`
registrados simultaneamente, hoje, sem nenhum trabalho de engenharia — não reconstruir um segundo produto de
conhecimento dentro do Axon.

## 6. E se, em vez de portar capacidades para dentro do Axon (§5), portarmos o ai-memory inteiro como produto
   **irmão** — mesma família/harness nativo do Axon, binário separado, "Axon + Axon-Memory (port)"?

Pergunta levantada pelo owner em 2026-09-04, como alternativa às duas anteriores. É a forma organizacionalmente
mais correta das três, e resolve objeções reais de §2 e §5 — mas não resolve a mais cara delas, e isso precisa
ficar dito sem meio-termo.

### 6.1 O que "irmão" resolve de verdade

- **Diluição de identidade (§5.2)**: resolvida. O Axon continua sendo só motor de código + diálogo auxiliar; a
  identidade de "memória agêntica geral" vive num produto próprio, não dentro do binário do Axon.
- **Incompatibilidade de storage (§2.2)**: resolvida, e de forma melhor que qualquer opção anterior — como é
  uma reimplementação nativa, não uma migração de código Rust→C++, o port nasce **já** sobre o seam que o Axon
  provou em G0-G16 (DuckDB local + PostgreSQL 16 governado + Keycloak OIDC + `llama.cpp` vendorizado), em vez
  de precisar reproduzir o SQLite-only do ai-memory original.
- **Consistência de distribuição**: um produto irmão pode reaproveitar literalmente a mesma esteira — mesmo
  CMake/C++20, mesmo padrão de binário único relocável, mesmo instalador (`install.sh`/`install.ps1`), mesmo
  padrão de release repo separado (`HideakiSolutions/axon-releases`), mesmo mecanismo de instalação de hooks.
  A forma mais coerente disso tecnicamente é extrair uma **biblioteca interna compartilhada** ("axon-kernel":
  wrapper DuckDB, `PortfolioStore`/Postgres provider, `KeycloakOidcAuthenticator`, `EmbeddingModel`/llama.cpp,
  CCR) da qual **os dois binários** dependem — não duas cópias divergentes da mesma infraestrutura.
- **Composição no harness do agente**: dois MCP servers nativos, cada um com identidade clara
  (`mcp__axon__*` para código, algo como `mcp__axon-memory__*` para memória), plausivelmente rodando os dois
  já hoje via `install.sh` de cada um — igual à opção de co-instalar o ai-memory original, só que nativo e
  arquiteturalmente alinhado.

### 6.2 O que "irmão" não resolve: o port continua sendo construir um produto do zero, sozinho, contra um alvo em movimento

Isto é o ponto central, e vale repetir sem suavizar: virar produto irmão muda **onde** o código mora, não
**quanto** código precisa ser escrito. Portar "o ai-memory inteiro" continua sendo reescrever, do zero, em C++,
um produto que hoje tem ~189-217K linhas Rust, construído ao longo de 137+ commits só entre v1.35.0→v2.0.1 —
ou seja, um ritmo de entrega de um time com uso de produção real, não de um projeto interno mantido por uma
pessoa. Três consequências concretas, não hipotéticas:

- **O port nunca alcança paridade estática** — ao mesmo tempo em que o port avança, o ai-memory upstream
  continua lançando (o intervalo v1.35.0→v2.0.1 aconteceu em ~1 mês corrido). Cada mês de atraso no port é
  mais superfície nova a replicar, indefinidamente, a menos que o escopo do port seja deliberadamente **menor**
  que o produto original (ver §6.3).
- **Segurança é o pior lugar para reescrever do zero.** Auth multiusuário, RBAC, verificação JWT/JWK, rate
  limiting — o CHANGELOG do ai-memory mostra correções reais e específicas nessa superfície (rejeição de
  escrita sem escopo #564, escrita atômica do modo de captura para não reverter privacidade por padrão,
  rejeição de parâmetros RSA fracos em JWK). Um port começa com **nenhuma** dessas lições — reintroduzir essa
  classe de bug em código novo é o risco mais caro desta proposta, não o mais barato.
- **Custo de manutenção dobra, permanentemente**, não só durante a construção: duas superfícies de auth, dois
  MCPs, dois modelos de release a testar/homologar/atualizar a cada mudança de infra governada (ex.: rotação
  de credencial do Postgres compartilhado, mudança de realm Keycloak).

### 6.3 Recomendação

"Irmão" é a forma certa **se e somente se** o escopo do port for o Tier 1 (+ eventualmente Tier 2) de §5.3 —
exatamente as mesmas capacidades já recomendadas, agora empacotadas como binário próprio em vez de
absorvidas dentro do Axon — e não uma tentativa de replicar o produto ai-memory inteiro (Tier 3 incluído:
OKF/wiki, workstreams, admin API completo). Chamar isso de "portar o ai-memory" é o enquadramento errado,
mesmo que o resultado final pareça superficialmente parecido; o enquadramento certo é "construir um produto de
memória nativo, pequeno, na família Axon, cobrindo só as lacunas já identificadas e comprovadamente valiosas" —
que é uma reafirmação de §5, com o benefício adicional (real) de identidade de produto limpa, e sem o benefício
ilusório de "temos tudo que o ai-memory tem", que nenhuma quantidade de reorganização de repositório entrega de
graça.
