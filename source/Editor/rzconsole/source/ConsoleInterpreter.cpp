/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <rzconsole/ConsoleInterpreter.h>
#include <rzconsole/ConsoleObjects.h>
#include <rzconsole/string_utils.h>
#include <spdlog/spdlog.h>

#include <cassert>

RUZINO_NAMESPACE_OPEN_SCOPE
namespace console {
//
// Lexer
//

class Lexer {
   public:
    Lexer(std::string_view stream);

    bool hasNextToken()
    {
        return !m_Eof;
    }

    Token nextToken();

    std::string const& getErrorString() const;

   private:
    void advance();

    void parseSpace();

    Token parseToken();

   private:
    enum class Error {
        NONE = 0,
        MISSING_QUOTE_ENDING,
        MISSING_ESCAPED_CHARACTER,
        UNEXPECTED_STRING_ENDING,
        READING_PAST_END,
    } m_Error = Error::NONE;

    char m_Next = 0;
    bool m_Eof = false;
    std::string_view m_Stream;
};

Lexer::Lexer(std::string_view stream) : m_Stream(stream)
{
    if (!stream.empty())
        advance();
    parseSpace();
}

Token Lexer::nextToken()
{
    if (!m_Eof)
        return parseToken();

    m_Error = Error::READING_PAST_END;
    return Token();
}

std::string const& Lexer::getErrorString() const
{
    static std::string errs[] = {
        "unexpected lexer error",
        "unexpected end of stream after escape character",
        "missing closing quote",
        "characters after quote ending"
        "unexpected end of stream",
    };

    switch (m_Error) {
        case Error::MISSING_ESCAPED_CHARACTER: return errs[0];
        case Error::MISSING_QUOTE_ENDING: return errs[1];
        case Error::UNEXPECTED_STRING_ENDING: return errs[2];
        case Error::READING_PAST_END: return errs[3];
        default: return errs[0];
    }
}

void Lexer::advance()
{
    if (!m_Stream.empty()) {
        m_Next = m_Stream.front();
        m_Stream.remove_prefix(1);
    }
    else
        m_Eof = true;
}

void Lexer::parseSpace()
{
    while (!m_Eof && std::isspace(m_Next))
        advance();
}

Token Lexer::parseToken()
{
    Token token;

    bool inString = true;
    bool inEscape = false;
    bool inQuotes = false;

    for (; !m_Eof && inString && !std::isspace(m_Next); advance()) {
        if (inEscape) {
            token.value.push_back(m_Next);
            inEscape = false;
        }
        else {
            switch (m_Next) {
                case '\\': inEscape = true; break;
                case '\'':
                case '\"': inString = inQuotes = !inQuotes; break;
                default: token.value.push_back(m_Next);
            }
        }
    }

    if (!m_Eof && !std::isspace(m_Next))
        m_Error = Error::UNEXPECTED_STRING_ENDING;
    if (inEscape)
        m_Error = Error::MISSING_ESCAPED_CHARACTER;
    if (inQuotes)
        m_Error = Error::MISSING_QUOTE_ENDING;

    if (m_Error == Error::NONE) {
        token.type = TokenType::STRING;
        parseSpace();
        return token;
    }
    return Token();
}

//
// Interpreter implementation
//

typedef Command::Args Args;

static void initializeDefaultCommands();

Interpreter::Interpreter()
{
    initializeDefaultCommands();
}

Interpreter::Result Interpreter::Execute(std::string_view const cmdline)
{
    if (cmdline.empty())
        return { false };

    // Check if derived class wants to handle this command directly FIRST
    if (ShouldHandleCommand(cmdline)) {
        return HandleDirectExecution(cmdline);
    }

    // Super-simple parser
    Command::Args args;
    for (Lexer lexer(cmdline); lexer.hasNextToken();) {
        if (Token token = lexer.nextToken(); token.type != TokenType::INVALID)
            args.push_back(std::move(token.value));
        else {
            std::string err = "syntax error";
            err += args.empty() ? "" : " near token \"" + args.back() + '\"';
            err += " : " + lexer.getErrorString();
            spdlog::error(err.c_str());
            return { false };
        }
    }

    if (args.empty())
        return { false };

    // Try virtual command execution first
    if (IsValidCommand(args[0])) {
        return ExecuteCommand(args[0], args);
    }

    // Fall back to console command system
    if (auto* cobj = FindObject(args[0])) {
        if (auto* cmd = cobj->AsCommand()) {
            auto [status, output] = cmd->Execute(args);
            return { status, output };
        }
    }
    else {
        // If no console command found, let derived class try to handle it
        if (ShouldHandleCommand(cmdline)) {
            return HandleDirectExecution(cmdline);
        }

        spdlog::error(
            "no console object with name '{}' found", std::string(args[0]));
    }

    return { false };
}

std::vector<std::string> Interpreter::Suggest(
    std::string_view const cmdline,
    size_t cursor_pos)
{
    if (cmdline.empty() || (cursor_pos > cmdline.size()))
        return {};

    auto tokens = ds::split(cmdline);

    if (!tokens.empty()) {
        char const* token_start = tokens[0].data();
        char const* cursor = cmdline.data() + cursor_pos;
        char const* token_end = tokens[0].data() + tokens[0].size();
        if ((tokens.size() == 1) && (token_start <= cursor) &&
            (cursor <= token_end)) {
            // user is looking for a command
            auto names = MatchObjectNames(
                ("^" + std::string(token_start, cursor) + ".*").c_str());
            std::vector<std::string> suggestions(names.begin(), names.end());

            // Add virtual command suggestions
            auto virtual_suggestions = SuggestCommand("", cmdline, cursor_pos);
            suggestions.insert(
                suggestions.end(),
                virtual_suggestions.begin(),
                virtual_suggestions.end());

            return suggestions;
        }
        else {
            // user is looking for the command's arguments
            if (IsValidCommand(tokens[0])) {
                return SuggestCommand(tokens[0], cmdline, cursor_pos);
            }
            if (auto* cobj = FindCommand(tokens[0]))
                return cobj->Suggest(cmdline, cursor_pos);
        }
    }
    return {};
}

// Default virtual method implementations
Interpreter::Result Interpreter::ExecuteCommand(
    std::string_view command,
    const std::vector<std::string>& args)
{
    return { false, "Command not implemented" };
}

std::vector<std::string> Interpreter::SuggestCommand(
    std::string_view command,
    std::string_view cmdline,
    size_t cursor_pos)
{
    return {};
}

bool Interpreter::IsValidCommand(std::string_view command) const
{
    return false;
}

bool Interpreter::ShouldHandleCommand(std::string_view command) const
{
    return false;
}

Interpreter::Result Interpreter::HandleDirectExecution(std::string_view cmdline)
{
    return { false, "Direct execution not implemented" };
}

// Register various commands

static CommandDesc help_cmd = {
    // name
    "help",
    // description
    "usage: \n"
    "   help [name]\n"
    "       returns the description of console objects.\n"
    "   help --list [regex pattern]\n"
    "       returns a list of console objects matching the regex.\n",
    // on exec
    [](Command::Args const& args) -> Command::Result {
        if (args.size() >= 2) {
            if (args[1] == "--list") {
                Command::Result r;
                for (auto name : MatchObjectNames(
                         args.size() > 2 ? std::string(args[2]).c_str()
                                         : ".*")) {
                    r.output += name;
                    r.output += '\n';
                }
                r.status = true;
                return r;
            }
            else {
                if (auto cobj = FindObject(args[1]))
                    return { true, cobj->GetDescription() };
                else
                    return { false,
                             std::string("no console object with name '") +
                                 std::string(args[1]) + "' found" };
            }
        }
        else
            return { true, help_cmd.description };
    },
    // on suggest
    [](std::string_view cmdline,
       size_t cursor_pos) -> std::vector<std::string> {
        auto tokens = ds::split(cmdline);

        assert(tokens[0] == "help");

        char const* cursor = cmdline.data() + cursor_pos;

        for (auto& token : tokens) {
            if ((token.data() <= cursor) &&
                (cursor <= (token.data() + token.size()))) {
                auto names = MatchObjectNames(
                    ("^" + std::string(token.data(), cursor) + ".*").c_str());
                return { names.begin(), names.end() };
            }
        }
        return {};
    }
};

static void initializeDefaultCommands()
{
    static bool initialized = false;
    if (!initialized)
        for (auto const& cmd : { help_cmd })
            RegisterCommand(cmd);
    initialized = true;
}

}  // namespace console
RUZINO_NAMESPACE_CLOSE_SCOPE
