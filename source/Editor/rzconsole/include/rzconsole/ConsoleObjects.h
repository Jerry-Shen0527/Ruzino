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

#pragma once

#include <string.h>

#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace console {

class Command;

//
// Base Console Object
//

class Object {
   public:
    virtual ~Object()
    {
    }

    std::string const& GetName() const;

    std::string const& GetDescription() const
    {
        return m_Description;
    }
    void SetDescription(std::string const& description)
    {
        m_Description = description;
    }

    virtual Command* AsCommand()
    {
        return nullptr;
    }

   protected:
    friend class ObjectDictionary;

    Object(char const* description) : m_Description(description)
    {
    }

    std::string m_Description;
};

//
// Console Commands
//

class Command : public Object {
   public:
    virtual Command* AsCommand() override
    {
        return this;
    }

    // execution callback

    struct Result {
        bool status = false;
        std::string output;
    };

    typedef std::vector<std::string> Args;
    typedef std::function<Result(Args const& args)> OnExecuteFunction;

    Result Execute(Args const& args);

    // optional callback to suggest argument values

    typedef std::function<std::vector<std::string>(
        std::string_view const cmdline,
        size_t cursor_pos)>
        OnSuggestFunction;

    std::vector<std::string> Suggest(
        std::string_view const cmdline,
        size_t cursor_pos);

   private:
    friend class ObjectDictionary;

    Command(
        char const* description,
        OnExecuteFunction on_exec,
        OnSuggestFunction on_suggest);

    OnExecuteFunction m_OnExecute;
    OnSuggestFunction m_OnSuggest;
};

//
// Object functions
//

struct CommandDesc {
    char const* name = nullptr;
    char const* description = nullptr;
    Command::OnExecuteFunction on_execute;
    Command::OnSuggestFunction on_suggest;
};

RZCONSOLE_API bool RegisterCommand(CommandDesc const& desc);

RZCONSOLE_API bool UnregisterCommand(std::string_view name);

Object* FindObject(std::string_view name);

std::vector<std::string_view> MatchObjectNames(char const* regex = ".*");

std::vector<Object*> MatchObjects(char const* regex = ".*");

RZCONSOLE_API Command* FindCommand(std::string_view name);

// note: ini files can only modify values of existing consolve variables
void ParseIniFile(char const* inidata, char const* filename);

// nuclear option: removes all console objects from dictionary
void ResetAll();

}  // end namespace console

RUZINO_NAMESPACE_CLOSE_SCOPE
