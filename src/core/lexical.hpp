#pragma once
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace axon {

inline std::string code_without_comments(const std::string& source, bool hash_comments) {
    std::string out;
    out.reserve(source.size());
    bool block = false, line = false, escaped = false;
    char quote = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        char c = source[i];
        char next = i + 1 < source.size() ? source[i + 1] : 0;
        if (c == '\n') { line = false; out += '\n'; continue; }
        if (line) continue;
        if (block) { if (c == '*' && next == '/') { block = false; ++i; } continue; }
        if (quote) {
            out += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { quote = c; out += c; continue; }
        if (c == '/' && next == '*') { block = true; ++i; continue; }
        if ((hash_comments && c == '#') || (c == '/' && next == '/')) {
            line = true; if (c == '/') ++i; continue;
        }
        out += c;
    }
    return out;
}

// ASCII identifier boundaries are deliberate: UTF-8 words remain intact, while
// camelCase, snake_case and path segments become independently searchable.
inline std::vector<std::string> lexical_terms(const std::string& input) {
    std::vector<std::string> out;
    std::string word;
    auto flush = [&] {
        if (word.size() >= 2) out.push_back(word);
        word.clear();
    };
    unsigned char previous = 0;
    for (unsigned char ch : input) {
        bool letter = std::isalnum(ch) || ch >= 128;
        if (!letter) { flush(); previous = 0; continue; }
        if (!word.empty() && std::islower(previous) && std::isupper(ch)) flush();
        word.push_back(static_cast<char>(ch < 128 ? std::tolower(ch) : ch));
        previous = ch;
    }
    flush();
    return out;
}

inline std::string lexical_document(const std::string& input) {
    std::string valid;
    valid.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        auto c = static_cast<unsigned char>(input[i]);
        size_t width = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF ? 2 :
                        c >= 0xE0 && c <= 0xEF ? 3 :
                        c >= 0xF0 && c <= 0xF4 ? 4 : 0);
        if (width == 0 || i + width > input.size()) { valid += ' '; ++i; continue; }
        bool okay = true;
        for (size_t j = 1; j < width; ++j)
            if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) okay = false;
        if (okay && width >= 3) {
            auto next = static_cast<unsigned char>(input[i + 1]);
            if ((c == 0xE0 && next < 0xA0) || (c == 0xED && next >= 0xA0) ||
                (c == 0xF0 && next < 0x90) || (c == 0xF4 && next >= 0x90)) okay = false;
        }
        if (!okay) { valid += ' '; ++i; continue; }
        valid.append(input, i, width);
        i += width;
    }
    std::string out = " ";
    for (const auto& term : lexical_terms(valid)) {
        out += term;
        out += ' ';
    }
    return out;
}

inline std::vector<std::string> lexical_query_terms(const std::string& query) {
    static const std::unordered_map<std::string, std::vector<std::string>> aliases = {
        {"movimento", {"movement", "move"}}, {"velocidade", {"velocity"}},
        {"jogador", {"player"}}, {"câmera", {"camera"}},
        {"camera", {"camera"}}, {"segue", {"follow"}},
        {"seguir", {"follow"}}, {"segui", {"follow"}},
        {"zoom", {"zoom"}}, {"antecipa", {"anticipation", "follow"}},
        {"posição", {"position"}}, {"posicao", {"position"}},
        {"diálogo", {"dialogue"}}, {"dialogo", {"dialogue"}},
        {"escolhas", {"choose", "choice", "option"}},
        {"escolha", {"choose", "choice", "option"}},
        {"consequências", {"apply", "effect", "result"}},
        {"consequencia", {"apply", "effect"}},
        {"estado", {"state"}}, {"missão", {"mission"}},
        {"missao", {"mission"}}, {"salvo", {"save", "persist", "persistence"}},
        {"salva", {"save", "persist", "persistence"}},
        {"salvar", {"save", "persist", "persistence"}},
        {"restaurado", {"load", "persistence"}},
        {"carregamento", {"load", "persistence"}},
        {"retomada", {"resume"}}, {"retomado", {"resume"}},
        {"gravidade", {"gravity", "velocity", "compute"}}, {"rota", {"route"}},
        {"cancela", {"cancel"}}, {"vira", {"face"}},
        {"direção", {"direction"}}, {"direcao", {"direction"}},
        {"ajusta", {"apply", "adjust"}}, {"ajustado", {"apply", "adjust"}},
        {"opções", {"options"}}, {"opcao", {"option"}},
        {"conversa", {"dialogue"}}, {"pessoa", {"person"}},
        {"aberta", {"open"}}, {"abre", {"open"}},
        {"confirma", {"commit"}}, {"efeito", {"effect", "apply"}},
        {"persiste", {"persist"}}, {"persistido", {"persist"}},
        {"vazios", {"empty"}}, {"inválidos", {"corrupt"}},
        {"ajuste", {"adjust"}}, {"começa", {"begin"}},
        {"comeca", {"begin"}}, {"termina", {"end"}},
        {"escrita", {"write", "save"}},
        {"armazenamento", {"storage", "save"}},
        {"bloqueado", {"locked"}}, {"exportado", {"export"}},
    };
    static const std::unordered_set<std::string> stop = {
        "como", "que", "quando", "qual", "quais", "uma", "para", "com", "por",
        "dos", "das", "seu", "sua", "são", "sao", "entre", "após", "apos",
        "the", "how", "and", "does", "with", "from", "into"
    };
    auto raw = lexical_terms(query);
    std::vector<std::string> out;
    for (const auto& term : raw) {
        if (stop.count(term)) continue;
        out.push_back(term);
        auto it = aliases.find(term);
        if (it != aliases.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    }
    return out;
}

} // namespace axon
