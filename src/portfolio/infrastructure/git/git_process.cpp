#include "git_process.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace axon::portfolio::git {
namespace {

std::string path_as_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
    return path.string();
#endif
}

std::vector<std::string> command_arguments(const std::filesystem::path& root,
                                           const std::vector<std::string>& arguments) {
    if (root.empty() || arguments.empty()) {
        throw std::invalid_argument("Git command requires a repository root and arguments");
    }
    // Replacement refs are local overlays and would make a displayed object id
    // identify different bytes. The declaration importer therefore always sees
    // canonical Git objects, never a repository-local replacement view.
    std::vector<std::string> result{"git", "--no-replace-objects", "-C", path_as_utf8(root)};
    result.insert(result.end(), arguments.begin(), arguments.end());
    return result;
}

void append_bounded(CommandResult& result, const char* bytes, std::size_t count,
                    const std::size_t maximum_output_bytes) {
    const auto available = result.stdout_text.size() < maximum_output_bytes
                               ? maximum_output_bytes - result.stdout_text.size()
                               : 0U;
    const auto copied = std::min(available, count);
    result.stdout_text.append(bytes, copied);
    result.output_truncated = result.output_truncated || copied != count;
}

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw std::invalid_argument("Git argument is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::wstring quote_windows_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
            result.append(backslashes * 2U + 1U, L'\\');
        else
            result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}
#endif

} // namespace

CommandResult run(const std::filesystem::path& repository_root,
                  const std::vector<std::string>& arguments,
                  const std::size_t maximum_output_bytes) {
    if (maximum_output_bytes == 0U)
        throw std::invalid_argument("Git output limit must be positive");
    const auto argv = command_arguments(repository_root, arguments);
    CommandResult result;

#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0) ||
        !SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0)) {
        if (read_handle) CloseHandle(read_handle);
        if (write_handle) CloseHandle(write_handle);
        throw std::runtime_error("unable to create Git output pipe");
    }
    std::wstring command_line;
    for (const auto& argument : argv) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += quote_windows_argument(utf8_to_wide(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_handle;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(read_handle);
        CloseHandle(write_handle);
        throw std::runtime_error("unable to start Git executable");
    }
    CloseHandle(write_handle);
    char buffer[8192];
    DWORD count = 0;
    while (ReadFile(read_handle, buffer, sizeof(buffer), &count, nullptr) && count != 0) {
        append_bounded(result, buffer, count, maximum_output_bytes);
    }
    CloseHandle(read_handle);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    int pipefd[2]{};
    if (pipe(pipefd) != 0) throw std::runtime_error("unable to create Git output pipe");
    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("unable to fork Git process");
    }
    if (child == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1U);
        for (const auto& argument : argv)
            exec_argv.push_back(const_cast<char*>(argument.c_str()));
        exec_argv.push_back(nullptr);
        execvp("git", exec_argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buffer[8192];
    ssize_t count = 0;
    while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        append_bounded(result, buffer, static_cast<std::size_t>(count), maximum_output_bytes);
    }
    close(pipefd[0]);
    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) throw std::runtime_error("unable to wait for Git process");
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
    return result;
}

} // namespace axon::portfolio::git
