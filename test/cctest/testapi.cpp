/*
 * Copyright (c) 2017-present Samsung Electronics Co., Ltd
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 */

#include "api/EscargotPublic.h"

using namespace Escargot;

#include "gtest/gtest.h"

#include <vector>

static bool stringEndsWith(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() && 0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

static const char32_t offsetsFromUTF8[6] = { 0x00000000UL, 0x00003080UL, 0x000E2080UL, 0x03C82080UL, static_cast<char32_t>(0xFA082080UL), static_cast<char32_t>(0x82082080UL) };

char32_t readUTF8Sequence(const char*& sequence, bool& valid, int& charlen)
{
    unsigned length;
    const char sch = *sequence;
    valid = true;
    if ((sch & 0x80) == 0)
        length = 1;
    else {
        unsigned char ch2 = static_cast<unsigned char>(*(sequence + 1));
        if ((sch & 0xE0) == 0xC0
            && (ch2 & 0xC0) == 0x80)
            length = 2;
        else {
            unsigned char ch3 = static_cast<unsigned char>(*(sequence + 2));
            if ((sch & 0xF0) == 0xE0
                && (ch2 & 0xC0) == 0x80
                && (ch3 & 0xC0) == 0x80)
                length = 3;
            else {
                unsigned char ch4 = static_cast<unsigned char>(*(sequence + 3));
                if ((sch & 0xF8) == 0xF0
                    && (ch2 & 0xC0) == 0x80
                    && (ch3 & 0xC0) == 0x80
                    && (ch4 & 0xC0) == 0x80)
                    length = 4;
                else {
                    valid = false;
                    sequence++;
                    return -1;
                }
            }
        }
    }

    charlen = length;
    char32_t ch = 0;
    switch (length) {
    case 4:
        ch += static_cast<unsigned char>(*sequence++);
        ch <<= 6; // Fall through.
    case 3:
        ch += static_cast<unsigned char>(*sequence++);
        ch <<= 6; // Fall through.
    case 2:
        ch += static_cast<unsigned char>(*sequence++);
        ch <<= 6; // Fall through.
    case 1:
        ch += static_cast<unsigned char>(*sequence++);
    }
    return ch - offsetsFromUTF8[length - 1];
}

static std::string evalScript(ContextRef* context, StringRef* str, StringRef* fileName, bool isModule)
{
    if (stringEndsWith(fileName->toStdUTF8String(), "mjs")) {
        isModule = isModule || true;
    }

    auto scriptInitializeResult = context->scriptParser()->initializeScript(str, fileName, isModule);
    if (!scriptInitializeResult.script) {
        std::string result("Script parsing error: ");
        switch (scriptInitializeResult.parseErrorCode) {
        case Escargot::ErrorObjectRef::Code::SyntaxError:
            result += "SyntaxError";
            break;
        case Escargot::ErrorObjectRef::Code::EvalError:
            result += "EvalError";
            break;
        case Escargot::ErrorObjectRef::Code::RangeError:
            result += "RangeError";
            break;
        case Escargot::ErrorObjectRef::Code::ReferenceError:
            result += "ReferenceError";
            break;
        case Escargot::ErrorObjectRef::Code::TypeError:
            result += "TypeError";
            break;
        case Escargot::ErrorObjectRef::Code::URIError:
            result += "URIError";
            break;
        default:
            break;
        }
        char str[256];
        snprintf(str, sizeof(str), ": %s\n", scriptInitializeResult.parseErrorMessage->toStdUTF8String().data());
        result += str;
        return result;
    }

    std::string result;
    auto evalResult = Evaluator::execute(context, [](ExecutionStateRef* state, ScriptRef* script) -> ValueRef* {
        return script->execute(state);
    },
                                         scriptInitializeResult.script.get());

    char str[256];
    if (!evalResult.isSuccessful()) {
        snprintf(str, sizeof(str), "Uncaught %s:\n", evalResult.resultOrErrorToString(context)->toStdUTF8String().data());
        result += str;
        for (size_t i = 0; i < evalResult.stackTraceData.size(); i++) {
            snprintf(str, sizeof(str), "%s (%d:%d)\n", evalResult.stackTraceData[i].src->toStdUTF8String().data(), (int)evalResult.stackTraceData[i].loc.line, (int)evalResult.stackTraceData[i].loc.column);
            result += str;
        }
        return result;
    }

    result += evalResult.resultOrErrorToString(context)->toStdUTF8String();

    while (context->vmInstance()->hasPendingPromiseJob()) {
        context->vmInstance()->executePendingPromiseJob();
    }
    return result;
}

static OptionalRef<StringRef> builtinHelperFileRead(OptionalRef<ExecutionStateRef> state, const char* fileName, const char* builtinName)
{
    FILE* fp = fopen(fileName, "r");
    if (fp) {
        StringRef* src = StringRef::emptyString();
        std::string utf8Str;
        std::basic_string<unsigned char, std::char_traits<unsigned char>> str;
        char buf[512];
        bool hasNonLatin1Content = false;
        size_t readLen;
        while ((readLen = fread(buf, 1, sizeof buf, fp))) {
            if (!hasNonLatin1Content) {
                const char* source = buf;
                int charlen;
                bool valid;
                while (source < buf + readLen) {
                    char32_t ch = readUTF8Sequence(source, valid, charlen);
                    if (ch > 255) {
                        hasNonLatin1Content = true;
                        fseek(fp, 0, SEEK_SET);
                        break;
                    } else {
                        str += (unsigned char)ch;
                    }
                }
            } else {
                utf8Str.append(buf, readLen);
            }
        }
        fclose(fp);
        if (StringRef::isCompressibleStringEnabled()) {
            if (state) {
                if (hasNonLatin1Content) {
                    src = StringRef::createFromUTF8ToCompressibleString(state->context(), utf8Str.data(), utf8Str.length());
                } else {
                    src = StringRef::createFromLatin1ToCompressibleString(state->context(), str.data(), str.length());
                }
            } else {
                if (hasNonLatin1Content) {
                    src = StringRef::createFromUTF8(utf8Str.data(), utf8Str.length());
                } else {
                    src = StringRef::createFromLatin1(str.data(), str.length());
                }
            }
        } else {
            if (hasNonLatin1Content) {
                src = StringRef::createFromUTF8(utf8Str.data(), utf8Str.length());
            } else {
                src = StringRef::createFromLatin1(str.data(), str.length());
            }
        }
        return src;
    } else {
        char msg[1024];
        snprintf(msg, sizeof(msg), "GlobalObject.%s: cannot open file %s", builtinName, fileName);
        if (state) {
            state->throwException(URIErrorObjectRef::create(state.get(), StringRef::createFromUTF8(msg, strnlen(msg, sizeof msg))));
        } else {
            puts(msg);
        }
        return nullptr;
    }
}

class ShellPlatform : public PlatformRef {
public:
    virtual void didPromiseJobEnqueued(ContextRef* relatedContext, PromiseObjectRef* obj) override
    {
        // ignore. we always check pending job after eval script
    }

    static std::string dirnameOf(const std::string& fname)
    {
        size_t pos = fname.find_last_of("/");
        if (std::string::npos == pos) {
            pos = fname.find_last_of("\\/");
        }
        return (std::string::npos == pos)
            ? ""
            : fname.substr(0, pos);
    }

    static std::string absolutePath(const std::string& referrerPath, const std::string& src)
    {
        std::string utf8MayRelativePath = dirnameOf(referrerPath) + "/" + src;
        auto absPath = realpath(utf8MayRelativePath.data(), nullptr);
        if (!absPath) {
            return std::string();
        }
        std::string utf8AbsolutePath = absPath ? absPath : "";
        free(absPath);

        return utf8AbsolutePath;
    }

    static std::string absolutePath(const std::string& src)
    {
        auto absPath = realpath(src.data(), nullptr);
        std::string utf8AbsolutePath = absPath;
        free(absPath);

        return utf8AbsolutePath;
    }

    std::vector<std::tuple<std::string /* abs path */, ContextRef*, PersistentRefHolder<ScriptRef>>> loadedModules;
    virtual LoadModuleResult onLoadModule(ContextRef* relatedContext, ScriptRef* whereRequestFrom, StringRef* moduleSrc) override
    {
        std::string referrerPath = whereRequestFrom->src()->toStdUTF8String();

        for (size_t i = 0; i < loadedModules.size(); i++) {
            if (std::get<2>(loadedModules[i]) == whereRequestFrom) {
                referrerPath = std::get<0>(loadedModules[i]);
                break;
            }
        }

        std::string absPath = absolutePath(referrerPath, moduleSrc->toStdUTF8String());
        if (absPath.length() == 0) {
            std::string s = "Error reading : " + moduleSrc->toStdUTF8String();
            return LoadModuleResult(ErrorObjectRef::Code::None, StringRef::createFromUTF8(s.data(), s.length()));
        }

        for (size_t i = 0; i < loadedModules.size(); i++) {
            if (std::get<0>(loadedModules[i]) == absPath && std::get<1>(loadedModules[i]) == relatedContext) {
                return LoadModuleResult(std::get<2>(loadedModules[i]));
            }
        }

        OptionalRef<StringRef> source = builtinHelperFileRead(nullptr, absPath.data(), "");
        if (!source) {
            std::string s = "Error reading : " + absPath;
            return LoadModuleResult(ErrorObjectRef::Code::None, StringRef::createFromUTF8(s.data(), s.length()));
        }

        auto parseResult = relatedContext->scriptParser()->initializeScript(source.value(), StringRef::createFromUTF8(absPath.data(), absPath.size()), true);
        if (!parseResult.isSuccessful()) {
            return LoadModuleResult(parseResult.parseErrorCode, parseResult.parseErrorMessage);
        }

        return LoadModuleResult(parseResult.script.get());
    }

    virtual void didLoadModule(ContextRef* relatedContext, OptionalRef<ScriptRef> referrer, ScriptRef* loadedModule) override
    {
        std::string path;
        if (referrer && loadedModule->src()->length() && loadedModule->src()->charAt(0) != '/') {
            path = absolutePath(referrer->src()->toStdUTF8String(), loadedModule->src()->toStdUTF8String());
        } else {
            path = absolutePath(loadedModule->src()->toStdUTF8String());
        }
        loadedModules.push_back(std::make_tuple(path, relatedContext, PersistentRefHolder<ScriptRef>(loadedModule)));
    }
};

PersistentRefHolder<VMInstanceRef> g_instance;
PersistentRefHolder<ContextRef> g_context;

int main(int argc, char* argv[])
{
    testing::InitGoogleTest(&argc, argv);

    Globals::initialize();

    Memory::setGCFrequency(24);

    ShellPlatform* platform = new ShellPlatform();
    PersistentRefHolder<VMInstanceRef> g_instance = VMInstanceRef::create(platform);
    g_instance->setOnVMInstanceDelete([](VMInstanceRef* instance) {
        delete instance->platform();
    });

    g_context = ContextRef::create(g_instance.get());

    return RUN_ALL_TESTS();
}

TEST(EvalScript, Run) {
    auto s = evalScript(g_context.get(), StringRef::createFromASCII("1 + 1"), StringRef::createFromASCII("test.js"), false);
    EXPECT_EQ(s, "2");
}

TEST(EvalScript, Run2) {
    auto s = evalScript(g_context.get(), StringRef::createFromASCII("'1' - 1"), StringRef::createFromASCII("test.js"), false);
    EXPECT_EQ(s, "0");
}

TEST(EvalScript, ParseError) {
    auto s = evalScript(g_context.get(), StringRef::createFromASCII("."), StringRef::createFromASCII("test.js"), false);
    EXPECT_TRUE(s.find("SyntaxError") != std::string::npos);
}

TEST(EvalScript, RuntimeError) {
    auto s = evalScript(g_context.get(), StringRef::createFromASCII("throw 1"), StringRef::createFromASCII("test.js"), false);
    EXPECT_TRUE(s.find("Uncaught 1") == 0);
}

TEST(ObjectTemplate, Basic1) {
    ObjectTemplateRef* tpl = ObjectTemplateRef::create();
    tpl->set(StringRef::createFromASCII("asdf"), StringRef::createFromASCII("asdfData"), false, false, false);

    ObjectTemplateRef* another = ObjectTemplateRef::create();
    tpl->set(StringRef::createFromASCII("another"), another, false, false, false);


    ObjectRef* obj = tpl->instantiate(g_context.get());

    Evaluator::execute(g_context.get(), [](ExecutionStateRef* state, ObjectRef* obj) -> ValueRef* {
        auto desc = obj->getOwnPropertyDescriptor(state, StringRef::createFromASCII("asdf"));
        auto value = desc->asObject()->get(state, StringRef::createFromASCII("value"));
        EXPECT_TRUE(value->asString()->equalsWithASCIIString("asdfData", 8));

        value = desc->asObject()->get(state, StringRef::createFromASCII("writable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("enumerable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("configurable"));
        EXPECT_TRUE(value->isFalse());

        desc = obj->getOwnPropertyDescriptor(state, StringRef::createFromASCII("another"));
        EXPECT_TRUE(desc->asObject()->get(state, StringRef::createFromASCII("value"))->isObject());

        value = desc->asObject()->get(state, StringRef::createFromASCII("writable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("enumerable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("configurable"));
        EXPECT_TRUE(value->isFalse());

        return ValueRef::createUndefined();
    }, obj);
}

TEST(ObjectTemplate, Basic2) {
    ObjectTemplateRef* tpl = ObjectTemplateRef::create();

    auto getter = FunctionTemplateRef::create(AtomicStringRef::emptyAtomicString(), 1, true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        return ValueRef::create(12);
    }, nullptr);

    tpl->setAccessorProperty(StringRef::createFromASCII("asdf"), getter, nullptr, false, true);

    auto getter2 = FunctionTemplateRef::create(AtomicStringRef::emptyAtomicString(), 1, true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        return (ValueRef*)thisValue->asObject()->extraData();
    }, nullptr);
    auto setter = FunctionTemplateRef::create(AtomicStringRef::emptyAtomicString(), 1, true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        thisValue->asObject()->setExtraData(argv[0]);
        return ValueRef::createUndefined();
    }, nullptr);
    tpl->setAccessorProperty(StringRef::createFromASCII("asdf2"), getter2, setter, false, true);

    ObjectRef* obj = tpl->instantiate(g_context.get());

    Evaluator::execute(g_context.get(), [](ExecutionStateRef* state, ObjectRef* obj) -> ValueRef* {
        auto desc = obj->getOwnPropertyDescriptor(state, StringRef::createFromASCII("asdf"));

        auto value = desc->asObject()->get(state, StringRef::createFromASCII("enumerable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("configurable"));
        EXPECT_TRUE(value->isTrue());

        value = desc->asObject()->get(state, StringRef::createFromASCII("get"));
        EXPECT_TRUE(value->isFunctionObject());

        value = desc->asObject()->get(state, StringRef::createFromASCII("set"));
        EXPECT_TRUE(value->isUndefined());

        EXPECT_TRUE(obj->get(state, StringRef::createFromASCII("asdf"))->equalsTo(state, ValueRef::create(12)));

        obj->set(state, StringRef::createFromASCII("asdf2"), StringRef::createFromASCII("test"));
        EXPECT_TRUE(obj->get(state, StringRef::createFromASCII("asdf2"))->equalsTo(state, StringRef::createFromASCII("test")));

        return ValueRef::createUndefined();
    }, obj);
}

TEST(ObjectTemplate, Basic3) {
    ObjectTemplateRef* tpl = ObjectTemplateRef::create();

    class TestNativeDataAccessorPropertyData : public ObjectRef::NativeDataAccessorPropertyData {
    public:
        TestNativeDataAccessorPropertyData()
        : ObjectRef::NativeDataAccessorPropertyData(false, false, false, nullptr, nullptr)
        {
            number = 10;
        }
        double number;
    };

    TestNativeDataAccessorPropertyData* data = new TestNativeDataAccessorPropertyData();
    data->m_isConfigurable = false;
    data->m_isEnumerable = false;
    data->m_isWritable = true;
    data->m_getter = [](ExecutionStateRef* state, ObjectRef* self, ObjectRef::NativeDataAccessorPropertyData* data) -> ValueRef* {
        return ValueRef::create(((TestNativeDataAccessorPropertyData*)data)->number);
    };

    data->m_setter = [](ExecutionStateRef* state, ObjectRef* self, ObjectRef::NativeDataAccessorPropertyData* data, ValueRef* setterInputData) -> bool {
        ((TestNativeDataAccessorPropertyData*)data)->number = setterInputData->toNumber(state);
        return true;
    };

    tpl->setNativeDataAccessorProperty(StringRef::createFromASCII("asdf"), data);

    ObjectRef* obj = tpl->instantiate(g_context.get());

    Evaluator::execute(g_context.get(), [](ExecutionStateRef* state, ObjectRef* obj) -> ValueRef* {
        auto desc = obj->getOwnPropertyDescriptor(state, StringRef::createFromASCII("asdf"));

        auto value = desc->asObject()->get(state, StringRef::createFromASCII("enumerable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("configurable"));
        EXPECT_TRUE(value->isFalse());

        value = desc->asObject()->get(state, StringRef::createFromASCII("writable"));
        EXPECT_TRUE(value->isTrue());

        obj->set(state, StringRef::createFromASCII("asdf"), ValueRef::create(20));

        EXPECT_TRUE(obj->get(state, StringRef::createFromASCII("asdf"))->equalsTo(state, ValueRef::create(20)));

        return ValueRef::createUndefined();
    }, obj);
}

TEST(FunctionTemplate, Basic1) {
    auto ft = FunctionTemplateRef::create(AtomicStringRef::create(g_context.get(), "asdf"), 2, true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        EXPECT_TRUE(argc == 1);
        return argv[0];
    }, nullptr);

    FunctionObjectRef* fn = ft->instantiate(g_context.get())->asFunctionObject();

    // same instance on same context
    EXPECT_EQ(fn, ft->instantiate(g_context.get())->asFunctionObject());

    Evaluator::execute(g_context.get(), [](ExecutionStateRef* state, FunctionObjectRef* fn) -> ValueRef* {
        ValueRef* arr[1] = { ValueRef::create(123) };
        EXPECT_TRUE(fn->call(state, ValueRef::createUndefined(), 1, arr)->equalsTo(state, ValueRef::create(123)));
        return ValueRef::createUndefined();
    }, fn);
}

TEST(FunctionTemplate, Basic2) {
    auto ft = FunctionTemplateRef::create(AtomicStringRef::create(g_context.get(), "parent"), 0,
        true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        return ValueRef::createUndefined();
    }, nullptr);
    ft->prototypeTemplate()->set(StringRef::createFromASCII("asdf1"), ValueRef::create(1), true, true, true);

    auto ftchildobj = ObjectTemplateRef::create();
    ftchildobj->set(StringRef::createFromASCII("asdf"), ValueRef::create(0), true, true, true);
    auto ftchild = FunctionTemplateRef::create(AtomicStringRef::create(g_context.get(), "asdf"), 2,
        true, true, [](ExecutionStateRef* state, ValueRef* thisValue, size_t argc, ValueRef** argv, bool isConstructCall) -> ValueRef* {
        return ValueRef::create(123);
    }, ftchildobj);

    ftchild->prototypeTemplate()->set(StringRef::createFromASCII("asdf2"), ValueRef::create(2), true, true, true);
    ftchild->inherit(ft);

    Evaluator::execute(g_context.get(), [](ExecutionStateRef* state, FunctionTemplateRef* ftchild) -> ValueRef* {
        ObjectRef* ref = ftchild->instantiate(g_context.get())->construct(state, 0, 0);

        EXPECT_TRUE(ref->get(state, StringRef::createFromASCII("asdf"))->equalsTo(state, ValueRef::create(0)));
        EXPECT_TRUE(ref->hasOwnProperty(state, StringRef::createFromASCII("asdf")));
        EXPECT_TRUE(ref->get(state, StringRef::createFromASCII("asdf1"))->equalsTo(state, ValueRef::create(1)));
        EXPECT_TRUE(ref->get(state, StringRef::createFromASCII("asdf2"))->equalsTo(state, ValueRef::create(2)));

        return ValueRef::createUndefined();
    }, ftchild);
}

